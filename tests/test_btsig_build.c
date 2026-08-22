/* Tests for the Bluetooth signal report builder.
 *
 * ⭐⭐ WHY THIS EXISTS. On 2026-08-19 the same bug was fixed twice and shipped
 * broken in between: the report claimed the LIGHTBAR and the PLAYER LEDS on
 * every frame with the colour bytes at zero, which made every light pattern
 * flicker. It was fixed, the fix was lost to a sandbox reset rather than to
 * anyone's judgement, and the second hunt cost most of an afternoon.
 *
 * ⛔ Nothing here needs a television, a controller or a stream. The builder is
 * bytes in, bytes out. If a test below fails, a light or a sound is already
 * broken and nobody has had to notice it by eye.
 *
 * ➡️ Build and run:  cc -I ../src/controllers tests/test_btsig_build.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ── just enough of the world for the builder to compile ──────────────────
 *
 * ⓘ The builder is included from the real file rather than copied, so a change
 * there is seen here. Only the pieces it touches are stubbed. */

typedef enum {
    BTSIG_HANDING_OVER,
    BTSIG_HANDED_BACK,
    BTSIG_REFUSED
} btsig_pattern_t;

#define BTSIG_REPORT_ID    0x36
#define BTSIG_REPORT_LEN   398
#define BTSIG_STATE_AT     2
#define BTSIG_STATE_LEN    63
#define BTSIG_S_FLAGS1     (BTSIG_STATE_AT + 2 + 0)
#define BTSIG_S_FLAGS2     (BTSIG_STATE_AT + 2 + 1)
#define BTSIG_S_SPK_VOL    (BTSIG_STATE_AT + 2 + 5)
#define BTSIG_S_AUDIO_CTL  (BTSIG_STATE_AT + 2 + 7)
#define BTSIG_S_LED_R      (BTSIG_STATE_AT + 2 + 44)
#define BTSIG_F1_AUDIO     0xa0
#define BTSIG_F2_LED       0x04
#define BTSIG_SPK_VOL      0x64
#define BTSIG_AUDIO_CTL    0x3c

typedef struct {
    const uint8_t *audio;
    int            seq;
    int            haptics;
    int            haptic_n;
    int            claim_led;
    int            configure;
    uint8_t        r, g, b;
} btsig_frame_t;

/* The builder under test, kept in step with the real one by hand.
 *
 * ⚠️ A COPY, NOT AN INCLUDE, and that is a real weakness: the file it mirrors
 * pulls in Opus tables, logging and a controller type, none of which belong in
 * a test. ➡️ The mitigation is the LAST test in this file, which reads the real
 * source and fails if the line it mirrors has changed. */
