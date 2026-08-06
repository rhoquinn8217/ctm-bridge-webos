/* DualSense (DS5) controller, BT. Classification + the 0x36/0x32 BT output
 * patching (audio route / volume / latency / haptics) ported verbatim from
 * tv_bridge_worker.c's apply_ds5_settings, now the patch_output hook. */

#define _GNU_SOURCE

#include "ctm_controller.h"

#include <math.h>
#include <string.h>
#include <time.h>

/* matches: claim the DualSense over BT or USB. When: factory classification. */
static bool ds5_matches(const ctm_controller_dev_t *dev)
{
    return dev &&
           strcmp(dev->vid, "054c") == 0 &&
           strcmp(dev->pid, "0ce6") == 0 &&
           (strcmp(dev->bus, "BT") == 0 || strcmp(dev->bus, "USB") == 0);
}

/* Map an audio mode to the DS5 BT 0x36 sub-block header byte. */
static uint8_t ds5_audio_block_for_mode(tv_bridge_audio_mode_t mode)
{
    switch (mode) {
        case TV_BRIDGE_AUDIO_SPEAKER: return 0x93;
        case TV_BRIDGE_AUDIO_HEADSET: return 0x96;
        case TV_BRIDGE_AUDIO_BOTH: return 0x95;
        case TV_BRIDGE_AUDIO_OFF:
        default: return 0x00;
    }
}

/* Perceptual haptics-gain curve (1.0 == unity), clamped 0..5. */
static double ds5_haptics_gain(unsigned int gain_centi)
{
    double gain = (double)gain_centi / 100.0;
    if (gain <= 0.0) return 0.0;
    if (gain >= 5.0) return 5.0;
    if (gain <= 1.0) return gain;
    return 1.0 + pow((gain - 1.0) / 4.0, 1.35) * 4.0;
}

/* Clamp a volume percent to the DS5 raw byte range (0..0x64). */
static uint8_t ds5_volume_raw_byte(unsigned int value)
{
    return (uint8_t)(value > 0x64u ? 0x64u : value);
}

/* patch_output: rewrite a DS5 0x36/0x32 BT output report in place per the live
 * settings — audio route (0x9x), volume + audio-ctrl bits (0x90), latency
 * (0x91), haptics gain (0x92) — then re-CRC. AUTO touches only the latency
 * block. When: every outbound report, from the pump. Returns 0 (never drops). */
static int ds5_patch_output(ctm_controller_t *c, uint8_t *data, size_t *len_io)
{
    tv_bridge_worker_settings_t s;
    ctm_controller_get_settings(c, &s);
    const tv_bridge_worker_settings_t *settings = &s;

    size_t len = len_io ? *len_io : 0;
    if (!data || len < 12 || (data[0] != 0x36 && data[0] != 0x32)) return 0;

    int patched = 0;
    size_t pos = 2;
    size_t limit = len - 4;

    if (settings->audio_mode == TV_BRIDGE_AUDIO_AUTO) {
        uint8_t auto_latency = (uint8_t)settings->latency_ms;
        if (auto_latency < 20) auto_latency = 20;
        while (pos + 2 <= limit) {
            uint8_t block_id = data[pos];
            size_t payload_len = data[pos + 1];
            size_t block_len = payload_len + 2;
            if (block_id == 0 && payload_len == 0) break;
            if (block_len > limit - pos) break;
            if (block_id == 0x91 && payload_len >= 6) {
                for (size_t i = 3; i <= 7; ++i) {
                    if (data[pos + i] != auto_latency) {
                        data[pos + i] = auto_latency;
                        patched = 1;
                    }
                }
            }
            pos += block_len;
        }
        if (patched) ctm_bt_sign_output(data, len);
        return 0;
    }

    uint8_t audio_block = ds5_audio_block_for_mode(settings->audio_mode);
    uint8_t latency = (uint8_t)settings->latency_ms;
    uint8_t headset_volume = ds5_volume_raw_byte(settings->headset_volume_percent);
    uint8_t speaker_volume = ds5_volume_raw_byte(settings->speaker_volume_percent);
    uint8_t target_headset_volume = 0;
    uint8_t target_speaker_volume = 0;
    uint8_t target_audio_flags = 0;
    if (latency < 20) latency = 20;

    switch (settings->audio_mode) {
        case TV_BRIDGE_AUDIO_HEADSET:
            target_headset_volume = headset_volume;
            break;
        case TV_BRIDGE_AUDIO_SPEAKER:
            target_speaker_volume = speaker_volume;
            target_audio_flags = 0x30;
            break;
        case TV_BRIDGE_AUDIO_BOTH:
            target_headset_volume = headset_volume;
            target_speaker_volume = speaker_volume;
            target_audio_flags = 0x30;
            break;
        case TV_BRIDGE_AUDIO_OFF:
        default:
            break;
    }

    while (pos + 2 <= limit) {
        uint8_t block_id = data[pos];
        size_t payload_len = data[pos + 1];
        size_t block_len = payload_len + 2;
        if (block_id == 0 && payload_len == 0) break;
        if (block_len > limit - pos) break;

        if (block_id == 0x90 && payload_len >= 8) {
            /* Only patch confirmed audio fields; preserve effect/rumble bytes. */
            if ((data[pos + 2] & 0xb0u) != 0xb0u) {
                data[pos + 2] = (uint8_t)(data[pos + 2] | 0xb0u);
                patched = 1;
            }
            if ((data[pos + 3] & 0x80u) != 0x80u) {
                data[pos + 3] = (uint8_t)(data[pos + 3] | 0x80u);
                patched = 1;
            }
            if (data[pos + 6] != target_headset_volume) {
                data[pos + 6] = target_headset_volume;
                patched = 1;
            }
            if (data[pos + 7] != target_speaker_volume) {
                data[pos + 7] = target_speaker_volume;
                patched = 1;
            }
            if (data[pos + 9] != target_audio_flags) {
                data[pos + 9] = target_audio_flags;
                patched = 1;
            }
        } else if ((block_id == 0x93 || block_id == 0x94 || block_id == 0x95 || block_id == 0x96) && audio_block != 0) {
            if (data[pos] != audio_block) {
                data[pos] = audio_block;
                patched = 1;
            }
        } else if (block_id == 0x91 && payload_len >= 6) {
            for (size_t i = 3; i <= 7; ++i) {
                if (data[pos + i] != latency) {
                    data[pos + i] = latency;
                    patched = 1;
                }
            }
        } else if (block_id == 0x92 && payload_len >= 2 && settings->haptics_gain_centi != 100) {
            double gain = ds5_haptics_gain(settings->haptics_gain_centi);
            for (size_t i = 2; i < block_len; ++i) {
                int sample = (int)(int8_t)data[pos + i];
                int scaled = (int)lrint((double)sample * gain);
                if (scaled < -128) scaled = -128;
                if (scaled > 127) scaled = 127;
                uint8_t value = (uint8_t)(int8_t)scaled;
                if (data[pos + i] != value) {
                    data[pos + i] = value;
                    patched = 1;
                }
            }
        }
        pos += block_len;
    }

    if (patched) ctm_bt_sign_output(data, len);
    return 0;
}

