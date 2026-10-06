/* What setting the listener's address does to what is known about it.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). "Probed" was set by the first
 * probe ever made and never cleared, while setting an address set "online" for
 * any address it was given. So a stream to a different PC read ONLINE before
 * anything had asked that PC, and a new stream to the same PC read ONLINE over
 * the last reading, even when that reading said its listener was down.
 *
 * ⭐ THE RULE:
 * - no address: nothing is known, and nothing is online;
 * - a different address or port: nothing is known about it yet, so "probed"
 *   is cleared and the status reads unknown until it answers. "Online" is
 *   assumed, as it always was for a new address: the standalone app waits on
 *   it, and every reading the TV app shows asks "probed" first;
 * - the same address and port: what is known about it stays, until the probe.
 *
 * Returns true when the address or port changed.
 *
 * ⓘ Pure, so that tests/test_agent_endpoint.c can include it as it is.
 */

#ifndef AGENT_ENDPOINT_INL
#define AGENT_ENDPOINT_INL

#include <stdbool.h>
#include <string.h>

static inline bool agent_endpoint_set(const char *old_host, int old_port,
                                      const char *new_host, int new_port,
                                      bool *online, bool *probed)
{
    const bool had = old_host != NULL && old_host[0] != '\0';
    if (new_host == NULL || new_host[0] == '\0') {
        *online = false;
        *probed = false;
        return had;
    }
    const bool same = had && strcmp(old_host, new_host) == 0 && old_port == new_port;
    if (!same) {
        *online = true;
        *probed = false;
    }
    return !same;
}

#endif /* AGENT_ENDPOINT_INL */
