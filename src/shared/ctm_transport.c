#define _GNU_SOURCE

#include "ctm_transport.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
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

/* ⛔⛔ HOW LONG TO WAIT FOR A CONNECT BEFORE GIVING UP.
 *
 * A plain blocking connect() has no deadline of its own. To a port that
 * REFUSES, it returns instantly; to one that silently DROPS -- a host that is
 * gone, a firewall, a listener that died -- it waits out the kernel's SYN
 * timeout, which is about a minute.
 *
 * ⛔ AND THAT MINUTE LANDS ON THE INTERFACE. Plug out sets a stop flag and then
 * joins the session thread. A thread parked inside connect() cannot see a flag,
 * so the join waits for whatever is left of the connect, and the join runs on
 * the UI thread. Confirmed on the TV with the attempts sat in SYN_SENT, and
 * measured as a one-minute freeze of the overlay -- the video stream carried on
 * throughout, because only the overlay's thread was waiting.
 *
 * ⭐ Two seconds is chosen against the failure it exists for, not against a
 * healthy connect. On a local network a listener that is there answers in
 * single-digit milliseconds; one that does not answer in two seconds is not
 * about to. The retry loop is what handles a listener that comes back, and it
 * can now run instead of the UI waiting. */
#define CTM_CONNECT_TIMEOUT_MS 2000

/* connect() with a deadline: start it non-blocking, wait on the socket, and
 * read the result back out of SO_ERROR.
 *
 * ⚠️ The blocking flag is restored afterwards, because everything downstream
 * expects a blocking socket and would otherwise get EAGAIN on its first read
 * for reasons it has no way to explain. */
static int connect_with_deadline(int fd, const struct sockaddr *addr,
                                 socklen_t addrlen, int timeout_ms)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int rc = connect(fd, addr, addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        do {
            rc = poll(&pfd, 1, timeout_ms);
        } while (rc < 0 && errno == EINTR);

        if (rc <= 0) {
            /* Timed out, or poll itself failed. Either way this is not a
             * connection, and the caller moves on to the next address. */
            (void) fcntl(fd, F_SETFL, flags);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            (void) fcntl(fd, F_SETFL, flags);
            return -1;
        }
        rc = 0;
    }
    (void) fcntl(fd, F_SETFL, flags);
    return rc;
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
