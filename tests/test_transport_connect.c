/* Tests for how long a connect may wait.
 *
 * ⭐ WHY THIS EXISTS (2026-10-08). A bridged pad whose PC has gone waits out
 * the session's give-up before it goes back to the TV, and the give-up is
 * three seconds. A PC that drops a connect in silence, as Windows' firewall
 * does with nothing listening, held each one for the transport's whole two
 * seconds, so a second attempt begun at two carried the give-up to four or
 * five. These pin ctm_transport_connect_within over real sockets: a connect
 * nobody answers waits no longer than it is allowed.
 *
 * ➡️ Build and run:  cc tests/test_transport_connect.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include "../src/shared/ctm_transport.c"

#include <arpa/inet.h>

/* The ENet client is not under test: these only let the transport link. */
int enet_client_connect(ctm_enet_client_t *client, const char *host, int port, unsigned int timeout_ms)
{ (void)client; (void)host; (void)port; (void)timeout_ms; return -1; }
void enet_client_disconnect(ctm_enet_client_t *client) { (void)client; }
int enet_client_connected(const ctm_enet_client_t *client) { (void)client; return 0; }
int enet_client_service(ctm_enet_client_t *client, unsigned int timeout_ms)
{ (void)client; (void)timeout_ms; return 0; }
int enet_client_send_msg(ctm_enet_client_t *client, uint16_t type, uint32_t flags,
                         uint32_t request_id, const void *payload, size_t len)
{ (void)client; (void)type; (void)flags; (void)request_id; (void)payload; (void)len; return -1; }
int enet_client_recv_msg(ctm_enet_client_t *client, ctmb_header_t *h, uint8_t **payload)
{ (void)client; (void)h; (void)payload; return -1; }

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

/* A socket on loopback, listening with `backlog`; its port in *port. */
static int listening(int backlog, int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;   /* the system picks a free port */
    socklen_t alen = sizeof(a);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, backlog) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &alen) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    return fd;
}

/* A connect begun and never finished with: it takes a place in a queue. */
static int dial_and_leave(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    (void)connect(fd, (struct sockaddr *)&a, sizeof(a));
    return fd;
}

int main(void)
{
    ctm_transport_t t;
    ctm_transport_init(&t, NULL);
    struct timespec t0;
    int port = 0;

    printf("\na PC that answers\n");
    int lfd = listening(4, &port);
    if (lfd < 0) { perror("listener"); return 2; }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ok(ctm_transport_connect_within(&t, "127.0.0.1", port, 0, 300) == 0, "the connect lands");
    ok(ms_since(&t0) < 200, "at once");
    ctm_transport_disconnect(&t);
    close(lfd);

    printf("\na PC with nothing listening that says so\n");
    lfd = listening(1, &port);
    if (lfd < 0) { perror("listener"); return 2; }
    close(lfd);   /* the port is free again, and nothing is on it */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ok(ctm_transport_connect_within(&t, "127.0.0.1", port, 0, 1000) != 0, "the connect is refused");
    ok(ms_since(&t0) < 200, "at once: the session paces its attempts for this one");

    printf("\na PC that drops the connect in silence\n");
    /* ⓘ A listener whose queue is full: Linux drops the next connect's first
     * packet, as Windows' firewall does with nothing listening, and the
     * connect waits for an answer that never comes. */
    lfd = listening(0, &port);
    if (lfd < 0) { perror("listener"); return 2; }
    const int fill_a = dial_and_leave(port);
    const int fill_b = dial_and_leave(port);
    usleep(100000);   /* the first is in the queue now */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = ctm_transport_connect_within(&t, "127.0.0.1", port, 0, 300);
    long took = ms_since(&t0);
    ok(rc != 0, "a connect nobody answers fails");
    ok(took >= 250 && took < 1000, "after the 300 ms it was allowed, not the transport's two seconds");
    printf("        (%ld ms)\n", took);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rc = ctm_transport_connect_within(&t, "127.0.0.1", port, 0, 5000);
    took = ms_since(&t0);
    ok(rc != 0 && took >= 1900 && took < 2600,
       "allowed five seconds, it still waits only the transport's two");
    printf("        (%ld ms)\n", took);
    ok(ctm_transport_connect_within(&t, "127.0.0.1", port, 0, 0) != 0, "allowed nothing, it does not try");
    if (fill_a >= 0) close(fill_a);
    if (fill_b >= 0) close(fill_b);
    close(lfd);
    ctm_transport_destroy(&t);

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
