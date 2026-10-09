/* Tests for when a bridged pad whose listener stops answering goes back to
 * the TV.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). A listener that took the
 * connection and never answered the greeting left the pad "bridged" for the
 * whole stream: each connect succeeded, and a connect that succeeded started
 * the fifteen seconds again. These pin src/controllers/link_trouble.inl, and
 * read the shipped session code for where it is wired.
 *
 * ⭐ AND THE GIVE-UP IS THREE SECONDS NOW (2026-10-08), with no redial
 * waiting past it: a PC that drops a connect in silence held each one for
 * two seconds, so a second attempt carried three to four or five. The TV
 * pointer's own redial (src/shared/ctm_hostmouse.c) is held to the same:
 * past the listener's five-second hold on a dropped port it could reach
 * another session that took the port.
 *
 * ➡️ Build and run:  cc tests/test_link_trouble.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/controllers/link_trouble.inl"

#define GONE_MS 3000    /* CTM_HOST_GONE_MS: the session's own is checked below */

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static char *load(const char *rel)
{
    const char *prefixes[] = { "", "../" };
    for (unsigned i = 0; i < 2; ++i) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s", prefixes[i], rel);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *s = malloc((size_t)n + 1);
        if (!s) { fclose(f); return NULL; }
        s[fread(s, 1, (size_t)n, f)] = '\0';
        fclose(f);
        return s;
    }
    return NULL;
}

/* From `head` to the first `end` after it. NULL if not found. */
static char *between(const char *src, const char *head, const char *end)
{
    const char *a = src ? strstr(src, head) : NULL;
    if (!a) return NULL;
    const char *b = strstr(a, end);
    if (!b) return NULL;
    char *out = malloc((size_t)(b - a) + 1);
    if (!out) return NULL;
    memcpy(out, a, (size_t)(b - a));
    out[b - a] = '\0';
    return out;
}

/* `b` within `within` characters after `a` in src: two lines that belong
 * together, whatever the file's line endings. */
static int near(const char *src, const char *a, const char *b, long within)
{
    const char *at = src ? strstr(src, a) : NULL;
    const char *then = at ? strstr(at, b) : NULL;
    return then != NULL && then - at <= within;
}

/* session_main's loop, as the session runs it: each connection starts the
 * clock if it is not running, and an unanswered one is checked against the
 * give-up when it ends. Returns the connection it gave up on, or 0. */
static int connections_until_given_up(int answered_every, unsigned long long each_us, int most)
{
    link_trouble_t t = {0, 0};
    unsigned long long now = 1000000ull;
    for (int n = 1; n <= most; ++n) {
        link_trouble_begin(&t, now);
        now += each_us;
        if (answered_every && n % answered_every == 0) {
            link_trouble_clear(&t);
            continue;
        }
        ++t.unanswered;
        if (link_trouble_ms(&t, now) >= GONE_MS) return n;
    }
    return 0;
}

/* The redial after a drop, as session_main runs it, against a PC that
 * answers no connect: each one may wait what is left before the give-up, at
 * most `connect_ms` (the transport's own limit, or 0 for a connect refused at
 * once), and attempts are paced at one each half second, counting their own
 * wait. Returns when the pad goes back, in ms from the drop, or -1. */
static long redial_until_given_up(long connect_ms, int *attempts)
{
    link_trouble_t t = {0, 0};
    const unsigned long long dropped = 1000000ull;
    unsigned long long now = dropped;
    *attempts = 0;
    link_trouble_begin(&t, now);
    while (*attempts < 1000) {
        const long left = link_trouble_left_ms(&t, now, GONE_MS);
        if (left == 0) return (long)((now - dropped) / 1000u);
        const unsigned long long tried = now;
        now += 1000ull * (unsigned long long)(connect_ms < left ? connect_ms : left);
        ++*attempts;
        while (now - tried < 500000ull && link_trouble_left_ms(&t, now, GONE_MS) > 0) now += 50000ull;
    }
    return -1;
}

/* The number after `name` in src, or -1. */
static long defined_as(const char *src, const char *name)
{
    const char *at = src ? strstr(src, name) : NULL;
    return at ? strtol(at + strlen(name), NULL, 10) : -1;
}

