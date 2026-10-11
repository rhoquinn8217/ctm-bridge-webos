/* Tests for when a bridged pad whose listener stops answering goes back to
 * the TV.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). A listener that took the
 * connection and never answered the greeting left the pad "bridged" for the
 * whole stream: each connect succeeded, and a connect that succeeded started
 * the fifteen seconds again. These pin src/controllers/link_trouble.inl, and
 * read the shipped session code for where it is wired.
 *
 * ➡️ Build and run:  cc tests/test_link_trouble.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/controllers/link_trouble.inl"

#define GONE_MS 15000   /* CTM_HOST_GONE_MS */

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
 * fifteen seconds when it ends. Returns the connection it gave up on, or 0. */
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

    printf("\nwhat a listener that answers nothing gets\n");
    ok(connections_until_given_up(0, 5500000ull, 50) == 3,
       "every connect succeeds into no answer: given up on the third, about 16 s in");
    ok(connections_until_given_up(0, 900000ull, 50) == 17,
       "short connections, each unanswered: given up after fifteen seconds of them");
    ok(connections_until_given_up(2, 5500000ull, 50) == 0,
       "a link that drops and comes back with an answer is never given up on");

    printf("\nwhere the session uses it\n");
    char *common = load("src/controllers/controller_common.c");
    ok(common != NULL, "the session code can be read");
    if (!common) return 1;
    char *main_loop = between(common, "static void *session_main(void *arg)", "\n}");
    char *run = between(common, "static void run_session(ctm_controller_t *c,", "\n}");
    char *msg = between(common, "static void handle_message(ctm_controller_t *c,", "\n}");
    ok(main_loop && strstr(main_loop, "link_trouble_begin(&trouble, now_us());") != NULL,
       "each connection starts the clock");
    ok(main_loop && strstr(main_loop, "const long waited_ms = link_trouble_ms(&trouble, now_us());") != NULL,
       "failed connects are timed by it, not from their own first attempt");
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

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
