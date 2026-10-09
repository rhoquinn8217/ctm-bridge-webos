/* How long a bridged pad's link may be in trouble before the pad goes back to
 * the TV.
 *
 * ⛔ A REFUSED GREETING STRANDED THE PAD (code review, 2026-10-05). The
 * session gave up after CTM_HOST_GONE_MS of FAILED CONNECTS, and a connect
 * that succeeded started the count again. A listener that took the connection
 * and then never answered the greeting -- a port with nothing behind it, a
 * session it no longer had -- let every connect succeed: the TV sent HELLO,
 * waited out its 5 s for the host's settings, dialled again, and so on for the
 * whole stream, with the row "bridged" and nothing reaching the PC.
 * ➡️ The clock now starts at the first failure of any kind since the listener
 * last answered a greeting, and only an answered greeting stops it. Its own
 * file so a test can pin it.
 *
 * ⭐ AND NO REDIAL WAITS PAST THE GIVE-UP (2026-10-08): link_trouble_left_ms
 * is what the next connect may wait, so the pad goes back at the give-up and
 * not a whole connect after it. */

#include <stdint.h>

typedef struct {
    uint64_t since_us;   /* the first failure since the last answered greeting; 0 = none */
    int unanswered;      /* greetings not answered in that time */
} link_trouble_t;

/* A connection is being tried: start the clock unless it is running already. */
static inline void link_trouble_begin(link_trouble_t *t, uint64_t now_us)
{
    if (t->since_us == 0) t->since_us = now_us ? now_us : 1;
}

/* The listener answered a greeting: the link is good, and the clock stops. */
static inline void link_trouble_clear(link_trouble_t *t)
{
    t->since_us = 0;
    t->unanswered = 0;
}

/* How long the trouble has lasted, in ms; 0 when there is none. */
static inline long link_trouble_ms(const link_trouble_t *t, uint64_t now_us)
{
    if (t->since_us == 0 || now_us <= t->since_us) return 0;
    return (long)((now_us - t->since_us) / 1000u);
}

/* What is left of gone_ms before the pad goes back, in ms; 0 = nothing left,
 * give up. */
static inline long link_trouble_left_ms(const link_trouble_t *t, uint64_t now_us, long gone_ms)
{
    const long left = gone_ms - link_trouble_ms(t, now_us);
    return left > 0 ? left : 0;
}