int main(void)
{
    printf("\nthe clock\n");
    link_trouble_t t = {0, 0};
    ok(link_trouble_ms(&t, 5000000ull) == 0, "no trouble, no time");
    link_trouble_begin(&t, 2000000ull);
    ok(link_trouble_ms(&t, 3500000ull) == 1500, "it counts from the first failure");
    link_trouble_begin(&t, 3000000ull);
    ok(t.since_us == 2000000ull, "a later connection does not start it again");
    ok(link_trouble_ms(&t, 1000000ull) == 0, "a clock that went back reads no time");
    link_trouble_clear(&t);
    ok(t.since_us == 0 && t.unanswered == 0 && link_trouble_ms(&t, 9000000ull) == 0,
       "an answered greeting stops it");
    link_trouble_begin(&t, 0);
    ok(t.since_us != 0, "it starts even at time zero");

    printf("\nwhat is left before the give-up\n");
    link_trouble_t l = {0, 0};
    ok(link_trouble_left_ms(&l, 5000000ull, GONE_MS) == GONE_MS, "no trouble: all of it");
    link_trouble_begin(&l, 2000000ull);
    ok(link_trouble_left_ms(&l, 4500000ull, GONE_MS) == GONE_MS - 2500, "it shrinks as the trouble lasts");
    ok(link_trouble_left_ms(&l, 2000000ull + 1000ull * GONE_MS, GONE_MS) == 0, "and is gone at the give-up");
    ok(link_trouble_left_ms(&l, 9000000ull, GONE_MS) == 0, "and stays gone after it");

    printf("\nwhat a listener that answers nothing gets\n");
    ok(connections_until_given_up(0, 5500000ull, 50) == 1,
       "every connect succeeds into no answer: given up on the first, at its wait for an answer");
    ok(connections_until_given_up(0, 900000ull, 50) == 4,
       "short connections, each unanswered: given up once three seconds of them have passed");
    ok(connections_until_given_up(2, 2000000ull, 50) == 0,
       "a link that drops and comes back with an answer is never given up on");

    printf("\na PC that has gone\n");
    int tries = 0;
    long back_ms = redial_until_given_up(2000, &tries);
    ok(back_ms == GONE_MS && tries == 2,
       "it drops every redial in silence: the pad goes back at three seconds, after two");
    printf("        (%ld ms, %d attempts)\n", back_ms, tries);
    back_ms = redial_until_given_up(0, &tries);
    ok(back_ms == GONE_MS && tries == 6,
       "it refuses every redial: one each half second, and back at three seconds");
    printf("        (%ld ms, %d attempts)\n", back_ms, tries);

    printf("\nwhere the session uses it\n");
    char *common = load("src/controllers/controller_common.c");
    ok(common != NULL, "the session code can be read");
    if (!common) return 1;
    char *main_loop = between(common, "static void *session_main(void *arg)", "\n}");
    char *run = between(common, "static void run_session(ctm_controller_t *c,", "\n}");
    char *msg = between(common, "static void handle_message(ctm_controller_t *c,", "\n}");
    ok(defined_as(common, "#define CTM_HOST_GONE_MS ") == GONE_MS,
       "the session gives up at the time these checks assume");
    ok(main_loop && strstr(main_loop, "link_trouble_begin(&trouble, now_us());") != NULL,
       "each connection starts the clock");
    ok(main_loop && strstr(main_loop, "const long waited_ms = link_trouble_ms(&trouble, now_us());") != NULL,
       "failed connects are timed by it, not from their own first attempt");
    ok(near(main_loop, "const long left_ms = link_trouble_left_ms(&trouble, now_us(), CTM_HOST_GONE_MS);",
            "if (left_ms == 0) {", 120),
       "the redial gives up when nothing is left");
    ok(near(main_loop, "ctm_transport_connect_within(", "(unsigned int)left_ms)", 160),
       "and no connect may wait longer than what is left");
    ok(main_loop && strstr(main_loop, "now_us() - tried_us < 500000ull") != NULL,
       "the pause between attempts counts the attempt's own wait");
    ok(near(main_loop, "if (c->host_answered) {", "link_trouble_clear(&trouble);", 60),
       "only an answered greeting stops it");
    ok(near(main_loop, "c->host_answered = 0;", "run_session(", 40),
       "and each connection starts unanswered");
    ok(run && strstr(run, "if (c->ops->needs_host_config) c->host_answered = 1;") != NULL,
       "a DualSense's handshake that got its settings is an answer");
    ok(msg && strstr(msg, "c->host_answered = 1;") != NULL,
       "and any message at all is one, for the pads whose handshake does not wait");
    ok(run && strstr(run, "now_us() >= answer_by_us") != NULL,
       "a connection whose greeting is never answered ends");

    printf("\nthe TV pointer, which redials on its own\n");
    char *pointer = load("src/shared/ctm_hostmouse.c");
    ok(pointer != NULL, "the pointer's code can be read");
    ok(defined_as(pointer, "#define HM_HOST_GONE_MS ") == GONE_MS,
       "it gives up when a pad does, inside the listener's hold on a dropped port");
    ok(near(pointer, "if (gone_ms >= HM_HOST_GONE_MS) {", "g_hm.host_gone = 1;", 200),
       "it gives up when its time is gone");
    ok(near(pointer, "ctm_transport_connect_within(&g_hm.xport,",
            "(unsigned int)(HM_HOST_GONE_MS - gone_ms))", 160),
       "and no connect of its waits past that");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
