/* Tests for the card-match cache: a remembered card is believed only while it
 * is still the device it was written for.
 *
 * ⭐⭐ WHY THIS EXISTS. On 2026-09-01 a controller was released and bridged
 * again, and its confirmation tone played on the OTHER controller. The cache
 * had remembered a card number for a device node, the node had been reused,
 * the note was trusted, and the probe that would have caught it never ran --
 * the gestured controller never showed its mute light. The fix is that every
 * note carries the card's USB identity and is checked against it before it is
 * believed.
 *
 * ⛔ Nothing here needs a television, a controller or a stream. The cache reads
 * one small file per card; these tests point it at a scratch directory and
 * write those files themselves.
 *
 * ➡️ Build and run:  cc tests/test_cardmatch_cache.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── just enough of the world for the cache to compile ──────────────────
 *
 * ⓘ The cache is included from the real file rather than copied, so a change
 * there is seen here. Only the pieces it touches are stubbed: the path it
 * reads, and the log it writes. */

static char g_dir[128];
static char g_usbbus_path[160];
static char g_notes_path[160];
#define CARDMATCH_USBBUS_PATH g_usbbus_path   /* "<dir>/card%d/usbbus" */
#define CARDMATCH_NOTES_PATH  g_notes_path    /* "<dir>/notes" */

static int  log_lines;
static char last_log[512];
static void alsa_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    ++log_lines;
}

#include "../src/controllers/cardmatch_cache.inl"

/* ── the scratch directory ───────────────────────────────────────────── */

static void set_usbbus(int card, const char *value)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/card%d", g_dir, card);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/card%d/usbbus", g_dir, card);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fprintf(f, "%s\n", value);
    fclose(f);
}

static void remove_usbbus(int card)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/card%d/usbbus", g_dir, card);
    unlink(path);
    snprintf(path, sizeof(path), "%s/card%d", g_dir, card);
    rmdir(path);
}

static void reset(void)
{
    for (int card = 0; card < 8; ++card) remove_usbbus(card);
    unlink(g_notes_path);
    memset(g_cardmatch_cache, 0, sizeof(g_cardmatch_cache));
    g_cardmatch_cached = 0;
    g_cardmatch_loaded = 0;
    log_lines = 0;
    last_log[0] = '\0';
}

/* What an app restart does to the cache: the table is gone, the file is not. */
static void restart(void)
{
    memset(g_cardmatch_cache, 0, sizeof(g_cardmatch_cache));
    g_cardmatch_cached = 0;
    g_cardmatch_loaded = 0;
}

/* ── the harness ─────────────────────────────────────────────────────── */

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* ── the tests ───────────────────────────────────────────────────────── */

static void test_remembers_while_the_card_is_the_same(void)
{
    puts("remembers while the card is the same device");
    reset();
    set_usbbus(2, "002/014");
    cardmatch_cache_put("/dev/hidraw2", 2);
    ok(g_cardmatch_cached == 1, "one note written");
    ok(cardmatch_cache_get("/dev/hidraw2") == 2, "the note is believed");
    ok(strcmp(cardmatch_cache_usbbus("/dev/hidraw2"), "002/014") == 0,
       "the note carries the card's usb identity");
    ok(cardmatch_cache_get("/dev/hidraw3") == -1, "an unknown node has no note");
}

static void test_forgets_when_the_card_reenumerates(void)
{
    puts("forgets when the card is a different device -- the 2026-09-01 fault");
    reset();
    set_usbbus(2, "002/014");
    cardmatch_cache_put("/dev/hidraw2", 2);
    set_usbbus(2, "002/015");          /* unplugged, something else replugged */
    ok(cardmatch_cache_get("/dev/hidraw2") == -1, "the stale note is not believed");
    ok(g_cardmatch_cached == 0, "and it is gone, not merely skipped");
    ok(strstr(last_log, "forgot /dev/hidraw2") != NULL &&
       strstr(last_log, "002/014") != NULL && strstr(last_log, "002/015") != NULL,
       "the log says what was forgotten, and why");
}

static void test_forgets_when_the_card_is_gone(void)
{
    puts("forgets when the card no longer exists");
    reset();
    set_usbbus(2, "002/014");
    cardmatch_cache_put("/dev/hidraw2", 2);
    remove_usbbus(2);
    ok(cardmatch_cache_get("/dev/hidraw2") == -1, "a note for a missing card is not believed");
    ok(g_cardmatch_cached == 0, "and it is gone");
    ok(strstr(last_log, "now gone") != NULL, "the log says the card is gone");
}

static void test_does_not_remember_what_it_cannot_verify(void)
{
    puts("does not remember a card whose identity cannot be read");
    reset();
    cardmatch_cache_put("/dev/hidraw2", 2);   /* no usbbus file at all */
    ok(g_cardmatch_cached == 0, "nothing written");
    ok(cardmatch_cache_get("/dev/hidraw2") == -1, "so nothing is believed later");
    ok(strstr(last_log, "usbbus unreadable") != NULL, "and the log says why");
}

static void test_stale_notes_make_room(void)
{
    puts("stale notes make room rather than filling the table for good");
    reset();
    const char *nodes[CARDMATCH_CACHE_MAX] = {
        "/dev/hidraw0", "/dev/hidraw1", "/dev/hidraw2", "/dev/hidraw3" };
    for (int i = 0; i < CARDMATCH_CACHE_MAX; ++i) {
        char id[16];
        snprintf(id, sizeof(id), "002/01%d", i);
        set_usbbus(i, id);
        cardmatch_cache_put(nodes[i], i);
    }
    ok(g_cardmatch_cached == CARDMATCH_CACHE_MAX, "the table is full");
    for (int i = 0; i < CARDMATCH_CACHE_MAX; ++i) set_usbbus(i, "009/099");
    set_usbbus(5, "002/030");
    cardmatch_cache_put("/dev/hidraw9", 5);
    ok(cardmatch_cache_get("/dev/hidraw9") == 5, "a fresh answer still gets written");
    ok(g_cardmatch_cached == 1, "and the four stale notes were dropped to make room");
}

