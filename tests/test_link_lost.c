/* A guard on what a failed send does: it ends the connection, never the
 * session.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). The input thread set `stop`
 * when a report or a keepalive would not send. That ended the session for good
 * and said nothing: no reconnect, and neither host gone nor device gone, so
 * the app never released the row, which stayed "bridged" with nothing
 * reaching the PC. These read the shipped source: inside the threads that
 * send, a failed send goes to input_link_lost(), and the session loop ends the
 * connection on its flag, so the reconnect and its give-up run.
 *
 * ➡️ Build and run:  cc tests/test_link_lost.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static char *source;

static int load(void)
{
    const char *paths[] = {
        "src/controllers/controller_common.c",
        "../src/controllers/controller_common.c",
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        source = malloc((size_t)n + 1);
        if (!source) { fclose(f); return 0; }
        const size_t got = fread(source, 1, (size_t)n, f);
        source[got] = '\0';
        fclose(f);
        return 1;
    }
    return 0;
}

/* The body of the function whose definition starts with `head`: from there to
 * the first closing brace at the start of a line. NULL if not found. */
static char *body_of(const char *head)
{
    const char *start = strstr(source, head);
    if (!start) return NULL;
    const char *end = strstr(start, "\n}");
    if (!end) return NULL;
    const size_t n = (size_t)(end - start);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, start, n);
    out[n] = '\0';
    return out;
}

static int count(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; p && (p = strstr(p, needle)) != NULL; p += strlen(needle)) ++n;
    return n;
}

int main(void)
{
    if (!load()) { printf("  FAIL  cannot read controller_common.c\n"); return 1; }

    char *input = body_of("static void *input_thread_main(void *arg)");
    char *sibling = body_of("static void *composite_reader_main(void *arg)");
    char *session = body_of("static void run_session(ctm_controller_t *c,");
    ok(input && sibling && session, "the guard can find the three functions");
    if (!input || !sibling || !session) return 1;

    printf("\nthe input thread\n");
    ok(count(input, "c->stop = 1") == 1,
       "sets stop only where its device has gone, never for a send");
    ok(count(input, "input_link_lost(") == 2, "a failed report and a failed keepalive both go to input_link_lost");
    ok(strstr(input, "while (!c->stop && !c->link_send_failed)") != NULL,
       "and the thread ends when the link is lost");

    printf("\nthe sibling readers\n");
    ok(count(sibling, "input_link_lost(") == 1, "a failed sibling report goes there too");
    ok(count(sibling, "c->stop = 1") == 0, "and never stops the session");

    printf("\nthe session\n");
    ok(strstr(session, "c->link_send_failed = 0;") != NULL, "starts each connection with the flag clear");
    ok(strstr(session, "while (!c->stop && link_alive && !c->link_send_failed)") != NULL,
       "ends the connection on it, so the reconnect and its give-up run");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
