/* DualSense (DS5) controller, BT. Classification + the 0x36/0x32 BT output
 * patching (audio route / volume / latency / haptics) ported verbatim from
 * tv_bridge_worker.c's apply_ds5_settings, now the patch_output hook. */

#define _GNU_SOURCE

#include "ctm_controller.h"

/* ⚠️ snprintf needs this, and the file went without it for weeks -- it was
 * arriving through some other header until a 2026-08-19 upstream merge changed
 * what that header pulls in. Included directly so it cannot happen again. */
#include <stdio.h>
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

/* Light-show diagnostics. The relay path runs up to a thousand times a second,
 * so this logs the FIRST few reports of a show and nothing after -- enough to
 * see what arrived and what was written, without writing a file at input rate.
 * Uses the controller's own log, which lands in /tmp/ctm-<mac-or-kind>.log. */

/* Which scratch slot each feature uses. Per controller, never shared. */
/* We write no lightbar patterns. The light belongs, in order, to the player's
 * own config, then the game, then Steam, then Windows -- and every moment we
 * might have wanted to signal something is a moment one of them may be driving
 * it. The confirmation show that used to live here also suppressed the host's
 * lightbar bytes while it played, which is this layer overriding the game.
 *
 * What a user sees on a successful plug is Steam taking the light over, which
 * is both more recognisable and the honest signal: the controller is behaving
 * like a natively connected one. Confirmation of our own belongs on the
 * speaker and the motors, which nothing else claims.
 *
 * Removing the show frees two of the three type-state slots. */
#define DS5_SLOT_CHORD 0

/* Waiting for the host's first output report to start the clock. */

static uint64_t ds5_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* Map an audio mode to the DS5 BT 0x36 sub-block header byte. */
/* Speaker routing WITH echo cancellation, for the Bluetooth report's audio
 * control byte.
 *
 * WHY THE `0x0c` MATTERS, and it is not cosmetic: the DualSense suppresses its
 * own speaker whenever echo cancellation is off, because the microphone sits
 * beside the speaker and would feed back. Routing alone (`0x30`) produces a
 * whisper; routing plus cancellation (`0x3c`) produces full output.
 *
 * This was `0x30` here, which is the same fault the wired path had until
 * 2026-07-22. Measured then on C1 with an 800 Hz tone and no game running:
 * `0x55`/`0x30` a whisper, `0x64`/`0x30` 80 dB, `0x64`/`0x3c` 94 dB. A game
 * launch repaired it because games send `0x3c` themselves -- which is exactly
 * the symptom seen on Bluetooth on 2026-08-11: quiet at bridging, silent from
 * a browser, correct once a game had started, and correct afterwards.
 *
 * Same two bits, same fix, the other transport. */
#define DS5_BT_AUDIO_OUT_PATH_SPEAKER  0x30
/* ⭐⭐ ECHO CANCEL ONLY -- NOISE CANCEL IS DELIBERATELY OFF (2026-08-23).
 *
 * ⓘ Byte 8 packs two independent switches: bit 2 echo cancel, bit 3 noise
 * cancel. They were set together because games send 0x3c, and only ONE of them
 * was ever justified.
 *
 * ⭐ ECHO CANCEL (bit 2) IS PROVEN NECESSARY. The controller suppresses its own
 * speaker without it -- feedback protection, the mic sits centimetres away.
 * Measured on a C1: attenuated -> 80 dB -> 94 dB with this bit alone, volume
 * held constant.
 *
 * ⛔ NOISE CANCEL (bit 3) WAS NEVER TESTED, and it is beamforming: on a
 * MICROPHONE ARRAY it combines the capsules to isolate one voice and suppress
 * the rest. ⚠️ MEASURED 2026-08-23 on the Monitor -- through our capture path
 * ch0 peaked at 4-31 while ch1 reached 1369, and `arecord` on the same
 * controller minutes later gave ch0 740 and ch1 1939. `arecord` sends no such
 * report. ➡️ One channel effectively cancelled away, and the other quiet.
 *
 * ⚠️ IF THE SPEAKER ATTENUATES AGAIN, PUT BIT 3 BACK AND SAY SO HERE -- that
 * would mean the two bits are not independent after all, which the staged
 * measurement suggests they are but never proved. */
