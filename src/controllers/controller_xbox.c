/* Xbox controller, BT only (works-ish; GIP translation is Windows-side). USB
 * Xbox is the blocked usbip/input-only path and never reaches here. STAGE 1:
 * classification only; on_plug_init reserved for a TV-side BT handshake if one
 * proves necessary. */

#define _GNU_SOURCE

#include "ctm_controller.h"
#include "xbox_bt_report.inl"

#include <stdlib.h>
#include <string.h>

static bool xbox_pid(const char *pid)
{
    static const char *const pids[] = {
        "02d1", "02dd", "02e0", "02e3", "02ea", "02fd",
        "0b00", "0b05", "0b0a", "0b12", "0b13", "0b20",
    };
    for (size_t i = 0; i < sizeof(pids) / sizeof(pids[0]); ++i) {
        if (strcmp(pid, pids[i]) == 0) return true;
    }
    return false;
}

bool xbox_known_pid(const char *pid)
{
    return pid && xbox_pid(pid);
}

static bool xbox_matches(const ctm_controller_dev_t *dev)
{
    return dev &&
           strcmp(dev->vid, "045e") == 0 &&
           strcmp(dev->bus, "BT") == 0 &&
           (xbox_pid(dev->pid) || (dev->name[0] && strcasestr(dev->name, "xbox")));
}

/* ⭐ Every report the pad sends passes here as the pad sent it, whether or not
 * the overlay holds input. The input thread both keeps it and sends it again
 * (send_keepalive), so nothing here needs a lock. */
static void xbox_on_input_report(ctm_controller_t *c, const uint8_t *data, size_t len)
{
    xbox_bt_last_t *last = (xbox_bt_last_t *)controller_type_ctx(c);
    if (!last) {
        last = (xbox_bt_last_t *)calloc(1, sizeof(*last));
        if (!last) return;
        controller_set_type_ctx(c, last);
    }
    (void)xbox_bt_keep(last, data, len);
}

static int xbox_current_report(ctm_controller_t *c, uint8_t *report, size_t cap)
{
    return (int)xbox_bt_current((const xbox_bt_last_t *)controller_type_ctx(c), report, cap);
}

const ctm_controller_ops_t ctm_controller_xbox_ops = {
    .kind = "xbox",
    /* ⛔ Not grabbed: SDL may read a Bluetooth Xbox pad through evdev, and the
     * overlay combo must keep working while it is bridged -- see
     * controller_xpad.c. The handover keeps it off the host twice. */
    .grab_evdev = false,
    .matches = xbox_matches,
    .select_node = NULL,
    .on_plug_init = NULL,   /* STAGE 2: reserved for BT init/handshake if needed */
    .patch_output = NULL,   /* none: verbatim relay, Windows map does GIP */
    .set_settings = NULL,
    .on_input_report = xbox_on_input_report,
    .blank_input = xbox_bt_blank_report,
    .current_report = xbox_current_report,
    /* ⭐⭐ THE PAD'S LAST STATE AGAIN EVERY 4 MS WHEN IT SENDS NOTHING
     * (rhoquinn8217, 2026-09-16: a still pad left the bridge after 15 s). Over
     * Bluetooth it reports only on a change, so the listener's 15 s silence
     * limit took a pad that had been put down, and a stick held still would
     * stall its scroll the way a cabled pad's did before 4 ms (controller_xpad.c).
     * See xbox_bt_report.inl. */
    .keepalive_ms = 4,
};
