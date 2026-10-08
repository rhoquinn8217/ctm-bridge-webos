/* What send_agent_command returns, from what happened on the wire.
 *
 * ⭐ THE CONTRACT, which its callers branch on, and which nothing tested (code
 * review, 2026-10-05):
 *
 *    0  the listener answered, and the answer began "OK".
 *   -2  the listener was NOT REACHED: no connection. One lost packet can do
 *       that, so a caller may try once more (the app's bridge request does).
 *   -1  everything else: the command was not sent, no answer came, or the
 *       answer was not OK. ⚠️ No answer is -1 with an EMPTY reply, and a
 *       caller that asked for something to start must then assume it may have
 *       started (plug_in_scan_index tells the listener to stop it).
 *
 * ⓘ No address and no socket are -1 too, decided before anything is sent.
 * Its own file so a test can pin it. */

#include <stdbool.h>
#include <string.h>
#include <sys/types.h>

static inline int agent_command_rc(bool reached, bool sent, ssize_t got, const char *reply)
{
    if (!reached) return -2;
    if (!sent) return -1;
    return (got > 0 && reply && strncmp(reply, "OK", 2) == 0) ? 0 : -1;
}

/* The port a BRIDGE_START answer hands out, "OK bridge starting port=48057",
 * or 0 when it names none, or a number that cannot be a bridge's port.
 * ⓘ The listener hands one out to a TV that asks with port 0, so two TVs
 * never choose the same one (rhoquinn8217, 2026-10-08: two TVs at once). */
static inline int agent_reply_port(const char *reply)
{
    const char *at = reply ? strstr(reply, "port=") : NULL;
    if (!at) return 0;
    at += 5;
    long value = 0;
    int digits = 0;
    while (*at >= '0' && *at <= '9' && digits < 6) {
        value = value * 10 + (*at - '0');
        ++at;
        ++digits;
    }
    if (digits == 0 || value < 1024 || value > 65535) return 0;
    return (int)value;
}