/* ⛔⛔ BLUETOOTH KEEPS BIT 3. WIRED DOES NOT. Measured 2026-08-24.
 *
 * ⚠️ THE TWO BITS ARE INDEPENDENT ON A CABLE AND NOT OVER BLUETOOTH. Dropping
 * noise cancel fixed the microphone's dead channel on wired, and the speaker
 * stayed nominal -- so it was applied to both transports. ⛔ On Bluetooth it
 * ATTENUATES THE SPEAKER, and the haptics with it, because over that transport
 * the speaker and the haptics ride the same stream.
 *
 * ⭐ Bisected on hardware: build 260 nominal, 261 attenuated. 261 changed this
 * byte and nothing else.
 *
 * ⓘ WHY IT WAS MISSED: the staged measurement that isolated echo cancel as the
 * speaker lever was done on a C1 OVER A CABLE, and the re-check after the
 * change was wired too. **Neither bit was ever tested independently on
 * Bluetooth.**
 *
 * ⚠️ AND THE MICROPHONE FIX DOES NOT TRANSFER. Bluetooth capture goes through
 * the controller's own encoder rather than an ALSA device, so the array is not
 * read the same way and the dead-channel fault does not apply here. */
#define DS5_BT_AUDIO_ECHO_NOISE_CANCEL 0x0c  /* bits 2 AND 3 -- Bluetooth needs both */
#define DS5_BT_AUDIO_SPEAKER_ON \
    (DS5_BT_AUDIO_OUT_PATH_SPEAKER | DS5_BT_AUDIO_ECHO_NOISE_CANCEL)

/* One Opus frame at the settings the tone is encoded with: 48 kHz, 10 ms,
 * 160 kbps constant bitrate. Named here rather than including the generated
 * blob, which this file has no other need of. */
