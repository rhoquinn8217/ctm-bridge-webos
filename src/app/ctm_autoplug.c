/* Auto-plug policy, UI-free (shared by the standalone app and the fork glue).
 *
 * Bridge every recognised controller as soon as the agent is up. Called from
 * the host app's periodic device refresh, so pads connected mid-session get
 * plugged too. Each device key is attempted only once per process run — a
 * failed plug (or a manual Plug out) is never retried automatically; the
 * manual Plug in stays the fallback. Generic "hid" devices auto-plug only
 * when they are a real mouse/keyboard (item_is_mouse_or_keyboard) or the
 * Magic Remote row, which auto-plugs as the TV-pointer synthesizer
 * (item_is_tv_remote / ctm_tv_pointer_plug); exotic vendor HID stays manual
 * (unknown descriptors can code-10 on the host). */

#include <stdio.h>
#include <string.h>

#include "ctm_state.h"

void ctm_autoplug_tick(void)
{
    static char s_attempted[MAX_DEVICES][96];
    static int s_attempted_count;
    if (!g_agent_online) {
        return;
    }
    for (int i = 0; i < g_devices.count; ++i) {
        logical_device_t *item = &g_devices.items[i];
        const char *kind = bridge_kind_for_item(item);
        /* The once-per-key latch means a Back-hold pointer release is not
         * fought by auto-plug — the row's Plug in button re-bridges. */
        bool remote = item_is_tv_remote(item);
        if (item->plugged || kind == NULL ||
            (!remote && strcmp(kind, "hid") == 0 && !item_is_mouse_or_keyboard(item))) {
            continue;
        }
        bool seen = false;
        for (int j = 0; j < s_attempted_count; ++j) {
            if (strcmp(s_attempted[j], item->key) == 0) {
                seen = true;
                break;
            }
        }
        if (seen || s_attempted_count >= (int)(sizeof s_attempted / sizeof s_attempted[0])) {
            continue;
        }
        snprintf(s_attempted[s_attempted_count++], sizeof s_attempted[0], "%s", item->key);
        if (remote ? ctm_tv_pointer_plug() : plug_in_item(item)) {
            item->plugged = true;
            if (!remote)
                set_plug_key(item->key, true);
            log_append("auto-plug: %s (%s)", item->name, remote ? "tv-pointer" : kind);
        } else {
            log_append("auto-plug failed for %s; use Plug in", item->name);
        }
    }
}
