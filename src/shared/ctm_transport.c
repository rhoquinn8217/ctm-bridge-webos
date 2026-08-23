#define _GNU_SOURCE

#include "ctm_transport.h"

#include <errno.h>
#include <fcntl.h>      /* ⓘ connect_with_deadline -- O_NONBLOCK */
#include <netdb.h>
#include <poll.h>       /* ⓘ connect_with_deadline -- the deadline itself */
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t ctm_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *data, size_t len)
{
    uint8_t *p = (uint8_t *)data;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static void tune_tcp(int fd)
{
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
}

/* ⭐⭐ A CONNECT THAT COMES BACK. T-127, 2026-08-23.
 *
 * ⛔ THE FAULT: kill the listener while a controller is bridged and the app
 * hangs, and pressing Release in the panel crashes it. ⚠️ It is not the panel
 * and it is not the missing teardown -- IT IS THE JOIN. Release runs on the UI
 * thread, sets the session's stop flag, then waits for that thread. The thread
 * is parked inside connect(), and a thread inside a blocking call cannot see a
 * flag. The interface is frozen for whatever is left of the attempt.
 *
 * ⭐ AND A DEAD LISTENER DROPS PACKETS RATHER THAN REFUSING THEM. Observed on
 * the TV as `tcp 0 1 ...:60290 ...:48055 SYN_SENT`. A refused connection
 * returns instantly; a dropped one waits out the KERNEL'S SYN TIMEOUT, which is
 * around a minute. ⓘ That is also why the hang looked intermittent -- the wait
 * is whatever remains of the attempt in flight, so the same button gives a
 * different answer every time.
 *
 * ⛔ NOTHING ELSE HERE BOUNDS IT. `ui_bridge.c` sets SO_RCVTIMEO and
 * SO_SNDTIMEO, which bound reads and writes and NOT connect -- a note in the
 * docs claiming it hand-rolls a bounded connect is wrong, and was believed long
 * enough to be written into T-127.
 *
 * ⚠️ THE TIMEOUT IS DELIBERATELY GENEROUS. Two seconds is far longer than any
 * reachable host on a LAN needs and far shorter than a minute of frozen
 * interface. ⓘ A REMOTE host over the internet is the case to watch: if this
 * ever starts refusing connections that would have succeeded, raise it -- do
 * not remove it. */
#define CTM_CONNECT_TIMEOUT_MS 2000

static int connect_with_deadline(int fd, const struct sockaddr *addr,
                                 socklen_t addrlen, int timeout_ms)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int rc = connect(fd, addr, addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
        (void)fcntl(fd, F_SETFL, flags);
        return -1;
    }

    if (rc != 0) {
        /* ⓘ POLLOUT fires on success AND on failure; the error is read back
         * from the socket rather than inferred from poll's return. */
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) {                       /* 0 = timed out, -1 = error */
            (void)fcntl(fd, F_SETFL, flags);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            (void)fcntl(fd, F_SETFL, flags);
            return -1;
        }
    }

    /* ⭐ Blocking again for the caller. Everything downstream reads and writes
     * this socket expecting blocking semantics; only the CONNECT was the
     * problem. */
    if (fcntl(fd, F_SETFL, flags) < 0) return -1;
    return 0;
}

