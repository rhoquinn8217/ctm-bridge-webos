/* The shapes a signal's light takes, as brightness over time, for every pad
 * whose light the TV draws itself. One file, so one pad cannot mean something
 * different from another (rhoquinn8217, 2026-10-08):
 *
 *   bridged      green, solid with the tone, then BREATHING until the PC
 *                takes the light: signal_ready_level()
 *   handed back  yellow, three quick flashes: signal_flash_level()
 *   refused      red, three quick flashes
 *
 * ⭐⭐ THE BREATHING SAYS "BRIDGED, AND NOTHING HAS TAKEN IT YET".
 * rhoquinn8217: *"I want a continuous breathing to show that it's ready and
 * allow anything to take it, most likely steam. But I want it to signify it's
 * already bridge but nothing has taken it yet."* It runs from the session
 * (ready_light_tick, controller_common.c) and stops at the first report from
 * the PC that sets the light.
 *
 * ⓘ The flashes were three different things before: two yellow breaths on a
 * cabled DualSense, three very fast flashes over Bluetooth, three breaths on a
 * DS4, and three slow flashes from the app (code review, 2026-10-05). The
 * user chose three QUICK flashes, for the release and in red for a refusal.
 *
 * ⓘ Integer arithmetic only: the tests link nothing but libc. */

#ifndef SIGNAL_SHAPE_INL
#define SIGNAL_SHAPE_INL

#define SIGNAL_FLASHES          3
#define SIGNAL_FLASH_MS         750    /* three flashes, each 125 ms on and 125 off */
#define SIGNAL_READY_BREATH_MS  2000   /* one slow breath */
#define SIGNAL_READY_FLOOR      24     /* never quite dark: breathing, not blinking */
#define SIGNAL_READY_STEP_MS    50     /* one light report per step while it breathes */

/* Hard flashes: `count` of them across `total_ms`, each on for the first half
 * of its share of the time. 0 before and after. */
static inline int signal_flash_level(long at_ms, long total_ms, int count)
{
    if (at_ms < 0 || total_ms <= 0 || count <= 0 || at_ms >= total_ms) return 0;
    const long slot = total_ms / count;
    if (slot <= 1) return 0;
    return (at_ms % slot) < slot / 2 ? 255 : 0;
}

/* The ready light, `at_ms` after it started: full at the start (the bridge's
 * own green), then a smooth fall to the floor and back over one breath, for as
 * long as it is asked. */
static inline int signal_ready_level(long at_ms)
{
    if (at_ms < 0) at_ms = 0;
    const long period = SIGNAL_READY_BREATH_MS;
    const long x = at_ms % period;
    /* 1024 at either end of the breath, 0 in its middle. */
    const long tri = ((2 * x >= period) ? (2 * x - period) : (period - 2 * x)) * 1024 / period;
    /* Eased (3t^2 - 2t^3), so it lingers at the top and the bottom as a
     * breath does, rather than bouncing off them. In 1024ths. */
    const long eased = (tri * tri / 1024) * (3 * 1024 - 2 * tri) / 1024;
    return SIGNAL_READY_FLOOR + (int)(((255 - SIGNAL_READY_FLOOR) * eased) / 1024);
}

#endif
