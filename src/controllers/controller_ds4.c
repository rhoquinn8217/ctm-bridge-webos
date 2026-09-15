/* DualShock 4 (DS4) controller, BT. Classification + Layout B output
 * patching. The service-side map emits Layout B frames: 0x11 effect reports
 * (rumble/LED + volume bytes) and pure-audio reports 0x12/0x14/0x17 whose
 * route byte sits at offset 5 (probed bitmask: 0xFF = stereo headphones,
 * 0xDF = split ch0->speaker ch1->headphone). This hook runs AFTER the map's
 * translation, so forcing here wins without fighting the map.
 *
 * ⭐ And a CABLED DS4, which has a type of its own rather than falling through
 * to generic: the unbridge chord, blanking while the TV's overlay is open, and
 * the TV's own confirmation signals -- controller_ds4_usb_ops, at the end.
 * ⓘ Every byte those decide lives in ds4_report.inl, where it is tested. */

#define _GNU_SOURCE

#include "ctm_controller.h"
#include "ds4_report.inl"

#include <pthread.h>
#include <string.h>
#include <time.h>

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

/* --- the unbridge chord, both transports ----------------------------------- */

/* Which scratch slot each feature uses. Per controller, never shared -- and
 * each is only ever touched by one thread, so neither needs a lock. */
#define DS4_SLOT_CHORD     0   /* the input thread's: when the chord began */
#define DS4_SLOT_WITHHELD  1   /* the session thread's: host reports withheld in a row */

static uint64_t ds4_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* on_input_report: the unbridge chord on a DS4 -- two fingers on the touchpad
 * and press it down, held -- to unplug the pad without the overlay. When: this
 * controller's input thread, once per relayed report.
 *
 * ⭐ THE DUALSENSE'S GESTURE, KEPT IN STEP WITH IT: the same user switch and
 * the same hold, both read from ctm_gesture_chord.inl rather than copied; the
 * same lines in ctm-gesture.log, under this type's kind; and the timestamp in
 * the controller's own type state, never a file-level variable -- two
 * controllers each have their own input thread calling this.
 *
 * ⓘ Not swallowed: the report still reaches the host, as a DualSense's does.
 *
 * ⛔ A REPORT THAT IS NOT A WHOLE PAD REPORT SAYS NOTHING ABOUT THE CHORD, so it
 * leaves the hold timer exactly as it was. ds4_chord_held() answers false for
 * one, and false there means RELEASED -- which is how a DualSense's audio
 * reports once made its hold impossible to complete. ⓘ Over a cable every
 * report is the whole 0x01, so this only ever matters over Bluetooth. */
static void ds4_on_input_report(ctm_controller_t *c, const uint8_t *data, size_t len)
{
    if (!c) return;
    if (!gesture_chord_enabled()) return;
    if (!ds4_pad_report(data, len, NULL, NULL)) return;

    const int hold_ms = gesture_chord_hold_ms();

    /* Logged on TRANSITIONS ONLY. A cabled DS4 reports 250 times a second, and
     * a line per call would drown the file. */
    if (!ds4_chord_held(data, len)) {
        uint64_t was = ctm_controller_type_state(c, DS4_SLOT_CHORD);
        if (was != 0 && was != UINT64_MAX) {
            ctm_gesture_log(c, "chord released after %llums, short of %dms",
                            (unsigned long long)(ds4_now_ms() - was), hold_ms);
        }
        /* Released: re-arm. */
        ctm_controller_set_type_state(c, DS4_SLOT_CHORD, 0);
        return;
    }

    uint64_t since = ctm_controller_type_state(c, DS4_SLOT_CHORD);
    uint64_t now = ds4_now_ms();

    if (since == 0) {
        ctm_gesture_log(c, "chord held, timing a %dms hold", hold_ms);
        ctm_controller_set_type_state(c, DS4_SLOT_CHORD, now);
        return;
    }
    /* Already fired for this hold: wait for the fingers to lift. */
    if (since == UINT64_MAX) {
        return;
    }
    if (now - since >= (uint64_t)hold_ms) {
        ctm_gesture_log(c, "chord complete after %llums, asking to unplug",
                        (unsigned long long)(now - since));
        ctm_controller_set_type_state(c, DS4_SLOT_CHORD, UINT64_MAX);
        ctm_controller_request_unplug(c);
    }
}

