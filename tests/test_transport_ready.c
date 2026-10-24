/* Tests for the transport's hold on sends before a connection's opening
 * exchange.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). A cabled DualSense's
 * microphone thread runs across reconnects, and it asked only whether the
 * link was connected. A reconnect is connected before its HELLO, so the
 * microphone spoke first, the listener closed the connection, and every
 * redial met the same end until the give-up. These pin
 * ctm_transport_send_when_ready over real sockets: nothing it carries goes
 * before the session says the exchange is done, the opening messages are not
 * held back, and a new connection starts held back again.
 *
 * ➡️ Build and run:  cc tests/test_transport_ready.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include "../src/shared/ctm_transport.c"

#include <arpa/inet.h>
#include <poll.h>

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* Is there anything to read on fd right now? */
static int pending(int fd)
{
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, 50) > 0 && (p.revents & POLLIN);
}

/* The next message's header, its payload read and thrown away. */
static int next_type(int fd, uint32_t *payload_len)
{
    ctmb_header_t h;
    if (recv_all(fd, &h, sizeof(h)) != 0) return -1;
    uint8_t skip[64];
    for (uint32_t left = h.payload_len; left > 0;) {
        const uint32_t n = left < sizeof(skip) ? left : (uint32_t)sizeof(skip);
        if (recv_all(fd, skip, n) != 0) return -1;
        left -= n;
    }
    if (payload_len) *payload_len = h.payload_len;
    return h.type;
}

static int send_audio(ctm_transport_t *t)
{
    static const uint8_t chunk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    return ctm_transport_send_when_ready(t, CTMB_MSG_MIC_AUDIO, CTMB_FLAG_OK, 0, chunk, sizeof(chunk));
}

int main(void)
{
    ctm_transport_t t;
    ctm_transport_init(&t);

    printf("\na connection, before and after its opening exchange\n");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 2; }
    ctm_transport_attach_tcp(&t, sv[0]);
    ok(send_audio(&t) == 1, "before the hello, audio is held back");
    ok(!pending(sv[1]), "and nothing reaches the listener");
    ok(ctm_transport_send_msg(&t, CTMB_MSG_HELLO, CTMB_FLAG_OK, 0, NULL, 0) == 0,
       "the hello itself is not held back");
    ok(next_type(sv[1], NULL) == CTMB_MSG_HELLO, "so the hello is the first thing the listener reads");
    ctm_transport_set_ready(&t, 1);
    uint32_t len = 0;
    ok(send_audio(&t) == 0, "after the exchange, audio goes");
    ok(next_type(sv[1], &len) == CTMB_MSG_MIC_AUDIO && len == 8, "as one whole message");
    ctm_transport_set_ready(&t, 0);
    ok(send_audio(&t) == 1, "a connection that is ending holds it back again");
    ok(!pending(sv[1]), "and nothing more reaches the listener");

    printf("\na reconnect, the way the session makes one\n");
    ctm_transport_disconnect(&t);
    close(sv[1]);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;   /* the system picks a free port */
    socklen_t alen = sizeof(a);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 1) != 0 ||
        getsockname(lfd, (struct sockaddr *)&a, &alen) != 0) {
        perror("listener");
        return 2;
    }
    ctm_transport_set_ready(&t, 1);   /* as the last connection left it, at worst */
    ok(ctm_transport_connect_once(&t, "127.0.0.1", ntohs(a.sin_port)) == 0, "the reconnect lands");
    const int peer = accept(lfd, NULL, NULL);
    ok(ctm_transport_connected(&t), "the transport calls itself connected");
    ok(send_audio(&t) == 1, "and still holds audio back: a reconnect starts before its hello");
    ok(peer >= 0 && !pending(peer), "so the listener reads nothing before the hello");
    ok(ctm_transport_send_msg(&t, CTMB_MSG_HELLO, CTMB_FLAG_OK, 0, NULL, 0) == 0 &&
       next_type(peer, NULL) == CTMB_MSG_HELLO, "and the hello is first on the new connection too");

    ctm_transport_disconnect(&t);
    ok(send_audio(&t) == 1, "after a disconnect, nothing is sent at all");
    if (peer >= 0) close(peer);
    close(lfd);
    ctm_transport_destroy(&t);

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
