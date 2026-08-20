/* Tests for the output-report block walk.
 *
 * ⭐⭐ WHY THIS EXISTS. Every report the host sends to a bridged controller is
 * walked block by block and patched in place. It is the busiest code in the
 * project -- twenty-five reports a second, every second a controller is
 * bridged -- and a fault in it corrupts everything downstream SILENTLY: no
 * error, no log, just audio in the wrong field or a report the controller
 * quietly discards.
 *
 * ⛔ It is also the hardest thing here to notice by playing. A bad tone is
 * obvious; a mis-walked block is a controller that feels slightly wrong.
 *
 * ⚠️ WHAT THIS DOES NOT TEST. The real patcher needs settings, logging and a
 * controller object, so the WALK is mirrored here rather than included. The
 * last test guards that by reading the shipped source.
 *
 * ➡️ Run with ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ── the walk, mirrored from ds5_patch_output ────────────────────────── */

#define WALK_START   4      /* where blocks begin in a 0x36 report */

typedef struct { uint8_t id; size_t at; size_t payload_len; } found_t;

/* Returns how many blocks were found, and the highest byte index touched.
 *
 * ⭐ `touched` is the point of the exercise: a walk that reads past the buffer
 * is the failure that matters, and it cannot be seen from the outside. */
static int walk(const uint8_t *data, size_t limit,
                found_t *out, int max_out, size_t *touched)
{
    int n = 0;
    size_t pos = WALK_START;
    *touched = 0;
    while (pos + 2 <= limit) {
        uint8_t block_id = data[pos];
        size_t payload_len = data[pos + 1];
        size_t block_len = payload_len + 2;
        if (pos + 1 > *touched) *touched = pos + 1;
        if (block_id == 0 && payload_len == 0) break;
        if (block_len > limit - pos) break;
        if (n < max_out) {
            out[n].id = block_id;
            out[n].at = pos;
            out[n].payload_len = payload_len;
        }
        ++n;
        if (pos + block_len - 1 > *touched) *touched = pos + block_len - 1;
        pos += block_len;
    }
    return n;
}

/* ── harness ─────────────────────────────────────────────────────────── */

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* ── the tests ───────────────────────────────────────────────────────── */

/* A report shaped the way a real one is. */
static void test_finds_the_blocks(void)
{
    puts("a well-formed report is walked block by block");
    uint8_t rep[64];
    memset(rep, 0, sizeof(rep));
    rep[0] = 0x36;
    rep[4] = 0x90; rep[5] = 8;      /* state block, 8 bytes of payload */
    rep[14] = 0x91; rep[15] = 4;    /* another block after it */
    rep[20] = 0x00; rep[21] = 0;    /* terminator */

    found_t f[8];
    size_t touched = 0;
    int n = walk(rep, sizeof(rep), f, 8, &touched);

    ok(n == 2, "found both blocks");
    ok(f[0].id == 0x90 && f[0].at == 4 && f[0].payload_len == 8, "the first is where it should be");
    ok(f[1].id == 0x91 && f[1].at == 14, "the second follows its predecessor's length");
    ok(touched < sizeof(rep), "nothing was read past the report");
}

/* ⭐⭐ THE ONE THAT MATTERS: a block claiming more than the report holds. */
static void test_a_lying_length_cannot_overrun(void)
{
    puts("a block that claims more than the report holds is refused");
    uint8_t rep[32];
    memset(rep, 0, sizeof(rep));
    rep[0] = 0x36;
    rep[4] = 0x90;
    rep[5] = 200;                   /* ⛔ far longer than the buffer */

    found_t f[8];
    size_t touched = 0;
    int n = walk(rep, sizeof(rep), f, 8, &touched);

    ok(n == 0, "the block is rejected rather than trusted");
    ok(touched < sizeof(rep), "and nothing was read past the report");
}