const ctm_controller_ops_t ctm_controller_ds4_ops = {
    .kind = "ds4",
    .needs_host_config = true,
    .grab_evdev = true,
    .request_bt_mode = true,
    .matches = ds4_matches,
    .select_node = NULL,
    .on_plug_init = NULL,
    /* ⚠️ UNTESTED ON HARDWARE: the chord and the overlay blanking, at the
     * Bluetooth report's offsets. Kept to exactly that -- no signal, nothing
     * else -- because no TV available when it was written could bridge a
     * Bluetooth DS4 to try it. */
    .on_input_report = ds4_on_input_report,
    .blank_input = ds4_blank_input,
    .patch_output = ds4_patch_output,
    .set_settings = NULL,   /* live values read via get_settings in patch_output */
};

/* --- a cabled DS4 ----------------------------------------------------------- */

/* matches: claim the DualShock 4 (either PID) on a cable. When: classification.
 * ⓘ The Bluetooth type above takes the same pad over the air. */
static bool ds4_usb_matches(const ctm_controller_dev_t *dev)
{
    return dev &&
           strcmp(dev->vid, "054c") == 0 &&
           (strcmp(dev->pid, "09cc") == 0 || strcmp(dev->pid, "05c4") == 0) &&
           strcmp(dev->bus, "USB") == 0;
}

/* ⭐⭐ ITS CONFIRMATION SIGNALS, WRITTEN BY THE TV.
 *
 * ⛔ Until now a cabled DS4 bridged and came back in silence and darkness. It
 * does not speak the DualSense protocol, so none of the DualSense's signals
 * reach it, and the app's own SDL pulse stands down once a session exists.
 * ➡️ So the TV writes the pad's own output report: one green breath with a
 * short pulse when the host has it, two yellow breaths with a pulse when it
 * comes back -- the colours the DualSense's signals already use.
 *
 * ⓘ No refusal here: a refused pad has no session, and the app still holds it
 * through SDL and flashes and buzzes it itself.
 *
 * ⚠️ THE USER'S SWITCHES DECIDE WHAT IS CLAIMED, not only what is seen. With the
 * light off, no report claims the lightbar and the host's colour is never
 * withheld; with the rumble off, the same for the motors; with both off,
 * nothing is written at all. */
#define DS4_SIGNAL_STEP_MS  20     /* a report per step while the light moves */
#define DS4_PULSE_MS        250
/* ⓘ Both motors at half: the app's own success pulse is 0x7FFF of 0xFFFF, and
 * SDL hands a DS4 the top byte of that. */
#define DS4_PULSE_LEVEL     0x7f
#define DS4_CONNECTED_MS    700
#define DS4_RELEASED_MS     800
/* Kept beside the host's colour, above its 24 bits: "the host has set one",
 * so black is a colour and never having set one is not. */
#define DS4_KEPT_COLOUR     0x01000000u

typedef struct {
    uint8_t r, g, b;
    int breaths;
    long ms;
} ds4_signal_shape_t;

typedef struct {
    int sent;
    int failed;
    int cut;          /* stopped early: plug-out began */
    long took_ms;
} ds4_signal_run_t;

static const ds4_signal_shape_t k_ds4_connected = { 0x00, 0xff, 0x00, 1, DS4_CONNECTED_MS };
static const ds4_signal_shape_t k_ds4_released  = { 0xff, 0xff, 0x00, 2, DS4_RELEASED_MS };

/* What a signal may claim right now, as output valid flags. */
static uint8_t ds4_signal_drives(void)
{
    return (uint8_t)((signals_light_on() ? DS4_OUT_LIGHT : 0) |
                     (signals_rumble_on() ? DS4_OUT_MOTORS : 0));
}

static long ds4_elapsed_ms(const struct timespec *t0)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - t0->tv_sec) * 1000L + (now.tv_nsec - t0->tv_nsec) / 1000000L;
}

