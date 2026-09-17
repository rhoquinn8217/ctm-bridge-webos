/* Generic HID controller — verbatim relay, and the factory fallback. Its
 * matches() is always true so it claims any device no specific type took. */

#define _GNU_SOURCE

#include "ctm_controller.h"

static bool generic_matches(const ctm_controller_dev_t *dev)
{
    (void)dev;
    return true;
}

const ctm_controller_ops_t ctm_controller_generic_ops = {
    .kind = "generic",
    /* A bridged keyboard must stop typing on the TV too. ⛔ But a gamepad is
     * left alone: SDL must still see it for the overlay combo, and the
     * handover keeps it off the host twice. */
    .grab_evdev = true,
    .grab_skips_gamepads = true,
    .matches = generic_matches,
    .select_node = NULL,
    .on_plug_init = NULL,
    .patch_output = NULL,   /* verbatim */
    .set_settings = NULL,
    /* ⭐⭐ A STILL GAMEPAD'S STATE AGAIN AFTER 50 MS WITH NOTHING SENT
     * (rhoquinn8217, 2026-09-16: a GameSir in its Android mode left the bridge
     * 15 s after its last press). A pad like it reports only on a change, and
     * the listener drops a gamepad that is silent for 15 s. The pump sends its
     * last report, or one with nothing pressed until it has sent one
     * (pad_blank.inl); a keyboard, a mouse or anything else has none to send.
     * ⓘ 50 ms: never between the reports of a pad that streams (a Pro
     * Controller's come every 15 ms), and soon enough that a button held still
     * is let go on the host as the overlay opens, and back when it closes.
     * ⚠️ The Xbox types' 4 ms is for the listener's cursor, scroll and turbo,
     * which do not reach a generic pad yet; look at this again when they do. */
    .keepalive_ms = 50,
};
