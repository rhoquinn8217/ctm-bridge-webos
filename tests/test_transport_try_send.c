/* Tests for a send that must not wait, and a disconnect a stuck send cannot
 * hold up.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-27). The TV's button that opens
 * DS5-USBIP's settings window sends from the interface thread, with the app's
 * device locks held. A PC that stops reading fills the socket, the next send
 * blocks holding the transport's send lock, and the button waited behind it:
 * the whole interface froze. These pin, over real sockets,
 * ctm_transport_try_send_msg, which comes back when the lock stays busy or the
 * socket is full, and ctm_transport_disconnect, which wakes a send stuck on a
 * full socket instead of waiting behind it.
 *
 * ➡️ Build and run:  cc tests/test_transport_try_send.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include "../src/shared/ctm_transport.c"

#include <pthread.h>
#include <signal.h>

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static long ms_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (long)((t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000);
}

/* A call that never comes back is a failure, not a run that never ends. */
static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "  FAIL  a call was still waiting after 10 s\n";
    if (write(1, msg, sizeof(msg) - 1) < 0) _exit(1);
    _exit(1);
}

/* Fill fd's way out until the next send would wait. Returns 0 if it never
 * filled within 16 MB, which says the system's sockets do not push back. */
static int fill(int fd)
{
    static uint8_t junk[4096];
    size_t total = 0;
    for (size_t chunk = sizeof(junk); chunk > 0; chunk /= 2) {
        ssize_t n;
        while ((n = send(fd, junk, chunk, MSG_DONTWAIT | MSG_NOSIGNAL)) > 0) {
            total += (size_t)n;
            if (total > 16u * 1024 * 1024) return 0;
        }
    }
    return 1;
}

/* Read and throw away everything waiting on fd. */
static void drain(int fd)
{
    uint8_t sink[4096];
    while (recv(fd, sink, sizeof(sink), MSG_DONTWAIT) > 0) {}
}

static volatile int stuck_returned;
static int stuck_rc;

/* A send big enough not to fit, on a socket already full: it blocks. */
static void *stuck_send(void *arg)
{
    static uint8_t report[4096];
    stuck_rc = ctm_transport_send_msg((ctm_transport_t *)arg, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, 0,
                                      report, sizeof(report));
    stuck_returned = 1;
    return NULL;
}

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(10);

    ctm_transport_t t;
    ctm_transport_init(&t);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 2; }
    ctm_transport_attach_tcp(&t, sv[0]);

    printf("\na link with room\n");
    ok(ctm_transport_try_send_msg(&t, CTMB_MSG_OPEN_CONFIG, CTMB_FLAG_OK, 0, NULL, 0) == 0,
       "the request is sent");
    ctmb_header_t h;
    ok(recv_all(sv[1], &h, sizeof(h)) == 0 && h.type == CTMB_MSG_OPEN_CONFIG && h.payload_len == 0,
       "and arrives whole");

    printf("\nanother send holding the lock\n");
    pthread_mutex_lock(&t.send_mutex);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = ctm_transport_try_send_msg(&t, CTMB_MSG_OPEN_CONFIG, CTMB_FLAG_OK, 0, NULL, 0);
    long waited = ms_since(&t0);
    pthread_mutex_unlock(&t.send_mutex);
    ok(rc == 1, "the request is not sent");
    ok(waited < 1000, "and the call comes back instead of waiting for the lock");
    printf("        (it waited %ld ms)\n", waited);

    printf("\na PC that stopped reading\n");
    if (!fill(sv[0])) {
        printf("  --    this system's sockets never filled: the full-link checks cannot run here\n");
    } else {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        rc = ctm_transport_try_send_msg(&t, CTMB_MSG_OPEN_CONFIG, CTMB_FLAG_OK, 0, NULL, 0);
        waited = ms_since(&t0);
        ok(rc == 1, "on a full link the request is not sent");
        ok(waited < 1000, "and the call comes back at once");
        drain(sv[1]);
        ok(ctm_transport_try_send_msg(&t, CTMB_MSG_OPEN_CONFIG, CTMB_FLAG_OK, 0, NULL, 0) == 0,
           "once the PC reads again, it is sent");
        drain(sv[1]);

        printf("\na disconnect while a send is stuck on the full link\n");
        if (!fill(sv[0])) {
            printf("  --    the link did not fill a second time\n");
        } else {
            pthread_t th;
            pthread_create(&th, NULL, stuck_send, &t);
            const struct timespec a_tenth = {0, 100000000};
            nanosleep(&a_tenth, NULL);
            ok(!stuck_returned, "the send is stuck, holding the lock");
            clock_gettime(CLOCK_MONOTONIC, &t0);
            ctm_transport_disconnect(&t);
            waited = ms_since(&t0);
            pthread_join(th, NULL);
            ok(waited < 1000, "the disconnect does not wait behind it");
            ok(stuck_returned && stuck_rc == -1, "the stuck send is woken, and fails");
            ok(!ctm_transport_connected(&t), "the transport is closed");
        }
    }

    ctm_transport_disconnect(&t);
    ok(ctm_transport_try_send_msg(&t, CTMB_MSG_OPEN_CONFIG, CTMB_FLAG_OK, 0, NULL, 0) == -1,
       "after a disconnect, the request fails");
    close(sv[1]);
    ctm_transport_destroy(&t);

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
