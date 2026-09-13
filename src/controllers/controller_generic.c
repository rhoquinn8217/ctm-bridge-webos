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
};