static int connect_tcp(const char *host, int port)
{
    char port_text[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    snprintf(port_text, sizeof(port_text), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port_text, &hints, &result) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *rp = result; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect_with_deadline(fd, rp->ai_addr, rp->ai_addrlen,
                                  CTM_CONNECT_TIMEOUT_MS) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    if (fd >= 0) tune_tcp(fd);
    return fd;
}

void ctm_transport_init(ctm_transport_t *t, ctm_enet_client_t *enet)
{
    if (!t) return;
    t->kind = CTM_TRANSPORT_NONE;
    t->fd = -1;
    t->enet = enet;
    t->send_sequence = 0;
    pthread_mutex_init(&t->send_mutex, NULL);
}

void ctm_transport_destroy(ctm_transport_t *t)
{
    if (!t) return;
    pthread_mutex_destroy(&t->send_mutex);
}

void ctm_transport_attach_tcp(ctm_transport_t *t, int fd)
{
    if (!t) return;
    t->kind = CTM_TRANSPORT_TCP;
    t->fd = fd;
    if (fd >= 0) tune_tcp(fd);
}

int ctm_transport_connect_once(ctm_transport_t *t, const char *host, int port,
                               unsigned int enet_timeout_ms)
{
    if (!t) return -1;

    /* 1) ENet first, brief timeout. */
    if (t->enet && enet_client_connect(t->enet, host, port, enet_timeout_ms) == 0) {
        t->kind = CTM_TRANSPORT_ENET;
        t->fd = -1;
        return 0;
    }

    /* 2) Fall back to TCP. */
    int fd = connect_tcp(host, port);
    if (fd >= 0) {
        t->kind = CTM_TRANSPORT_TCP;
        t->fd = fd;
        return 0;
    }
    return -1;
}

int ctm_transport_send_msg(ctm_transport_t *t, uint16_t type, uint32_t flags,
                           uint32_t request_id, const void *payload, size_t len)
{
    if (!t || len > CTMB_MAX_PAYLOAD) return -1;

    /* ENet: enet_client_send_msg is thread-safe (queues to an outbox the
     * service pump drains on the owning thread) and stamps its own header. */
    if (t->kind == CTM_TRANSPORT_ENET) {
        return enet_client_send_msg(t->enet, type, flags, request_id, payload, len);
    }

    if (t->fd < 0) return -1;
    ctmb_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = CTMB_MAGIC;
    h.version = CTMB_VERSION;
    h.type = type;
    h.flags = flags;
    h.timestamp_us = ctm_now_us();
    h.request_id = request_id;
    h.payload_len = (uint32_t)len;

    pthread_mutex_lock(&t->send_mutex);
    h.sequence = ++t->send_sequence;
    int rc = 0;
    if (send_all(t->fd, &h, sizeof(h)) != 0) rc = -1;
    else if (len && send_all(t->fd, payload, len) != 0) rc = -1;
    pthread_mutex_unlock(&t->send_mutex);
    return rc;
}

int ctm_transport_recv_msg(ctm_transport_t *t, ctmb_header_t *h, uint8_t **payload)
{
    *payload = NULL;
    if (!t) return -1;
    if (t->kind == CTM_TRANSPORT_ENET) {
        return enet_client_recv_msg(t->enet, h, payload);
    }
    if (t->fd < 0) return -1;
    if (recv_all(t->fd, h, sizeof(*h)) != 0) return -1;
    if (h->magic != CTMB_MAGIC || h->version != CTMB_VERSION ||
        h->payload_len > CTMB_MAX_PAYLOAD) {
        return -1;
    }
    if (h->payload_len) {
        *payload = (uint8_t *)malloc(h->payload_len);
        if (!*payload) return -1;
        if (recv_all(t->fd, *payload, h->payload_len) != 0) {
            free(*payload);
            *payload = NULL;
            return -1;
        }
    }
    return 1;
}

int ctm_transport_service(ctm_transport_t *t, unsigned int timeout_ms)
{
    if (t && t->kind == CTM_TRANSPORT_ENET) {
        return enet_client_service(t->enet, timeout_ms);
    }
    return 0;
}

int ctm_transport_connected(const ctm_transport_t *t)
{
    if (!t) return 0;
    if (t->kind == CTM_TRANSPORT_ENET) return enet_client_connected(t->enet);
    return t->fd >= 0;
}

void ctm_transport_disconnect(ctm_transport_t *t)
{
    if (!t) return;
    if (t->kind == CTM_TRANSPORT_ENET) {
        if (t->enet) enet_client_disconnect(t->enet);
    } else if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
    }
    t->fd = -1;
    t->kind = CTM_TRANSPORT_NONE;
}