/* A final block cut off by the end of the report. */
static void test_a_truncated_last_block_is_refused(void)
{
    puts("a block cut off by the end of the report is refused");
    uint8_t rep[20];
    memset(rep, 0, sizeof(rep));
    rep[0] = 0x36;
    rep[4] = 0x90; rep[5] = 4;      /* fine: ends at 9 */
    rep[10] = 0x91; rep[11] = 40;   /* ⛔ would run past 20 */

    found_t f[8];
    size_t touched = 0;
    int n = walk(rep, sizeof(rep), f, 8, &touched);

    ok(n == 1, "the good block is taken and the truncated one is not");
    ok(touched < sizeof(rep), "and nothing was read past the report");
}

/* ⛔ A walk that fails to advance runs forever, at 25 reports a second. */
static void test_the_walk_always_advances(void)
{
    puts("the walk always moves forward");
    uint8_t rep[24];
    memset(rep, 0, sizeof(rep));
    rep[0] = 0x36;
    for (size_t i = 4; i + 1 < sizeof(rep); i += 2) {
        rep[i] = 0x90;
        rep[i + 1] = 0;             /* zero-length payloads, back to back */
    }
    found_t f[16];
    size_t touched = 0;
    int n = walk(rep, sizeof(rep), f, 16, &touched);
    ok(n > 0 && n <= 16, "a zero-length block still advances -- it cannot loop");
    ok(touched < sizeof(rep), "and nothing was read past the report");
}

/* An empty or nonsense report should simply find nothing. */
static void test_nothing_is_not_a_crash(void)
{
    puts("an empty report finds nothing and touches nothing");
    uint8_t rep[16];
    memset(rep, 0, sizeof(rep));
    rep[0] = 0x36;
    found_t f[4];
    size_t touched = 0;
    ok(walk(rep, sizeof(rep), f, 4, &touched) == 0, "no blocks");
    ok(walk(rep, 4, f, 4, &touched) == 0, "a report shorter than one block is safe");
    ok(walk(rep, 0, f, 4, &touched) == 0, "a zero-length report is safe");
}

/* Stripping the lightbar claim must touch that bit and no other.
 *
 * ⛔ The bit next to it is the PLAYER LEDS and the one below is POWER SAVE,
 * which can switch a controller off. A careless mask reaches both. */
static void test_stripping_the_lightbar_leaves_its_neighbours(void)
{
    puts("withholding the lightbar claim touches nothing else");
    uint8_t flags2 = 0x14 | 0x02;   /* lightbar, player LEDs, power save */
    uint8_t after = (uint8_t)(flags2 & ~0x04);
    ok((after & 0x04) == 0, "the lightbar claim is gone");
    ok((after & 0x10) != 0, "the player-LED claim is untouched");
    ok((after & 0x02) != 0, "the power-save claim is untouched");
}

/* ⭐⭐ THE GUARD ON THE MIRROR. */
static void test_mirrors_the_real_source(void)
{
    puts("the shipped walk still matches this copy");
    const char *paths[] = {
        "src/controllers/controller_ds5.c",
        "../src/controllers/controller_ds5.c",
        "ctm-bridge-webos/src/controllers/controller_ds5.c",
    };
    FILE *fp = NULL;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]) && !fp; ++i)
        fp = fopen(paths[i], "r");
    if (!fp) {
        ok(0, "found controller_ds5.c -- run this from the repo root");
        return;
    }
    char line[512];
    int guard = 0, terminator = 0, strip = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "block_len > limit - pos"))          guard = 1;
        if (strstr(line, "block_id == 0 && payload_len == 0")) terminator = 1;
        if (strstr(line, "&= (uint8_t)~0x04"))                 strip = 1;
    }
    fclose(fp);
    ok(guard,      "the overrun guard is still there");
    ok(terminator, "the terminator check is still there");
    ok(strip,      "the lightbar strip still clears only bit 0x04");
}

int main(void)
{
    puts("");
    test_finds_the_blocks();                        puts("");
    test_a_lying_length_cannot_overrun();           puts("");
    test_a_truncated_last_block_is_refused();       puts("");
    test_the_walk_always_advances();                puts("");
    test_nothing_is_not_a_crash();                  puts("");
    test_stripping_the_lightbar_leaves_its_neighbours(); puts("");
    test_mirrors_the_real_source();                 puts("");
    printf("%d checks, %d failed\n\n", checks, failed);
    return failed ? 1 : 0;
}