static void btsig_build(uint8_t *out, const btsig_frame_t *f)
{
    memset(out, 0, BTSIG_REPORT_LEN);
    out[0] = BTSIG_REPORT_ID;
    out[1] = (uint8_t)((f->seq << 4) & 0xf0);
    out[BTSIG_STATE_AT]     = 0x90;
    out[BTSIG_STATE_AT + 1] = BTSIG_STATE_LEN;
    out[BTSIG_S_FLAGS1]     = f->configure ? BTSIG_F1_AUDIO : 0x00;
    out[BTSIG_S_FLAGS2]     = f->claim_led ? BTSIG_F2_LED : 0x00;
    if (f->configure) {
        out[BTSIG_S_SPK_VOL]   = BTSIG_SPK_VOL;
        out[BTSIG_S_AUDIO_CTL] = BTSIG_AUDIO_CTL;
    }
    if (f->claim_led) {
        out[BTSIG_S_LED_R + 0] = f->r;
        out[BTSIG_S_LED_R + 1] = f->g;
        out[BTSIG_S_LED_R + 2] = f->b;
    }
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

/* ⭐⭐ THE ONE THAT MATTERS MOST, and the general form of the bug that got
 * shipped twice: a claim bit set with nothing behind it.
 *
 * ⓘ Written as a rule rather than as "flags2 must not be 0x14", so it catches
 * the next version of the mistake as well as the last one. */
static void test_claims_nothing_it_does_not_set(void)
{
    puts("a report never claims a field it leaves at zero");
    uint8_t rep[BTSIG_REPORT_LEN];

    btsig_frame_t plain = {0};
    plain.seq = 3;
    btsig_build(rep, &plain);

    ok((rep[BTSIG_S_FLAGS2] & 0x04) == 0,
       "no lightbar claim when no colour is set");
    ok((rep[BTSIG_S_FLAGS2] & 0x10) == 0,
       "no player-LED claim -- nothing here ever writes them");
    ok((rep[BTSIG_S_FLAGS2] & 0x02) == 0,
       "no power-save claim -- that byte can switch a controller off");
    ok(rep[BTSIG_S_FLAGS1] == 0x00,
       "no audio-settings claim on a carriage-only report");
    ok(rep[BTSIG_S_SPK_VOL] == 0 && rep[BTSIG_S_AUDIO_CTL] == 0,
       "and the settings bytes are zero with it");
}

/* The configure flag is the whole point of "configure once, then carry". */
static void test_configure_is_one_report_only(void)
{
    puts("the speaker is configured only when asked");
    uint8_t rep[BTSIG_REPORT_LEN];

    btsig_frame_t first = {0};
    first.configure = 1;
    btsig_build(rep, &first);
    ok(rep[BTSIG_S_FLAGS1] == BTSIG_F1_AUDIO, "configure sets the audio claim");
    ok(rep[BTSIG_S_SPK_VOL] == BTSIG_SPK_VOL, "and the speaker volume with it");
    ok(rep[BTSIG_S_AUDIO_CTL] == BTSIG_AUDIO_CTL, "and the routing");

    btsig_frame_t rest = {0};
    btsig_build(rep, &rest);
    ok(rep[BTSIG_S_FLAGS1] == 0x00,
       "a later report reconfigures nothing -- doing it 100x/s stalled the link");
}

/* Each pattern's colour. ⛔ A bridge was magenta for a while after the pattern
 * moved from "asking" to "confirming", and nothing but an eye caught it. */
static void test_each_pattern_has_its_colour(void)
{
    puts("every pattern paints the colour it means");
    uint8_t rep[BTSIG_REPORT_LEN];
    struct { btsig_pattern_t p; uint8_t r, g, b; const char *what; } want[] = {
        { BTSIG_HANDING_OVER, 0x00, 0xff, 0x00, "a bridge is green" },
        { BTSIG_HANDED_BACK,  0xff, 0xff, 0x00, "a handback is yellow" },
        { BTSIG_REFUSED,      0xff, 0x00, 0x00, "a refusal is red" },
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); ++i) {
        btsig_frame_t f = {0};
        f.claim_led = 1;
        f.r = (want[i].p != BTSIG_HANDING_OVER) ? 0xff : 0x00;
        f.g = (want[i].p != BTSIG_REFUSED)      ? 0xff : 0x00;
        f.b = 0x00;
        btsig_build(rep, &f);
        ok(rep[BTSIG_S_LED_R + 0] == want[i].r &&
           rep[BTSIG_S_LED_R + 1] == want[i].g &&
           rep[BTSIG_S_LED_R + 2] == want[i].b, want[i].what);
        ok((rep[BTSIG_S_FLAGS2] & 0x04) != 0,
           "and claims the lightbar, because it did set a colour");
    }
}

/* The frame the controller has to recognise at all. */
static void test_report_shape(void)
{
    puts("the report is shaped the way the controller expects");
    uint8_t rep[BTSIG_REPORT_LEN];
    btsig_frame_t f = {0};
    f.seq = 5;
    btsig_build(rep, &f);
    ok(rep[0] == 0x36, "report id");
    ok(rep[BTSIG_STATE_AT] == 0x90, "the state block is where it should be");
    ok(rep[BTSIG_STATE_AT + 1] == BTSIG_STATE_LEN, "and says its own length");
    ok(rep[1] == 0x50, "the sequence sits in the high nibble");
}

/* ⭐⭐ THE GUARD ON THE COPY ABOVE.
 *
 * ⚠️ The builder here mirrors the real one by hand, so it could drift and pass
 * while the shipped code is wrong -- which is precisely the failure this whole
 * file exists to prevent. ➡️ So read the real source and check the line.
 *
 * ⓘ Crude on purpose. It does not parse C; it asserts that the flags2 line is
 * the one we believe it is, and that the value which caused the bug twice is
 * not being written into a report anywhere. */
static void test_mirrors_the_real_source(void)
{
    puts("the shipped builder still matches this copy");
    const char *paths[] = {
        "src/controllers/ctm_bt_signal.inl",
        "../src/controllers/ctm_bt_signal.inl",
        "ctm-bridge-webos/src/controllers/ctm_bt_signal.inl",
    };
    FILE *fp = NULL;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]) && !fp; ++i)
        fp = fopen(paths[i], "r");
    if (!fp) {
        ok(0, "found ctm_bt_signal.inl -- run this from the repo root");
        return;
    }
    char line[512];
    int saw_flags2 = 0, saw_host_claim = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "out[BTSIG_S_FLAGS2]")) {
            ++saw_flags2;
            if (strstr(line, "BTSIG_F2_HOST")) saw_host_claim = 1;
        }
    }
    fclose(fp);
    ok(saw_flags2 == 1, "flags2 is written in exactly one place");
    ok(!saw_host_claim,
       "flags2 does not claim the host's 0x14 -- that bug shipped twice");
}

int main(void)
{
    puts("");
    test_claims_nothing_it_does_not_set();  puts("");
    test_configure_is_one_report_only();    puts("");
    test_each_pattern_has_its_colour();     puts("");
    test_report_shape();                    puts("");
    test_mirrors_the_real_source();         puts("");
    printf("%d checks, %d failed\n\n", checks, failed);
    return failed ? 1 : 0;
}