#define FEEDBACK_OPUS_FRAME_BYTES_EXPECTED 200

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

    /* ⭐ THE SHAPE OF THE FIRST FEW REPORTS, AND THEN NOTHING.
     *
     * ⛔ An earlier version of this only logged once an AUDIO block appeared,
     * and built a 512-byte string on EVERY report until then. No report ever
     * had one, so it ran on all of them and the extra work killed the link --
     * the controller powered itself off. Measured 2026-08-19, build 176.
     *
     * ⚠️ So: a hard count, decremented on every call, and NOTHING is built once
     * it reaches zero. Whatever the first few reports look like is what we get.
     *
     * ⭐ Why it is wanted: the TV's own Bluetooth signal invents a 398-byte
     * report with ONE audio block, so it needs 100 reports a second. The link
     * delivers about 25. If the host's reports carry four frames each, that is
     * the shape to copy. */
    {
        static int s_shape_left = 3;
        if (s_shape_left > 0) {
            --s_shape_left;
            char line[400];
            int n = snprintf(line, sizeof(line), "report_shape: id=0x%02x len=%zu:", data[0], len);
            size_t p2 = 2;
            while (p2 + 2 <= len - 4 && n > 0 && (size_t)n < sizeof(line) - 24) {
                uint8_t bid = data[p2];
                size_t blen = data[p2 + 1];
                if (bid == 0x00) break;
                n += snprintf(line + n, sizeof(line) - n, " [%02x len=%zu at=%zu]", bid, blen, p2);
                p2 += blen + 2;
            }
            ctl_log(c, "%s", line);

            /* ⭐⭐ AND THE BYTES OF THE BLOCKS WE FILL OURSELVES.
             *
             * The host's reports and ours have the SAME shape -- same id, same
             * length, same blocks at the same offsets. Yet the host's flow at
             * full rate and ours take 89 ms each and kill the link. So the
             * difference is in the CONTENTS, and these are the three blocks
             * where we choose values rather than copy them.
             *
             * ⓘ Audio payload is skipped: it is Opus, and 200 bytes of it says
             * nothing we can read. */
            char hx[300];
            int m = 0;
            m += snprintf(hx + m, sizeof(hx) - m, "host_bytes: hdr %02x %02x |", data[0], data[1]);
            m += snprintf(hx + m, sizeof(hx) - m, " 90:");
            for (int k = 0; k < 12 && (size_t)(2 + k) < len; ++k)
                m += snprintf(hx + m, sizeof(hx) - m, " %02x", data[2 + k]);
            m += snprintf(hx + m, sizeof(hx) - m, " | 91:");
            for (int k = 0; k < 9 && (size_t)(67 + k) < len; ++k)
                m += snprintf(hx + m, sizeof(hx) - m, " %02x", data[67 + k]);
            m += snprintf(hx + m, sizeof(hx) - m, " | 95hdr:");
            for (int k = 0; k < 4 && (size_t)(76 + k) < len; ++k)
                m += snprintf(hx + m, sizeof(hx) - m, " %02x", data[76 + k]);
            m += snprintf(hx + m, sizeof(hx) - m, " | 92hdr:");
            for (int k = 0; k < 6 && (size_t)(278 + k) < len; ++k)
                m += snprintf(hx + m, sizeof(hx) - m, " %02x", data[278 + k]);
            m += snprintf(hx + m, sizeof(hx) - m, " | tail:");
            for (int k = 4; k >= 1; --k)
                m += snprintf(hx + m, sizeof(hx) - m, " %02x", data[len - k]);
            ctl_log(c, "%s", hx);
        }
    }

    int patched = 0;
    size_t pos = 2;
    size_t limit = len - 4;

    if (settings->audio_mode == TV_BRIDGE_AUDIO_AUTO) {
        uint8_t auto_latency = (uint8_t)settings->latency_ms;
        uint8_t auto_speaker = ds5_volume_raw_byte(settings->speaker_volume_percent);
        /* ⛔ The 20 floor was removed 2026-08-16 so 0 can be tested. It was
         * inherited with the slider and never explained -- see the note on the
         * slider itself in ui_window_ds5.c. */
        while (pos + 2 <= limit) {
            uint8_t block_id = data[pos];
            size_t payload_len = data[pos + 1];
            size_t block_len = payload_len + 2;
            if (block_id == 0 && payload_len == 0) break;
            if (block_len > limit - pos) break;
            if (ctm_controller_tone_pending(c) &&
                (block_id == 0x93 || block_id == 0x94 ||
                 block_id == 0x95 || block_id == 0x96)) {
                /* A CONFIRMATION TONE, ONE OPUS FRAME PER REPORT.
                 *
                 * Only when the block is exactly one frame wide. Opus frames
                 * are not a byte stream and cannot be split or padded, so a
                 * block of any other size is left alone rather than filled
                 * with something the controller's decoder would reject. */
                if ((int)payload_len == FEEDBACK_OPUS_FRAME_BYTES_EXPECTED &&
                    ctm_controller_tone_take(c, &data[pos + 2],
                                             (int)payload_len) > 0) {
                    patched = 1;
                }
            }
            if (BT_FEAT_LATENCY && block_id == 0x91 && payload_len >= 6) {
                for (size_t i = 3; i <= 7; ++i) {
                    if (data[pos + i] != auto_latency) {
                        data[pos + i] = auto_latency;
                        patched = 1;
                    }
                }
            } else if (BT_FEAT_AUDIO && block_id == 0x90 && payload_len >= 8) {
                /* AUTO MEANS "FOLLOW THE HOST" -- AND THE HOST ASKS FOR
                 * NOTHING.
                 *
                 * Measured on C3, 2026-08-11, build 78: every report arrives
                 * with speaker volume 0 and audio control 0. So AUTO passed
                 * zeros straight through and the controller was SILENT -- in
                 * the DEFAULT mode, which is the first thing a user meets.
                 *
                 * And nothing upstream ever fills them in. The listener only
                 * learns a speaker volume when Windows sends a USB Audio
                 * Class volume message, which does not happen unless someone
                 * moves that device's slider in Windows -- and doing so was
                 * measured to have no effect here anyway.
                 *
                 * So: when the host has asked for nothing, supply the same
                 * defaults the wired path uses. When it HAS asked for
                 * something, leave it entirely alone -- that is what AUTO is
                 * for, and this must not become a second Speaker mode.
                 *
                 * Only the bits actually written are claimed. Claiming a
                 * field and then leaving the host's zero in it is how the
                 * headphone route ends up muted -- the documented trap from
                 * the wired investigation: "set the audio allow bits that
                 * match the bytes being patched". */
                if (settings->host_audio_set) {
                    /* ⭐⭐ THE USER'S CONFIGURED VOLUME WINS, EVEN IN AUTO.
                     * T-130, 2026-08-25.
                     *
                     * ⛔ THE FAULT: `speaker_volume = 0` on Windows left a
                     * Bluetooth controller at FULL VOLUME. Heard, not inferred.
                     *
                     * ⓘ Two separate things had to be true for that. Windows
                     * patches these settings into report 0x02 -- the WIRED id --
                     * so over Bluetooth its own override never ran and the
                     * report left the host carrying the GAME's 0x64. And then
                     * the branch below, correctly, left it alone: AUTO only
                     * fills in what the host left blank.
                     *
                     * ⚠️ AUTO WAS WRITTEN WHEN THERE WAS ONE HOST. There are now
                     * two things asking -- the game, and the user's
                     * per-controller configuration. **A user who sets a volume
                     * means it.**
                     *
                     * ⭐ So a CONFIGURED volume is written unconditionally, which
                     * is exactly how latency already behaves in this same
                     * branch. ⓘ The two were never symmetrical and the header
                     * note says so: AUTO *"touches only the latency block"*.
                     *
                     * ⛔ ONLY THE VOLUME. The audio ROUTE is not touched here --
                     * that is what the explicit modes are for, and changing it
                     * would make AUTO the second Speaker mode this file warns
                     * against.
                     *
                     * ⚠️ AND THE CLAIM BIT MATTERS. Writing a field without
                     * claiming it does nothing; claiming one and leaving the
                     * host's zero in it is the documented trap from the wired
                     * investigation. So 0x20 is set, and only 0x20. */
                    /* ⛔⛔ NOT `if (data[pos+7] != auto_speaker)`. THE CLAIM
                     * BIT IS THE POINT, NOT THE VALUE.
                     *
                     * ⚠️ Measured on build 287: the host sends
                     * `cur=00 ctl=00 flags=00` -- volume zero and NOTHING
                     * claimed. ⓘ **A zero with no claim bit does not mean
                     * "volume zero", it means "not setting volume"**, so the
                     * controller keeps whatever it had, which is full.
                     *
                     * ⛔ A first attempt skipped the write when the value
                     * already matched. With a configured 0 that is `0 != 0`,
                     * false -- so it wrote nothing AND shadowed the fallback
                     * below, which does set the claim bit. **It made a working
                     * default path stop working.**
                     *
                     * ⭐ This file's own note says it: *"A CLAIMED field is
                     * applied even when it is zero."* ➡️ So claim and write
                     * every time.
                     *
                     * ⓘ `patched` only when something actually changed, so an
                     * unchanged report is not needlessly re-signed. */
                    const uint8_t want_flags = (uint8_t)(data[pos + 2] | 0x20u);
                    if (data[pos + 2] != want_flags || data[pos + 7] != auto_speaker) {
                        data[pos + 2] = want_flags;
                        data[pos + 7] = auto_speaker;
                        patched = 1;
                    }
                } else if (data[pos + 7] == 0 && data[pos + 9] == 0) {
                    data[pos + 2] = (uint8_t)(data[pos + 2] | 0xa0u);  /* allow speaker vol + audio ctrl */
                    data[pos + 7] = auto_speaker;
                    data[pos + 9] = DS5_BT_AUDIO_SPEAKER_ON;
                    patched = 1;
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
    /* ⛔ Floor removed -- see the AUTO path above and the slider note. */

    switch (settings->audio_mode) {
        case TV_BRIDGE_AUDIO_HEADSET:
            target_headset_volume = headset_volume;
            break;
        case TV_BRIDGE_AUDIO_SPEAKER:
            target_speaker_volume = speaker_volume;
            target_audio_flags = DS5_BT_AUDIO_SPEAKER_ON;
            break;
        case TV_BRIDGE_AUDIO_BOTH:
            target_headset_volume = headset_volume;
            target_speaker_volume = speaker_volume;
            target_audio_flags = DS5_BT_AUDIO_SPEAKER_ON;
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

        /* ⭐ Withhold the host's lightbar claim for the moment after a bridge,
         * while the app draws its confirmation. See light_hold_until_ms. */
        if (block_id == 0x90 && payload_len >= 2 && ctm_controller_light_held(c)) {
            if (data[pos + 3] & 0x04) {              /* lightbar control */
                data[pos + 3] &= (uint8_t)~0x04;
                patched = 1;
                /* ⭐ COUNTED, because "the light still flickers" cannot say
                 * whether this ran. ⛔ Silence in the log means the hold never
                 * fired -- the deadline was not set, or the window had already
                 * closed. A count means it fired and the colour arrived some
                 * other way. ⓘ Logged once per session, not per report. */
                static int s_held_logged;
                if (!s_held_logged) {
                    s_held_logged = 1;
                    ctl_log(c, "lightbar: withholding the host's claim during the handover");
                }
            }
        }

        if (BT_FEAT_AUDIO && block_id == 0x90 && payload_len >= 8) {
            /* WHAT ARRIVES HERE, measured on C3 over Bluetooth 2026-08-11.
             *
             * Recorded because two builds of logging were spent finding it,
             * and the logging is gone:
             *   - This block DOES arrive. An earlier reading of the evidence
             *     said it did not; that was wrong.
             *   - THE HOST ASKS FOR NOTHING. Every report carried speaker
             *     volume 0, headset volume 0 and audio control 0. Whatever
             *     the controller ends up playing at, this side chose it.
             *
             * ⚠️ IF THIS TURNS OUT NOT TO BE WHERE AUDIO SETTINGS FLOW,
             * DELETE THIS COMMENT. A note in the wrong place is worse than
             * none: it will be read as fact by whoever comes next. */
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
        } else if (ctm_controller_tone_pending(c) &&
                   (block_id == 0x93 || block_id == 0x94 ||
                    block_id == 0x95 || block_id == 0x96) &&
                   (int)payload_len == FEEDBACK_OPUS_FRAME_BYTES_EXPECTED) {
            /* See the note in the AUTO loop above. Duplicated rather than
             * hoisted: the two loops are upstream's, and two small deletable
             * blocks reconcile better with an upstream change than a helper
             * wedged between them. */
            if (ctm_controller_tone_take(c, &data[pos + 2], (int)payload_len) > 0) {
                patched = 1;
            }
        } else if (BT_FEAT_AUDIO && (block_id == 0x93 || block_id == 0x94 || block_id == 0x95 || block_id == 0x96) && audio_block != 0) {
            if (data[pos] != audio_block) {
                data[pos] = audio_block;
                patched = 1;
            }
        } else if (BT_FEAT_LATENCY && block_id == 0x91 && payload_len >= 6) {
            for (size_t i = 3; i <= 7; ++i) {
                if (data[pos + i] != latency) {
                    data[pos + i] = latency;
                    patched = 1;
                }
            }
        } else if (BT_FEAT_HAPTICS && block_id == 0x92 && payload_len >= 2 && settings->haptics_gain_centi != 100) {
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

/* The unplug gesture lives in its own file. It is a feature of ours rather
 * than a gap in this driver -- a DualSense works without it -- so keeping it
 * out of here means an upstream change to this file cannot collide with it,
 * and removing it is deleting one include and one line in the ops table
 * below.
 *
 * Included rather than compiled separately because it is the on_input_report
 * hook for this controller type and reaches its private state. */
/* on_plug_init: once, immediately after the host accepts the device, before
 * the session loop starts.
 *
 * ⛔ THE WIRED SPEAKER IS NOT OPENED HERE ANY MORE (2026-09-07). It was, from
 * 2026-08-11. This hook runs before the card matcher, so the opener had no
 * card to aim at and fell back to the scan: the LOWEST FREE DualSense card.
 * With the other controller released, that was the other controller's card.
 * It was opened, held for about 65 ms, then closed again when the matcher
 * moved the speaker to the right card -- and the other controller chirped.
 * Measured on C1 2026-09-07, predicted and confirmed twice in one run. When
 * the scan happened to pick the right card, the matcher's reopen instead
 * failed on our own handle and logged "playback busy, keeping fd".
 *
 * cardmatch_identify opens the speaker itself, on the matched card, with the
 * settings report after it -- and it too runs before the session loop, so
 * nothing the host sends can arrive ahead of it. Over Bluetooth there was
 * never anything to open here: the controller's audio device is not ours, and
 * the haptics travel inside the reports. */
static int ds5_on_plug_init(ctm_controller_t *c, ctm_transport_t *t)
{
    (void)c; (void)t;

    /* ⓘ Over Bluetooth the speaker still has to be told its volume and
     * routing before anything plays. That is done in controller_common.c,
     * beside the code that owns the Bluetooth signal -- this file cannot see
     * it. */

    return 0;
}

#include "ctm_gesture_chord.inl"

const ctm_controller_ops_t ctm_controller_ds5_ops = {
    .kind = "ds5",
    .needs_host_config = true,
    .grab_evdev = true,
    .request_bt_mode = true,
    .matches = ds5_matches,
    .select_node = NULL,
    .on_plug_init = ds5_on_plug_init,
    .on_input_report = ds5_on_input_report,
    .blank_input = ds5_blank_input,
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
    .on_plug_init = ds5_on_plug_init,
    .on_input_report = ds5_on_input_report,
    .blank_input = ds5_blank_input,
    .patch_output = ds5_patch_output,
    .set_settings = NULL,
};
