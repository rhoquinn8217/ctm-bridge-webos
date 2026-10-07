/* Whether a socket has something to read within a deadline.
 *
 * ⛔⛔ WHY (code review, 2026-10-05). Over TCP a message is read by a blocking
 * recv, which returns when bytes arrive and not before. So the handshake's
 * five-second wait for the listener's host config could never time out: a
 * listener that accepted the connection and then said nothing held the session
 * for ever, with no "host gone". Waiting for readability first, a slice at a
 * time, gives that loop a "nothing yet" to count against its deadline.
 *
 * ⓘ Its own file so a test can drive it with a socketpair.
 *
 * Returns 1 when a read would not block (data, or the far end closed or
 * failed, which the read then reports), 0 when the deadline passed with
 * nothing, -1 when there is no socket or poll itself failed. */

#include <errno.h>
#include <poll.h>

static inline int ctm_wait_readable(int fd, int timeout_ms)
{
    if (fd < 0) return -1;
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    for (;;) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) return -1;
        return pr == 0 ? 0 : 1;
    }
}
