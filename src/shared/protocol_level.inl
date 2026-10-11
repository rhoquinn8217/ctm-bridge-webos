/* Can this TV app and DS5-USBIP on the PC work together? The protocol level
 * each side has, and what the TV does when they cannot.
 *
 * ⭐⭐ rhoquinn8217, 2026-10-08 ("1a and 2a"):
 * - 2a: A NUMBER THAT GOES UP ONLY WHEN THE WAY THE TWO TALK CHANGES, so an
 *   ordinary update on one side keeps working with the other. Raise
 *   CTM_PROTOCOL_LEVEL when a change means an older listener can no longer
 *   work with this TV -- a message it cannot skip, a layout it reads wrong --
 *   and CTM_PROTOCOL_MIN_LISTENER when this TV can no longer work with a
 *   listener below some level. A change the other side copes with -- an
 *   optional word an older peer ignores, as the pairing proof and the port
 *   request are -- raises nothing.
 * - 1a: WHEN THEY CANNOT, THE TV REFUSES TO BRIDGE AND SAYS WHICH SIDE TO
 *   UPDATE, in the bridge panel, with the usual three red flashes.
 *
 * ⛔ WHY (code review, 2026-10-05): nothing compared the two sides. A
 * listener that could not read the TV's greeting turned it down, and the pad
 * hung "bridged" for the whole stream with nothing to say why.
 *
 * HOW. The listener's STATUS answer, which the TV asks for every ten seconds,
 * carries its level and the oldest TV level it works with:
 *     OK CTM_AGENT_V1 level=1 min_tv=1
 * and a BRIDGE_START carries the TV's own, as "level=N", so a listener can
 * turn an older TV away with "ERR update the TV app ...". A side that says no
 * level is level 1, the level every TV and listener had before there were
 * levels. Its own file so a test can pin it. */

#ifndef PROTOCOL_LEVEL_INL
#define PROTOCOL_LEVEL_INL

#include <stdbool.h>
#include <string.h>

#define CTM_PROTOCOL_LEVEL         1
#define CTM_PROTOCOL_MIN_LISTENER  1

typedef enum {
    PROTOCOL_UNKNOWN = 0,   /* no answer yet */
    PROTOCOL_OK,
    PROTOCOL_UPDATE_PC,     /* the listener is older than this TV works with */
    PROTOCOL_UPDATE_TV      /* this TV is older than the listener works with */
} protocol_compat_t;

/* The number after `word` in a reply, " level=" in "OK CTM_AGENT_V1 level=2",
 * or `missing` when the word is not there or no number follows it. */
static inline int protocol_word(const char *reply, const char *word, int missing)
{
    const char *at = (reply && word) ? strstr(reply, word) : NULL;
    if (!at) return missing;
    at += strlen(word);
    if (*at < '0' || *at > '9') return missing;
    long value = 0;
    for (int digits = 0; *at >= '0' && *at <= '9' && digits < 6; ++at, ++digits) {
        value = value * 10 + (*at - '0');
    }
    return (int)value;
}

/* Who has to update, from both sides' levels. */
static inline protocol_compat_t protocol_compat_for(int listener_level, int listener_min_tv,
                                                    int tv_level, int tv_min_listener)
{
    if (listener_level < tv_min_listener) return PROTOCOL_UPDATE_PC;
    if (tv_level < listener_min_tv) return PROTOCOL_UPDATE_TV;
    return PROTOCOL_OK;
}

/* What a STATUS answer says about this TV and that listener. */
static inline protocol_compat_t protocol_compat(const char *status_reply)
{
    if (!status_reply || strncmp(status_reply, "OK", 2) != 0) return PROTOCOL_UNKNOWN;
    return protocol_compat_for(protocol_word(status_reply, " level=", 1),
                               protocol_word(status_reply, " min_tv=", 1),
                               CTM_PROTOCOL_LEVEL, CTM_PROTOCOL_MIN_LISTENER);
}

/* What the panel and the log say, or "" when there is nothing to update. */
static inline const char *protocol_advice(protocol_compat_t c)
{
    switch (c) {
    case PROTOCOL_UPDATE_PC: return "Update DS5-USBIP on the PC: it is too old for this TV app";
    case PROTOCOL_UPDATE_TV: return "Update the TV app: it is too old for DS5-USBIP on the PC";
    default:                 return "";
    }
}

/* The listener's own refusal of an older TV, from a BRIDGE_START. */
static inline bool protocol_refused_tv(const char *reply)
{
    return reply != NULL && strncmp(reply, "ERR update the TV app", 21) == 0;
}

#endif
