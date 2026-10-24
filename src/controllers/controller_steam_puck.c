/* Steam Controller ("puck", USB dongle bridged via hidraw — the no-/dev/bus/usb
 * workaround). Classification, and every interface forwarded for the host to
 * build the composite. ⓘ Never written: choosing the gamepad interface
 * (select_node) and leaving lizard mode (on_plug_init). */

#define _GNU_SOURCE

#include "ctm_controller.h"

#include <string.h>

static bool steam_puck_matches(const ctm_controller_dev_t *dev)
{
    return dev &&
           strcmp(dev->vid, "28de") == 0 &&
           strcmp(dev->pid, "1304") == 0;
}

const ctm_controller_ops_t ctm_controller_steam_puck_ops = {
    .kind = "steam_puck",
    .matches = steam_puck_matches,
    .select_node = NULL,    /* not written: pick the gamepad/vendor hidraw interface */
    .on_plug_init = NULL,   /* not written: SET_SETTINGS lizard-mode exit */
    .patch_output = NULL,
    .set_settings = NULL,
    .composite = true,   /* forward all 7 interfaces; host builds the composite */
};
