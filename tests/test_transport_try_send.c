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
 * ⓘ The full-link checks need two things of the system that Linux does and
 * the TV relies on: a send with MSG_DONTWAIT comes back from a full socket,
 * and shutting a socket down wakes a send blocked on it. The lab's WSL1 left
 * this test waiting twice (2026-10-27), so the system is asked first, with
 * plain sockets, and a check whose need it does not meet is skipped, saying
 * so. The watchdog names the step it caught, since the lab's log keeps only
 * the lines that say FAIL.
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

static void sleep_ms(long ms)
{
    const struct timespec d = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&d, NULL);
}

/* A call that never comes back is a failure, not a run that never ends. */
static const char *volatile step = "starting";

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "  FAIL  a call was still waiting after 10 s, at: ";
    const char *at = step;
    if (write(1, msg, sizeof(msg) - 1) < 0 || write(1, at, strlen(at)) < 0 || write(1, "\n", 1) < 0) {
        _exit(1);
    }
    _exit(1);
}

/* fd's own blocking mode, off for a moment. */
static void nonblocking(int fd, int on)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

/* Fill fd's way out until the next send would wait. Returns 0 if it never
 * filled within 16 MB, which says the system's sockets do not push back.
 * ⓘ With the socket non-blocking, not MSG_DONTWAIT, which the system is
 * asked about below. */
static int fill(int fd)
{
    static uint8_t junk[4096];
    size_t total = 0;
    int filled = 1;
    nonblocking(fd, 1);
    for (size_t chunk = sizeof(junk); chunk > 0 && filled; chunk /= 2) {
        ssize_t n;
        while ((n = send(fd, junk, chunk, MSG_NOSIGNAL)) > 0) {
            total += (size_t)n;
            if (total > 16u * 1024 * 1024) { filled = 0; break; }
        }
    }
    nonblocking(fd, 0);
    return filled;
}

/* Read and throw away everything waiting on fd. */
static void drain(int fd)
{
    uint8_t sink[4096];
    nonblocking(fd, 1);
    while (recv(fd, sink, sizeof(sink), 0) > 0) {}
    nonblocking(fd, 0);
}

/* --- the system itself ---------------------------------------------------- */

static volatile int dontwait_returned;

static void *dontwait_send(void *arg)
{
    static uint8_t more[64];
    (void)send(*(int *)arg, more, sizeof(more), MSG_DONTWAIT | MSG_NOSIGNAL);
    dontwait_returned = 1;
    return NULL;
}

/* 1 if a send with MSG_DONTWAIT comes back from a full socket. One that does
 * not stays blocked for the rest of the run, which ends with the process. */
static int system_honours_dontwait(void)
{
    static int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 || !fill(pair[0])) return 0;
    pthread_t th;
    if (pthread_create(&th, NULL, dontwait_send, &pair[0]) != 0) return 0;
    for (int i = 0; i < 100 && !dontwait_returned; ++i) sleep_ms(10);
    if (!dontwait_returned) return 0;
    pthread_join(th, NULL);
    close(pair[0]);
    close(pair[1]);
    return 1;
}

/* Does shutdown wake a send blocked on a full socket? */

static volatile int plain_returned;

static void *plain_blocked_send(void *arg)
{
    static uint8_t more[4096];
    (void)send(*(int *)arg, more, sizeof(more), MSG_NOSIGNAL);
    plain_returned = 1;
    return NULL;
}

/* 1 if it does. A send it does not wake stays blocked for the rest of the
 * run, which ends with the process. */
static int system_wakes_a_blocked_send(void)
{
    static int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 || !fill(pair[0])) return 0;
    pthread_t th;
    if (pthread_create(&th, NULL, plain_blocked_send, &pair[0]) != 0) return 0;
    sleep_ms(100);
    if (plain_returned) return 0;   /* it did not block: nothing to learn */
    shutdown(pair[0], SHUT_RDWR);
    for (int i = 0; i < 100 && !plain_returned; ++i) sleep_ms(10);
    if (!plain_returned) return 0;
    pthread_join(th, NULL);
    close(pair[0]);
    close(pair[1]);
    return 1;
}

/* --- the transport ------------------------------------------------------ */

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
    setvbuf(stdout, NULL, _IONBF, 0);   /* so a run cut short shows how far it got */
    signal(SIGALRM, on_alarm);
    alarm(10);

    ctm_transport_t t;
    ctm_transport_init(&t);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 2; }
    ctm_transport_attach_tcp(&t, sv[0]);

    printf("\na link with room\n");
    step = "a link with room";
    ok(ctm_transport_try_send_msg(&t, CTMB_MSG_OPEN_CONFIG, CTMB_FLAG_OK, 0, NULL, 0) == 0,
       "the request is sent");
    ctmb_header_t h;
    ok(recv_all(sv[1], &h, sizeof(h)) == 0 && h.type == CTMB_MSG_OPEN_CONFIG && h.payload_len == 0,
       "and arrives whole");
    ok(ctm_transport_try_send_msg(&t, CTMB_MSG_OPEN_CONFIG, CTMB_FLAG_OK, 0, NULL,
                                  CTM_TRY_SEND_MAX + 1) == -1,
       "a payload too big to send at once is refused");

    printf("\nanother send holding the lock\n");
    step = "another send holding the lock";
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
    step = "asking the system about MSG_DONTWAIT";
    const int dontwait = system_honours_dontwait();
    step = "filling the link";
    if (!dontwait) {
        printf("  --    this system's sends wait on a full socket despite MSG_DONTWAIT, so the\n"
               "        full-link checks cannot run here\n");
    } else if (!fill(sv[0])) {
        printf("  --    this system's sockets never filled: the full-link checks cannot run here\n");
    } else {
        step = "a request on a full link";
        clock_gettime(CLOCK_MONOTONIC, &t0);
        rc = ctm_transport_try_send_msg(&t, CTMB_MSG_OPEN_CONFIG, CTMB_FLAG_OK, 0, NULL, 0);
        waited = ms_since(&t0);
        ok(rc == 1, "on a full link the request is not sent");
        ok(waited < 1000, "and the call comes back at once");
        step = "a request once the PC reads again";
        drain(sv[1]);
        ok(ctm_transport_try_send_msg(&t, CTMB_MSG_OPEN_CONFIG, CTMB_FLAG_OK, 0, NULL, 0) == 0,
           "once the PC reads again, it is sent");
        drain(sv[1]);

        printf("\na disconnect while a send is stuck on the full link\n");
        step = "asking the system whether shutdown wakes a blocked send";
        if (!system_wakes_a_blocked_send()) {
            printf("  --    this system's sockets do not wake a send blocked on a full socket when it\n"
                   "        is shut down, so the transport's disconnect cannot be checked here\n");
        } else if (!fill(sv[0])) {
            printf("  --    the link did not fill a second time\n");
        } else {
            step = "a disconnect while a send is stuck";
            pthread_t th;
            pthread_create(&th, NULL, stuck_send, &t);
            sleep_ms(100);
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

    step = "after the disconnect";
    ctm_transport_disconnect(&t);
    ok(ctm_transport_try_send_msg(&t, CTMB_MSG_OPEN_CONFIG, CTMB_FLAG_OK, 0, NULL, 0) == -1,
       "after a disconnect, the request fails");
    close(sv[1]);
    ctm_transport_destroy(&t);

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
