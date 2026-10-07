/* Tests for waiting on a socket with a deadline.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). The handshake waits five
 * seconds for the listener's host config, and over TCP that wait could never
 * end: the read blocks until bytes arrive, so "nothing yet" never came back to
 * be counted. These pin src/shared/wait_readable.inl, which supplies that
 * "nothing yet": a quiet socket times out, a socket with bytes or a closed far
 * end is ready (the read then reports which), and no socket is an error.
 *
 * ➡️ Build and run:  cc tests/test_wait_readable.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../src/shared/wait_readable.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static long elapsed_ms(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000L + (b->tv_nsec - a->tv_nsec) / 1000000L;
}

int main(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("  FAIL  socketpair\n");
        return 1;
    }

    printf("\na quiet socket: nothing within the deadline\n");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = ctm_wait_readable(sv[0], 50);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ok(r == 0, "returns 0 (nothing yet), the case the handshake counts");
    ok(elapsed_ms(&t0, &t1) >= 40, "and it waited for the deadline, not returned at once");

    printf("\na byte waiting: ready\n");
    char b = 'x';
    ok(write(sv[1], &b, 1) == 1, "wrote one byte from the far end");
    ok(ctm_wait_readable(sv[0], 50) == 1, "returns 1");
    ok(read(sv[0], &b, 1) == 1, "and the read takes it without blocking");
    ok(ctm_wait_readable(sv[0], 0) == 0, "drained: back to 0");

    printf("\nthe far end closed: ready, so the read can report it\n");
    close(sv[1]);
    ok(ctm_wait_readable(sv[0], 50) == 1, "returns 1");
    ok(read(sv[0], &b, 1) == 0, "and the read says end of stream");
    close(sv[0]);

    printf("\nno socket\n");
    ok(ctm_wait_readable(-1, 50) == -1, "returns -1 for fd -1");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
