/* The listener's address as the bridge connects to it: a dotted IPv4 address.
 *
 * ⭐ A PC ADDED BY NAME IS LOOKED UP, ONCE A STREAM (code review, 2026-10-05).
 * The commands to the listener took only a dotted IPv4 address, so a PC added
 * by name -- a DDNS name, the usual way to reach one from outside -- streamed
 * and could never bridge: every bridge said "listener not reached". The name
 * becomes its IPv4 address here, and every connection to the listener uses
 * that, the commands and the pads' own.
 * ⓘ IPv4 only, because the listener listens on IPv4 alone. A name without an
 * IPv4 address, or an IPv6 address, is kept as given, and fails as before.
 *
 * Pure: a string in, a string out, and the caller says what happened, so
 * tests/test_agent_address.c checks it. */

#ifndef AGENT_ADDRESS_INL
#define AGENT_ADDRESS_INL

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

/* `host` as a dotted IPv4 address, in `out`. Returns 0 when it already was
 * one (copied as given), 1 when it was a name and was looked up, and -1 when
 * it names no IPv4 address (copied as given, and *why says why).
 * ⓘ A lookup can wait on the network. The caller runs this as a stream starts,
 * for the name the stream itself was looked up by a moment earlier, so the
 * network has just answered for it. */
static inline int agent_address_ipv4(const char *host, char *out, size_t out_len,
                                     const char **why)
{
    *why = "";
    snprintf(out, out_len, "%s", host);
    struct in_addr literal;
    if (inet_aton(host, &literal) != 0) {
        return 0;
    }
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const int rc = getaddrinfo(host, NULL, &hints, &result);
    char text[INET_ADDRSTRLEN];
    int found = -1;
    if (rc == 0 && result &&
        inet_ntop(AF_INET, &((const struct sockaddr_in *)result->ai_addr)->sin_addr,
                  text, sizeof(text))) {
        snprintf(out, out_len, "%s", text);
        found = 1;
    } else {
        *why = rc != 0 ? gai_strerror(rc) : "no answer";
    }
    if (result) {
        freeaddrinfo(result);
    }
    return found;
}

#endif
