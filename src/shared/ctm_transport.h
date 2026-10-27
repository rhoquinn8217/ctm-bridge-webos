#ifndef CTM_TRANSPORT_H
#define CTM_TRANSPORT_H

/* Unified CTMB transport: one framed-message channel over TCP. Promotes the
 * abstraction that lived inline in the first standalone worker and relay
 * (send_msg/recv_msg/connect_tcp) into a single module so the framing +
 * connect logic exists once. ⓘ It also spoke ENet/UDP until 2026-10-07,
 * when the app's build took that out; the rest of it went 2026-10-23.
 *
 * Behaviour matches the previously-proven worker path byte-for-byte:
 *   - TCP connect via getaddrinfo (hostname or IP), TCP_NODELAY + keepalive
 *   - send stamps magic/version/timestamp and a monotonic sequence under a
 *     mutex (thread-safe: the input thread and session loop may both send)
 *   - recv validates magic + version + payload_len
 *
 * The transport is framing + connect ONLY. Pacing, CRC, and per-controller
 * patching stay in the layer above. */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "ctm_bridge_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CTM_TRANSPORT_NONE = 0,
    CTM_TRANSPORT_TCP = 1
} ctm_transport_kind_t;

typedef struct {
    ctm_transport_kind_t kind;
    int fd;                     /* TCP socket; -1 otherwise */
    pthread_mutex_t send_mutex; /* serializes TCP header+payload writes */
    uint32_t send_sequence;
    /* The connection's opening exchange is done, so any message may go.
     * Set and read under send_mutex: ctm_transport_send_when_ready(). */
    int ready;
} ctm_transport_t;

/* Initialize to idle. */
void ctm_transport_init(ctm_transport_t *t);

/* Destroy the mutex. */
void ctm_transport_destroy(ctm_transport_t *t);

/* Wrap an already-accepted TCP socket (listen/server mode). */
void ctm_transport_attach_tcp(ctm_transport_t *t, int fd);

/* One attempt, for the transport's own two seconds at most. Sets kind/fd on
 * success. Returns 0 if connected, -1 if not answered this attempt. Callers
 * keep their own retry/backoff loop. */
int ctm_transport_connect_once(ctm_transport_t *t, const char *host, int port);

/* The same attempt, waiting no longer than most_ms in all: for a caller with
 * a deadline of its own, the session's give-up. ⓘ TCP never waits longer
 * than the transport's own two seconds, whatever most_ms says. */
int ctm_transport_connect_within(ctm_transport_t *t, const char *host, int port,
                                 unsigned int most_ms);

/* Send one framed message (header + payload). Thread-safe. Returns 0/-1. */
int ctm_transport_send_msg(ctm_transport_t *t, uint16_t type, uint32_t flags,
                           uint32_t request_id, const void *payload, size_t len);

/* ⭐⭐ SEND ONLY ONCE THE OPENING EXCHANGE IS DONE (code review, 2026-10-05).
 * For a thread that runs across reconnects, the microphone's, which must not
 * speak first on a new connection: the listener closes a connection whose
 * first message is not HELLO. Returns 0 sent, 1 not sent (not ready yet, or
 * no longer), -1 failed. The check and the send hold the send lock together,
 * so no send starts after ctm_transport_set_ready(t, 0) has returned. */
int ctm_transport_send_when_ready(ctm_transport_t *t, uint16_t type, uint32_t flags,
                                  uint32_t request_id, const void *payload, size_t len);

/* ⭐ SEND ONLY IF IT CAN GO NOW (code review, 2026-10-27). For the interface
 * thread, which must not wait on the PC: a PC that stops reading fills the
 * socket, the next send blocks holding the send lock, and anything waiting for
 * that lock waits with it. This waits at most about 50 ms for the lock, which
 * an ordinary send holds for microseconds, and sends only if the socket has
 * room now. Returns 0 sent, 1 not sent (the lock stayed busy, or no room), -1
 * failed. ⓘ For small messages: the room is looked at, not reserved. */
int ctm_transport_try_send_msg(ctm_transport_t *t, uint16_t type, uint32_t flags,
                               uint32_t request_id, const void *payload, size_t len);

/* The opening exchange is done (1), or the connection is ending (0). A
 * connect, an attach and a disconnect set 0 themselves. */
void ctm_transport_set_ready(ctm_transport_t *t, int ready);

/* Pop one received message, waiting for it. Returns 1 (got — fills *h and,
 * for non-empty payloads, mallocs *payload which the caller frees) or -1 (link
 * dropped / bad header). */
int ctm_transport_recv_msg(ctm_transport_t *t, ctmb_header_t *h, uint8_t **payload);

/* Wait up to timeout_ms for something to read. Returns 1 (a read will not
 * block), 0 (nothing yet), -1 (no socket). ⓘ For loops with a deadline, which
 * a blocking recv alone cannot honour (src/shared/wait_readable.inl). */
int ctm_transport_wait_readable(ctm_transport_t *t, unsigned int timeout_ms);

/* 1 if connected (the socket is open), else 0. */
int ctm_transport_connected(const ctm_transport_t *t);

/* Close the socket; return to idle. A send stuck on a peer that stopped
 * reading is woken first, and the close is made under the send lock. */
void ctm_transport_disconnect(ctm_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* CTM_TRANSPORT_H */
