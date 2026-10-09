/* The proof a TV sends with a bridge command: that it is paired with the PC.
 *
 * ⭐ WHY (code review, 2026-10-05). The listener's control port is forwarded
 * to the internet for remote play, and a BRIDGE_START from anyone made
 * Windows see a USB keyboard they could type on. rhoquinn8217 chose,
 * 2026-10-07, that the listener let in exactly the devices the stream already
 * trusts: a TV paired with the PC's streaming host proves it with the key it
 * paired with. The listener's half is its paired_tv.inl.
 *
 * ⭐ THE EXCHANGE, for BRIDGE_START and BRIDGE_STOP only:
 *   1. "PROOF_CHALLENGE", answered "OK NONCE <32 hex>": single use, about 30 s.
 *   2. The command with one more word on the end:
 *        BRIDGE_START ds5 48056 ctm-ds5-3 PROOF=<nonce>:<fingerprint>:<signature>
 *      where the signature is over agent_proof_signed_text(), by the app's
 *      paired key, and the fingerprint names which paired device it is.
 * ⓘ An older listener answers the challenge "ERR unknown command", and the
 * command then goes plainly, as before; one that does not know PROOF= reads
 * the command's own words and never looks at the last one. The core never
 * sees a key: the app signs (ctm_bridge_set_proof_signer).
 *
 * Its own file so a test can pin it, as agent_reply.inl is. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define AGENT_PROOF_NONCE_LEN 32
#define AGENT_PROOF_FINGERPRINT_LEN 32

/* Which commands carry a proof: the two that bridge and unbridge. STATUS is
 * the listener's probe, and stays as it is. */
static inline bool agent_proof_wanted(const char *command)
{
    return command != NULL &&
           (strncmp(command, "BRIDGE_START ", 13) == 0 ||
            strncmp(command, "BRIDGE_STOP ", 12) == 0);
}

/* The nonce from the listener's answer to PROOF_CHALLENGE: "OK NONCE " and 32
 * lowercase hex. False for anything else, an older listener's "ERR unknown
 * command" included. */
static inline bool agent_proof_nonce(const char *reply, char *nonce, size_t len)
{
    static const char prefix[] = "OK NONCE ";
    if (reply == NULL || nonce == NULL || len < AGENT_PROOF_NONCE_LEN + 1) return false;
    if (strncmp(reply, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *hex = reply + sizeof(prefix) - 1;
    for (int i = 0; i < AGENT_PROOF_NONCE_LEN; ++i) {
        const char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    const char end = hex[AGENT_PROOF_NONCE_LEN];
    if (end != '\0' && end != '\n' && end != '\r' && end != ' ') return false;
    memcpy(nonce, hex, AGENT_PROOF_NONCE_LEN);
    nonce[AGENT_PROOF_NONCE_LEN] = '\0';
    return true;
}

/* What the TV signs, byte for byte what the listener checks: a first line no
 * other message begins with, so the signature can serve for nothing else,
 * then the nonce and the command. The length, or 0 when it does not fit. */
static inline size_t agent_proof_signed_text(const char *nonce, const char *command,
                                             char *out, size_t len)
{
    const int n = snprintf(out, len, "DS5-USBIP proof 1\n%s\n%s", nonce, command);
    return (n > 0 && (size_t)n < len) ? (size_t)n : 0;
}

/* The line sent: the command, then " PROOF=<nonce>:<fingerprint>:<signature>".
 * The length, or 0 when it does not fit: the caller then sends the command
 * plainly rather than cut short. */
static inline size_t agent_proof_line(const char *command, const char *nonce,
                                      const char *fingerprint, const char *signature64,
                                      char *out, size_t len)
{
    const int n = snprintf(out, len, "%s PROOF=%s:%s:%s", command, nonce, fingerprint, signature64);
    return (n > 0 && (size_t)n < len) ? (size_t)n : 0;
}