static void test_a_card_belongs_to_one_node(void)
{
    puts("a card belongs to one controller");
    reset();
    set_usbbus(2, "002/014");
    cardmatch_cache_put("/dev/hidraw2", 2);
    cardmatch_cache_put("/dev/hidraw7", 2);   /* the probe just said otherwise */
    ok(cardmatch_cache_get("/dev/hidraw7") == 2, "the newer answer stands");
    ok(cardmatch_cache_get("/dev/hidraw2") == -1, "the older node's note for the same card went");
    ok(g_cardmatch_cached == 1, "one note, not two");
}

static void test_an_update_keeps_the_latest_identity(void)
{
    puts("an updated note carries the new card's identity, not the old one's");
    reset();
    set_usbbus(2, "002/014");
    set_usbbus(3, "002/020");
    cardmatch_cache_put("/dev/hidraw2", 2);
    cardmatch_cache_put("/dev/hidraw2", 3);
    ok(g_cardmatch_cached == 1, "still one note");
    ok(cardmatch_cache_get("/dev/hidraw2") == 3, "it names the new card");
    set_usbbus(2, "002/099");                  /* the OLD card changes */
    ok(cardmatch_cache_get("/dev/hidraw2") == 3, "and the old card's fate no longer matters");
    set_usbbus(3, "002/021");                  /* the NEW card changes */
    ok(cardmatch_cache_get("/dev/hidraw2") == -1, "but the new card's does");
}

static void test_notes_outlive_a_restart(void)
{
    puts("a note outlives an app restart -- the 2026-09-08 double tone");
    reset();
    set_usbbus(2, "002/014");
    cardmatch_cache_put("/dev/hidraw2", 2);
    restart();
    ok(g_cardmatch_cached == 0, "after the restart the table is empty");
    ok(cardmatch_cache_get("/dev/hidraw2") == 2, "and the note comes back from the file");
    ok(g_cardmatch_cached == 1, "one note loaded");
}

static void test_a_loaded_note_is_still_checked(void)
{
    puts("a note read back after a restart is checked like any other");
    reset();
    set_usbbus(2, "002/014");
    cardmatch_cache_put("/dev/hidraw2", 2);
    restart();
    set_usbbus(2, "002/016");            /* re-cabled while the app was down */
    ok(cardmatch_cache_get("/dev/hidraw2") == -1, "a stale loaded note is not believed");
    ok(g_cardmatch_cached == 0, "and it is gone");
    restart();
    ok(cardmatch_cache_get("/dev/hidraw2") == -1, "and the file no longer carries it either");
}

static void test_no_file_is_no_notes(void)
{
    puts("no file, as after a TV reboot, means no notes and no complaint");
    reset();
    ok(cardmatch_cache_get("/dev/hidraw2") == -1, "nothing believed");
    ok(g_cardmatch_cached == 0, "nothing loaded");
}

/* The real matcher must include this file and still ask it the two questions,
 * or the tests above are testing something the television never runs. */
static void test_mirrors_the_real_source(void)
{
    puts("the real matcher uses this cache");
    const char *paths[] = {
        "src/controllers/ctm_cardmatch.inl",
        "../src/controllers/ctm_cardmatch.inl",
    };
    FILE *fp = NULL;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && !fp; ++i) {
        fp = fopen(paths[i], "r");
    }
    if (!fp) { ok(0, "could not open ctm_cardmatch.inl (run from the repo root)"); return; }
    char line[512];
    int includes = 0, gets = 0, puts_ = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "#include \"cardmatch_cache.inl\"")) ++includes;
        if (strstr(line, "cardmatch_cache_get(")) ++gets;
        if (strstr(line, "cardmatch_cache_put(")) ++puts_;
    }
    fclose(fp);
    ok(includes == 1, "ctm_cardmatch.inl includes cardmatch_cache.inl exactly once");
    ok(gets >= 1 && puts_ >= 1, "and still reads and writes it");
}

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/ctm-cardmatch-cache-%ld", (long)getpid());
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST) { perror(g_dir); return 2; }
    snprintf(g_usbbus_path, sizeof(g_usbbus_path), "%s/card%%d/usbbus", g_dir);
    snprintf(g_notes_path, sizeof(g_notes_path), "%s/notes", g_dir);

    puts("");
    test_remembers_while_the_card_is_the_same();   puts("");
    test_forgets_when_the_card_reenumerates();     puts("");
    test_forgets_when_the_card_is_gone();          puts("");
    test_does_not_remember_what_it_cannot_verify(); puts("");
    test_stale_notes_make_room();                  puts("");
    test_a_card_belongs_to_one_node();             puts("");
    test_an_update_keeps_the_latest_identity();    puts("");
    test_notes_outlive_a_restart();                puts("");
    test_a_loaded_note_is_still_checked();         puts("");
    test_no_file_is_no_notes();                    puts("");
    test_mirrors_the_real_source();                puts("");

    reset();
    unlink(g_notes_path);
    rmdir(g_dir);
    printf("%d checks, %d failed\n\n", checks, failed);
    return failed ? 1 : 0;
}
