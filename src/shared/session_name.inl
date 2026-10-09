/* The name a bridge session goes by on the listener: "ctm-<kind>-<n>-<run>".
 *
 * ⛔⛔ UNIQUE TO THIS RUN OF THE APP (code review, 2026-10-05). The name was
 * "ctm-<kind>-<n>", with <n> counting from 1 in every run, so a restarted app
 * sent the listener the very names its last run had used. The listener
 * answers a name it already has with "OK" and makes nothing, and a session of
 * the last run, waiting out its 15 s for a reconnect, took the new pad as its
 * own: a DualSense could land in the old run's pointer session. Every TV used
 * the same names too. ➡️ A tag made once per run ends every name, so a new
 * run's names are new to the listener, which replaces the old run's session
 * on the same port as it replaces any stale one.
 *
 * ⓘ Keep "-<kind>-" in the name: ui_bridge.c finds a DS4's handback tone by
 * it. And keep it under 32 characters, which is all the listener takes. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SESSION_RUN_TAG_LEN 6

/* Six lowercase hex digits from `seed`: this run's tag. */
static void session_run_tag_from(uint32_t seed, char out[SESSION_RUN_TAG_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < SESSION_RUN_TAG_LEN; ++i) {
        out[i] = hex[(seed >> (4 * (SESSION_RUN_TAG_LEN - 1 - i))) & 0xfu];
    }
    out[SESSION_RUN_TAG_LEN] = '\0';
}

/* "ctm-<kind>-<seq>-<run>", cut to fit `out_len`. */
static void session_name_make(char *out, size_t out_len, const char *kind,
                              unsigned seq, const char *run_tag)
{
    snprintf(out, out_len, "ctm-%s-%u-%s", kind, seq, run_tag);
}

/* ⭐⭐ THE REMOTE POINTER'S SERIAL, ONE PER TV (rhoquinn8217, 2026-10-08: two
 * TVs streaming at once). Every TV's pointer told the listener the serial
 * "ctm-tv-pointer", so with two TVs bridged the second pointer retired the
 * first as the same device come back (code review, 2026-10-05). It now ends
 * with the first SESSION_TV_ID_LEN characters of the fingerprint of the key
 * this TV paired with the PC (agent_proof.inl): the same on every run of this
 * TV, different on another. A TV with no key has no fingerprint, and keeps
 * the plain name. */
#define SESSION_TV_ID_LEN 8

static void tv_pointer_serial(char *out, size_t out_len, const char *fingerprint)
{
    if (fingerprint == NULL || strlen(fingerprint) < SESSION_TV_ID_LEN) {
        snprintf(out, out_len, "ctm-tv-pointer");
        return;
    }
    snprintf(out, out_len, "ctm-tv-pointer-%.*s", SESSION_TV_ID_LEN, fingerprint);
}
