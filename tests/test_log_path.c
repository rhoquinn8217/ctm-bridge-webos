/* Tests for where the core's logs go, and that a thread's log path is its own.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). The path was built in one buffer
 * for the whole process, so a thread holding a log's path while another asked
 * for a different one found its own path changed underneath it, and its line
 * went into the other file.
 *
 * ➡️ Build and run:  cc -pthread tests/test_log_path.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/shared/log_path.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* Copied out inside the thread: a thread's own buffer goes when it ends. */
static char s_other[PATH_MAX];

static void *ask_for_another(void *arg)
{
    (void)arg;
    snprintf(s_other, sizeof(s_other), "%s", ctm_log_path("ctm-signal.log"));
    return NULL;
}

int main(void)
{
    /* No HOME: the folder is /tmp, and nothing is created anywhere. */
    unsetenv("HOME");

    printf("\nthe folder, and a name in it\n");
    const char *mine = ctm_log_path("ctm-gesture.log");
    ok(strcmp(mine, "/tmp/ctm-gesture.log") == 0, "no HOME: /tmp/ctm-gesture.log");
    ok(strcmp(ctm_log_path(NULL), "/tmp/ctm.log") == 0, "no name: ctm.log");

    printf("\none thread's path is not changed by another's\n");
    mine = ctm_log_path("ctm-gesture.log");
    pthread_t other;
    ok(pthread_create(&other, NULL, ask_for_another, NULL) == 0, "a second thread starts");
    pthread_join(other, NULL);
    ok(strcmp(s_other, "/tmp/ctm-signal.log") == 0, "the other thread got its own path");
    ok(strcmp(mine, "/tmp/ctm-gesture.log") == 0,
       "the path this thread holds is still its own (a shared buffer read ctm-signal.log)");

    printf("\n%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
