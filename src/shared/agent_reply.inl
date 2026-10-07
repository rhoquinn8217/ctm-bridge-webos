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
