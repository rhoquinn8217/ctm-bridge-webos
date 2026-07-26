/* DualShock 4 (DS4) controller, BT. Classification + Layout B output
 * patching. The service-side map emits Layout B frames: 0x11 effect reports
 * (rumble/LED + volume bytes) and pure-audio reports 0x12/0x14/0x17 whose
 * route byte sits at offset 5 (probed bitmask: 0xFF = stereo headphones,
 * 0xDF = split ch0->speaker ch1->headphone). This hook runs AFTER the map's
 * translation, so forcing here wins without fighting the map. */

#define _GNU_SOURCE

#include "ctm_controller.h"

#include <string.h>

/* matches: claim the DualShock 4 (either PID) over BT. When: classification. */
static bool ds4_matches(const ctm_controller_dev_t *dev)
{
    return dev &&
           strcmp(dev->vid, "054c") == 0 &&
           (strcmp(dev->pid, "09cc") == 0 || strcmp(dev->pid, "05c4") == 0) &&
           strcmp(dev->bus, "BT") == 0;
}

/* Layout B route byte (audio frame offset 5) for a forced mode; 0 = no
 * forcing (AUTO leaves the map's jack auto-route in charge). */
static uint8_t ds4_route_for_mode(tv_bridge_audio_mode_t mode)
{
    switch (mode) {
        case TV_BRIDGE_AUDIO_HEADSET: return 0xff;  /* stereo headphones */
        case TV_BRIDGE_AUDIO_BOTH: return 0xdf;     /* split: speaker + headphone-L */
        default: return 0x00;
    }
}

/* Clamp a volume value to the DS4 raw byte range (0..0x4F firmware ceiling
 * per the controller wiki). */
static uint8_t ds4_volume_raw_byte(unsigned int value)
{
    return (uint8_t)(value > 0x4fu ? 0x4fu : value);
}

/* patch_output: Layout B, in place, then re-CRC.
 * - 0x12/0x14/0x17 pure-audio frames: force the route byte [5] when the mode
 *   is Headphones (0xFF) or Split (0xDF); AUTO passes through — the map's
 *   auto_route already wrote it from the jack bit.
 * - 0x11 effect frames: volume bytes BT[21]=headphone-L, BT[22]=headphone-R,
 *   BT[24]=speaker are always the sliders' (TV owns volume; the pad persists
 *   whatever was set last, so an explicit value every frame is the sane
 *   default the user asked for). Rumble/LED bytes untouched.
 * When: every outbound report, from the pump. Returns 0 (never drops). */
static int ds4_patch_output(ctm_controller_t *c, uint8_t *data, size_t *len_io)
{
    tv_bridge_worker_settings_t s;
    ctm_controller_get_settings(c, &s);
    const tv_bridge_worker_settings_t *settings = &s;

    size_t len = len_io ? *len_io : 0;
    if (!data || len < 10) return 0;

    int patched = 0;

    if (data[0] == 0x12 || data[0] == 0x14 || data[0] == 0x17) {
        uint8_t route = ds4_route_for_mode(settings->audio_mode);
        if (route != 0 && data[5] != route) {
            data[5] = route;
            patched = 1;
        }
    } else if (data[0] == 0x11 && len >= 30) {
        uint8_t headset_volume = ds4_volume_raw_byte(settings->headset_volume_percent);
        uint8_t speaker_volume = ds4_volume_raw_byte(settings->speaker_volume_percent);
        /* BT[3] high bits are the volume-valid flags: 0x10/0x20 = headphone
         * L/R, 0x80 = speaker. Without them the pad ignores bytes 21/22/24.
         * The low nibble (rumble/LED/flash valid) stays the game's. */
        uint8_t valid_byte = (uint8_t)(data[3] | 0xb0u);
        if (data[3] != valid_byte) {
            data[3] = valid_byte;
            patched = 1;
        }
        if (data[21] != headset_volume) {
            data[21] = headset_volume;
            patched = 1;
        }
        if (data[22] != headset_volume) {
            data[22] = headset_volume;
            patched = 1;
        }
        if (data[24] != speaker_volume) {
            data[24] = speaker_volume;
            patched = 1;
        }
    }

    if (patched) ctm_bt_sign_output(data, len);
    return 0;
}

const ctm_controller_ops_t ctm_controller_ds4_ops = {
    .kind = "ds4",
    .needs_host_config = true,
    .grab_evdev = true,
    .request_bt_mode = true,
    .matches = ds4_matches,
    .select_node = NULL,
    .on_plug_init = NULL,
    .patch_output = ds4_patch_output,
    .set_settings = NULL,   /* live values read via get_settings in patch_output */
};