/* Sleep until `due_ms` after `t0`. ⭐ An absolute deadline rather than a fixed
 * sleep after each write, so a slow write is absorbed by the next wait instead
 * of pushing the rest of the signal back -- see btsig_wait_for. */
static void ds4_sleep_until(const struct timespec *t0, long due_ms)
{
    const long wait_ms = due_ms - ds4_elapsed_ms(t0);
    if (wait_ms <= 0) return;
    struct timespec ts = { (time_t)(wait_ms / 1000), (wait_ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* One report of the TV's own, claiming only what `claims` names. ⓘ Through
 * ctm_controller_write_raw, so it takes the node's lock like the host's writes
 * and skips the patcher, which exists for the host's reports. */
static int ds4_signal_write(ctm_controller_t *c, uint8_t claims, uint8_t motor,
                            uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t rep[DS4_OUT_LEN];
    if (ds4_build_output(rep, sizeof(rep), claims, motor, motor, r, g, b) == 0) return -1;
    return ctm_controller_write_raw(c, rep, sizeof(rep));
}

/* Play one signal's breaths and pulse, on the calling thread.
 *
 * ⓘ A report per step while the light moves and none while nothing changes, so
 * a rumble-only signal is two reports: the pulse and its stop.
 * ⚠️ A failed write ends the pattern. A pad that refuses one report refuses the
 * rest, and there is nothing to be gained sleeping through them.
 * ⛔ THE PULSE ALWAYS ENDS WITH A STOP, however the pattern ended -- a DS4's
 * motors keep running until a report tells them otherwise. */
static void ds4_signal_play(ctm_controller_t *c, const ds4_signal_shape_t *s, uint8_t drives,
                            bool cancellable, ds4_signal_run_t *run)
{
    memset(run, 0, sizeof(*run));
    const bool light = (drives & DS4_OUT_LIGHT) != 0;
    const bool rumble = (drives & DS4_OUT_MOTORS) != 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int last_level = -1, last_motor = -1;
    for (long step = 1;; ++step) {
        const long at = ds4_elapsed_ms(&t0);
        if (at >= s->ms || (!light && at >= DS4_PULSE_MS)) break;
        if (cancellable && controller_signal_stopping(c)) {
            run->cut = 1;
            break;
        }
        const int motor = (rumble && at < DS4_PULSE_MS) ? DS4_PULSE_LEVEL : 0;
        const int level = light ? ds4_breath_level(at, s->ms, s->breaths) : 0;
        if (level != last_level || motor != last_motor) {
            if (ds4_signal_write(c, drives, (uint8_t)motor,
                                 (uint8_t)((s->r * level) / 255),
                                 (uint8_t)((s->g * level) / 255),
                                 (uint8_t)((s->b * level) / 255)) != 0) {
                ++run->failed;
                break;
            }
            ++run->sent;
            last_level = level;
            last_motor = motor;
        }
        ds4_sleep_until(&t0, step * DS4_SIGNAL_STEP_MS);
    }
    if (rumble) {
        if (ds4_signal_write(c, DS4_OUT_MOTORS, 0, 0, 0, 0) == 0) ++run->sent;
        else ++run->failed;
    }
    run->took_ms = ds4_elapsed_ms(&t0);
}

/* The connected signal's thread.
 *
 * ⛔⛔ `c` IS SAFE ONLY UNTIL controller_signal_end RETURNS TRUE. Plug-out waits
 * for exactly that, then closes the node, and its caller frees the controller.
 * So ending is the last thing here: every log line comes before it.
 *
 * ⭐⭐ THEN THE LIGHT GOES BACK AS THE HOST LEFT IT. Its colour claims were
 * withheld while the breath played, and a host that sets its colour once and
 * never again would otherwise leave the pad dark for the whole session. Dark is
 * right only when the host has set nothing.
 *
 * ⚠️ AND IT ASKS AGAIN UNTIL NOTHING NEWER HAS ARRIVED. A colour the host sets
 * while this one is being written is withheld too, so the signal only lets go
 * once what it wrote is still the latest -- checked in the same step as letting
 * go. ⓘ Bounded: a host changing colour faster than a report can be written
 * sends its next one straight away, and that one passes untouched. */
static void *ds4_connected_thread(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    const uint8_t drives = ds4_signal_drives();
    ds4_signal_run_t run;
    ds4_signal_play(c, &k_ds4_connected, drives, true, &run);
    ctl_log(c, "signal: connected -- green breath %s, pulse %s: %d report(s), %d failed, %ldms%s",
            (drives & DS4_OUT_LIGHT) ? "on" : "off", (drives & DS4_OUT_MOTORS) ? "on" : "off",
            run.sent, run.failed, run.took_ms, run.cut ? ", cut short by a release" : "");

    uint32_t kept = controller_signal_kept(c);
    for (int round = 1;; ++round) {
        if ((drives & DS4_OUT_LIGHT) && !controller_signal_stopping(c)) {
            const bool seen = (kept & DS4_KEPT_COLOUR) != 0;
            const int rc = ds4_signal_write(c, DS4_OUT_LIGHT, 0,
                                            seen ? (uint8_t)(kept >> 16) : 0,
                                            seen ? (uint8_t)(kept >> 8) : 0,
                                            seen ? (uint8_t)kept : 0);
            if (seen) {
                ctl_log(c, "signal: connected -- light handed back as the host's #%06x%s",
                        (unsigned)(kept & 0xffffffu), rc == 0 ? "" : " (write failed)");
            } else {
                ctl_log(c, "signal: connected -- light left dark, the host has set no colour%s",
                        rc == 0 ? "" : " (write failed)");
            }
        }
        if (controller_signal_end(c, round < 4 ? &kept : NULL)) break;
        kept = controller_signal_kept(c);
    }
    return NULL;
}

/* signal_connected: start the connected signal. When: the session thread, as
 * the session starts.
 *
 * ⭐ ON A THREAD OF ITS OWN, DETACHED, as the DualSense's is: the session thread
 * carries the reports and must not sleep through a breath, and nobody waits for
 * a confirmation. ⛔ Unlike the DualSense's, it claims the controller first, so
 * plug-out cannot close the node under it -- see controller_signal_begin. */
static void ds4_usb_signal_connected(ctm_controller_t *c)
{
    if (!ds4_signal_drives()) {
        ctl_log(c, "signal: connected -- light and rumble are switched off, nothing played");
        return;
    }
    if (!controller_signal_begin(c)) {
        ctl_log(c, "signal: connected -- not played, a signal is still playing or the pad is being released");
        return;
    }
    pthread_t sig;
    const int rc = pthread_create(&sig, NULL, ds4_connected_thread, c);
    if (rc == 0) {
        pthread_detach(sig);
    } else {
        ctl_log(c, "signal: connected -- could not start the signal thread rc=%d", rc);
        controller_signal_end(c, NULL);   /* nothing was written, nothing to give back */
    }
}

/* signal_unplugging: two yellow breaths and a pulse as the pad comes back. When:
 * plug-out, before the session stops and the node closes, with any connected
 * signal already stopped and waited for.
 *
 * ⓘ Synchronous, and not cut short by the release it announces: it IS the
 * release. The host's claims stay withheld from here to the end of the session.
 * ⓘ Ends dark, as the DualSense's wired handback does: nothing is restored, and
 * whoever owns the light next writes over it. */
static void ds4_usb_signal_unplugging(ctm_controller_t *c, ctm_unplug_reason_t why)
{
    const char *what;
    switch (why) {
    case CTM_UNPLUG_SHUTDOWN: what = "unplugging (shutdown)"; break;
    case CTM_UNPLUG_REPLACED: what = "unplugging (replaced)"; break;
    default:                  what = "unplugging (requested)"; break;
    }
    const uint8_t drives = ds4_signal_drives();
    if (!drives) {
        ctl_log(c, "signal: %s -- light and rumble are switched off, nothing played", what);
        return;
    }
    ds4_signal_run_t run;
    ds4_signal_play(c, &k_ds4_released, drives, false, &run);
    if ((drives & DS4_OUT_LIGHT) && !run.failed) {
        if (ds4_signal_write(c, DS4_OUT_LIGHT, 0, 0, 0, 0) == 0) ++run.sent;
        else ++run.failed;
    }
    ctl_log(c, "signal: %s -- yellow breaths %s, pulse %s: %d report(s), %d failed, %ldms",
            what, (drives & DS4_OUT_LIGHT) ? "on" : "off", (drives & DS4_OUT_MOTORS) ? "on" : "off",
            run.sent, run.failed, run.took_ms);
}

/* patch_output: while the TV's own signal holds the pad -- one is playing, or
 * the pad is being released -- the host's claims on what that signal drives
 * are taken out of its reports, so the host cannot paint over the breath or cut
 * the pulse short. Otherwise the host's report passes through untouched. The
 * colour the host sets is remembered either way, for the connected signal to
 * hand back.
 *
 * ⚠️ NOT ctm_controller_light_held()'s WINDOW. That is a fixed 1.4 s, sized for
 * a 1.1 s breath drawn by someone else on a Bluetooth DualSense. This signal is
 * the core's own and ends when it says so; withholding past that would drop a
 * colour the host set with nothing left to hand it back -- and with the signals
 * switched off there is no signal at all to do it.
 *
 * ⓘ Counted, and logged when the withholding starts and ends, because "the
 * breath flickered" cannot say whether this ran.
 *
 * When: every outbound report, on the session thread. Returns 0 (never drops). */
static int ds4_usb_patch_output(ctm_controller_t *c, uint8_t *data, size_t *len_io)
{
    const size_t len = len_io ? *len_io : 0;
    if (!ds4_host_output(data, len)) return 0;

    uint32_t rgb = 0;
    const bool sets_colour = ds4_output_colour(data, len, &rgb);
    const uint64_t withheld = ctm_controller_type_state(c, DS4_SLOT_WITHHELD);
    if (!controller_signal_host_report(c, sets_colour, DS4_KEPT_COLOUR | rgb)) {
        if (withheld) {
            ctl_log(c, "signal: the host has the light and motors back, %llu report(s) were withheld",
                    (unsigned long long)withheld);
            ctm_controller_set_type_state(c, DS4_SLOT_WITHHELD, 0);
        }
        return 0;
    }
    const uint8_t taken = ds4_withhold_output(data, len, ds4_signal_drives());
    if (taken) {
        if (!withheld) {
            ctl_log(c, "signal: withholding the host's %s while the TV's signal holds the pad",
                    taken == (DS4_OUT_LIGHT | DS4_OUT_MOTORS) ? "light and motors"
                    : (taken & DS4_OUT_LIGHT) ? "light" : "motors");
        }
        ctm_controller_set_type_state(c, DS4_SLOT_WITHHELD, withheld + 1);
    }
    return 0;
}

/* ⭐⭐ A CABLED DUALSHOCK 4. Until now it matched nothing specific and fell
 * through to generic: relayed verbatim, with no chord, no blanking under the
 * overlay and no signal.
 *
 * ⓘ Everything not named here is what generic gave it, deliberately: the grab
 * rule that takes keyboards and mice and leaves a gamepad to SDL and the overlay
 * combo, no wait for a host config, no Bluetooth mode request.
 *
 * ⛔ NOT speaks_ds5. A DS4 speaks its own protocol, and that flag brings the
 * DualSense's sound card, its tones, the feature 0x09 probe and the
 * microphone-safety check, which exits the app on any report shaped like a
 * DualSense's audio report.
 *
 * ⓘ kind "ds4_usb" -- the word the TV already sends the listener for this pad,
 * so both sides' logs name the path the same way, and "ds4" stays the
 * Bluetooth type's: its chord lines in ctm-gesture.log are the untested ones,
 * and a line has to say which of the two wrote it. */
const ctm_controller_ops_t controller_ds4_usb_ops = {
    .kind = "ds4_usb",
    .needs_host_config = false,
    .grab_evdev = true,
    .grab_skips_gamepads = true,
    .request_bt_mode = false,
    .speaks_ds5 = false,
    .matches = ds4_usb_matches,
    .on_input_report = ds4_on_input_report,
    .blank_input = ds4_blank_input,
    .patch_output = ds4_usb_patch_output,
    .signal_connected = ds4_usb_signal_connected,
    .signal_unplugging = ds4_usb_signal_unplugging,
};