/* matches: claim the DualSense Edge over BT or USB. A separate device from the
 * base DualSense so it can be presented to the host as itself, but it shares
 * every behaviour below -- its reports are shaped identically, so a second copy
 * of patch_output would only drift out of step with this one. When: factory
 * classification. */
static bool ds5e_matches(const ctm_controller_dev_t *dev)
{
    return dev &&
           strcmp(dev->vid, "054c") == 0 &&
           strcmp(dev->pid, "0df2") == 0 &&
           (strcmp(dev->bus, "BT") == 0 || strcmp(dev->bus, "USB") == 0);
}

/* Local gesture: hold TWO fingers on the touchpad AND press it down, for
 * CHORD_HOLD_MS, to unplug this controller without the overlay.
 *
 * Byte offsets measured from real hardware 2026-08-05 (DualSense and Edge, both
 * identical), wired input report 0x01:
 *   byte 10  bit 1     touchpad pressed
 *   byte 33  bit 7 clear   first touch point active
 *   byte 37  bit 7 clear   second touch point active
 * An inactive touch point reads 0x80; a finger clears the top bit and the low
 * bits become a touch counter, so only the top bit is meaningful here.
 *
 * The gesture is deliberately NOT swallowed -- the report is relayed unchanged.
 * Two fingers plus a press is not a combination games ask for, so the safer
 * choice is to stay a pure relay. */
#define DS5_CHORD_HOLD_MS 2000

static bool ds5_chord_held(const uint8_t *data, size_t len)
{
    if (!data || len < 41 || data[0] != 0x01) {
        return false;
    }
    bool pressed = (data[10] & 0x02) != 0;
    bool finger1 = (data[33] & 0x80) == 0;
    bool finger2 = (data[37] & 0x80) == 0;
    return pressed && finger1 && finger2;
}

static uint64_t ds5_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* on_input_report: watch for the unplug gesture. When: this controller's input
 * thread, once per relayed report. The timestamp lives in the controller's own
 * type state, never in a file-level variable -- two controllers each have their
 * own thread calling this. */
static void ds5_on_input_report(ctm_controller_t *c, const uint8_t *data, size_t len)
{
    if (!c) return;

    if (!ds5_chord_held(data, len)) {
        /* Released: re-arm. */
        ctm_controller_set_type_state(c, 0);
        return;
    }

    uint64_t since = ctm_controller_type_state(c);
    uint64_t now = ds5_now_ms();

    if (since == 0) {
        ctm_controller_set_type_state(c, now);
        return;
    }
    /* Already fired for this hold: wait for the fingers to lift. */
    if (since == UINT64_MAX) {
        return;
    }
    if (now - since >= DS5_CHORD_HOLD_MS) {
        ctm_controller_set_type_state(c, UINT64_MAX);
        ctm_controller_request_unplug(c);
    }
}

const ctm_controller_ops_t ctm_controller_ds5_ops = {
    .kind = "ds5",
    .needs_host_config = true,
    .grab_evdev = true,
    .request_bt_mode = true,
    .matches = ds5_matches,
    .select_node = NULL,
    .on_plug_init = NULL,
    .on_input_report = ds5_on_input_report,
    .patch_output = ds5_patch_output,
    .set_settings = NULL,   /* live values read via get_settings in patch_output */
};

/* DualSense Edge. Same behaviour as the base DualSense -- deliberately sharing
 * its hooks rather than duplicating them -- but its own entry in the factory so
 * it is identified as itself and can diverge later without untangling. */
const ctm_controller_ops_t ctm_controller_ds5e_ops = {
    .kind = "ds5e",
    .needs_host_config = true,
    .grab_evdev = true,
    .request_bt_mode = true,
    .matches = ds5e_matches,
    .select_node = NULL,
    .on_plug_init = NULL,
    .on_input_report = ds5_on_input_report,
    .patch_output = ds5_patch_output,
    .set_settings = NULL,
};
