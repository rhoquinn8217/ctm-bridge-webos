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

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

#include <linux/hidraw.h>

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
/* ⓘ DIAGNOSTIC, added 2026-09-22. Counts host audio reports dropped while a
 * signal plays, so the tone's own log line can say whether the drop fired at
 * all. ⚠️ A count of ZERO during a manual bridge would mean host audio does
 * not reach ds4_patch_output, and the competition is somewhere else. */
#define DS4_SLOT_AUDIO_DROPPED 3
/* ⓘ DIAGNOSTIC: host reports of ANY id seen while a signal holds the pad.
 * `dropped=0` proved no AUDIO arrives; this says whether ANYTHING does -- a
 * 0x11 claiming the volumes mid-tone would be just as disruptive and would
 * never have shown up in that count. */
#define DS4_SLOT_HOST_SEEN     4

static int ds4_patch_output(ctm_controller_t *c, uint8_t *data, size_t *len_io)
{
    tv_bridge_worker_settings_t s;
    ctm_controller_get_settings(c, &s);
    const tv_bridge_worker_settings_t *settings = &s;

    size_t len = len_io ? *len_io : 0;
    if (!data || len < 10) return 0;

    if (controller_signal_host_report(c, false, 0)) {
        ctm_controller_set_type_state(c, DS4_SLOT_HOST_SEEN,
            ctm_controller_type_state(c, DS4_SLOT_HOST_SEEN) + 1);
    }

    int patched = 0;

    if (data[0] == 0x12 || data[0] == 0x14 || data[0] == 0x17) {
        /* ⛔⛔ WHILE OUR TONE PLAYS, THE HOST'S AUDIO IS DROPPED.
         *
         * ⚠️ THE FAULT THIS FIXES, heard 2026-09-22: rhoquinn8217 on build
         * 403 -- *"bridge tone is short sometimes I only hear a crack"*. The
         * REFUSAL tone was clean and the BRIDGE tone was not, and the one
         * difference is the node. A refusal plays to a pad that is NOT
         * bridged, so nothing else writes to it. A bridge and a handback play
         * to a pad that IS, and the host's own audio arrives as these very
         * reports -- so two streams of SBC frames interleave and the pad
         * decodes a mixture of both. Our 928 ms of tone becomes a fraction of
         * that, which is exactly what "short, sometimes a crack" sounds like.
         *
         * ⭐ So the signal gets the node to itself, the way the refusal always
         * had it. The cost is that a game's audio is muted for about a second,
         * which is the same trade the lightbar and the motors already make
         * just below -- a confirmation signal owns what it drives while it
         * plays.
         *
         * ⓘ Returning 1 means the patch CONSUMED the report: it is not
         * written on. 🔗 apply_output_settings. */
        if (controller_signal_host_report(c, false, 0)) {
            ctm_controller_set_type_state(c, DS4_SLOT_AUDIO_DROPPED,
                ctm_controller_type_state(c, DS4_SLOT_AUDIO_DROPPED) + 1);
            return 1;
        }
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
#define DS4_SLOT_AUDIO     2   /* the last audio values SENT, so a resend only happens on a change */

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

/* --- the Bluetooth DS4's audio settings, SENT rather than waited for --------
 *
 * ⛔⛔ THE FAULT THIS EXISTS FOR (T-229): speaker_volume, headset_volume and
 * audio_output reached the TV, the TV was willing, and the pad did nothing.
 *
 * ⭐ ds4_patch_output is a PATCH hook. It edits reports FLOWING FROM THE HOST
 * and originates none. The volume bytes are written only inside
 * `data[0] == 0x11`, the pad's effects report -- so unless the host happens to
 * send rumble or a lightbar change, there is nothing to patch and the setting
 * never leaves the TV. Measured 2026-09-20 on a bridged DS4: the section drew,
 * the values travelled, and none of the three did anything. ⓘ The route byte
 * has the same shape: patched onto host AUDIO frames, so with nothing playing
 * there is nothing to route.
 *
 * ➡️ So the pad is told directly when a value changes.
 *
 * ⛔⛔ AND IT IS ONE REPORT ON A CHANGE, NEVER A STREAM. The listener's own map
 * records a user test from 2026-07-25 where a continuous silence lane killed a
 * DS4's audio entirely -- "ALL audio died ... the controller misbehaved",
 * suspect "flooding 0x14s starves/roots the pad". ⚠️ Different report, same
 * lesson: this must not become a pump. The slot below is what keeps it honest.
 *
 * ⭐ THE REPORT ASKS FOR AS LITTLE AS IT CAN. Byte 3 is the valid-flag byte:
 * its LOW nibble is rumble, LED and flash, its high bits are the volumes
 * (0x10/0x20 headphone L/R, 0x80 speaker -- the same 0xb0 the patch path sets).
 * Sending 0xb0 with a zero low nibble says "change the volumes, leave the
 * rumble and the light alone", so a report that arrives while a game is driving
 * the lightbar cannot darken it.
 *
 * ⚠️ DERIVED, NOT MEASURED: bytes 1 and 2. The map states the AUDIO reports
 * use "0x40|poll_rate (HID bit OFF -- no effects in audio reports; effects stay
 * in 0x11)", so an effects report has that bit ON -- 0xC0 -- and 0xA0 follows
 * it, matching DS4Windows. The length, 78, is the standard DS4 Bluetooth output
 * report. ⛔ None of that has been confirmed against this pad; if it is wrong
 * the pad should ignore the report, and the log line below is how that is
 * told apart from it working. */
static void ds4_bt_send_audio(ctm_controller_t *c, const tv_bridge_worker_settings_t *s)
{
    if (!c || !s) return;

    const uint8_t headset = ds4_volume_raw_byte(s->headset_volume_percent);
    const uint8_t speaker = ds4_volume_raw_byte(s->speaker_volume_percent);
    const uint8_t route   = ds4_route_for_mode(s->audio_mode);

    /* Nothing new to say, so say nothing. Keeps a settings poll from becoming
     * the stream the July test warns about. */
    const uint64_t now = ((uint64_t)headset << 16) | ((uint64_t)speaker << 8) | route
                       | 0x1000000ull;   /* a marker, so "never sent" is not 0 */
    if (ctm_controller_type_state(c, DS4_SLOT_AUDIO) == now) return;

    uint8_t rep[78];
    memset(rep, 0, sizeof rep);
    rep[0]  = 0x11;
    rep[1]  = 0xc0;   /* HID bit on, poll rate 0 -- see the note above */
    rep[2]  = 0xa0;
    rep[3]  = 0xb0;   /* volumes valid; rumble/LED/flash NOT claimed */
    rep[21] = headset;
    rep[22] = headset;
    rep[24] = speaker;
    ctm_bt_sign_output(rep, sizeof rep);

    /* ctm_controller_write_raw, not a bare write: it is the documented way for a
     * type to say something to the controller itself rather than pass a host
     * report along, it checks the fd, and it takes hid_mutex -- which a bare
     * write would not, racing every other writer on this pad. */
    const int rc = ctm_controller_write_raw(c, rep, sizeof rep);
    ctm_controller_set_type_state(c, DS4_SLOT_AUDIO, now);
    ctl_log(c, "ds4 audio: told the pad headset=%u speaker=%u route=0x%02x, rc=%d",
            headset, speaker, route, rc);
}

/* set_settings: a live slider moved. ⓘ The FIRST set_settings in this tree --
 * every other type leaves it NULL and reads its values inside patch_output,
 * which is exactly why a pad the host is not talking to never heard them. */
static void ds4_bt_set_settings(ctm_controller_t *c, const tv_bridge_worker_settings_t *s)
{
    ds4_bt_send_audio(c, s);
}

/* --- the BLUETOOTH pad's own signals (T-238) -------------------------------
 *
 * ⛔⛔ UNTIL NOW A BLUETOOTH DS4 SIGNALLED NOTHING AT ALL. `signal_connected`
 * sat on the CABLED ops table alone, and ctm_controller.h says why in words:
 * *"for a Bluetooth Xbox pad or a Bluetooth DS4 the core has nothing to play,
 * so NOBODY signals and the bridge is silent."* ⭐ Giving the Bluetooth table
 * these two hooks also makes ctm_controller_will_signal_connect() answer yes
 * for this pad, so the TV correctly stands aside -- that rule is COMPUTED from
 * the ops table rather than copied, which is exactly why this works without
 * touching the TV.
 *
 * ⓘ TONE ONLY, FOR NOW. The light and the pulse need a Bluetooth output
 * report (0x11, 78 bytes, CRC-signed) and ds4_build_output makes the CABLED
 * 0x05 -- so they are a separate piece of work, and they will use the gate
 * ds4_signal_drives() already applies. ⚠️ Until then a Bluetooth bridge is
 * heard and not seen.
 *
 * ✅ THE TONE IS GATED, inside ds4sig_play_fd, by the same switch the
 * DualSense's uses. */
/* ⛔⛔ BACK ON ITS OWN THREAD, AND THE REASON IS A REGRESSION I CAUSED.
 *
 * Running it on the session thread DID free the Bluetooth link -- measured,
 * build 413: writes fell from 10257 ms to 81 ms, and the pacing went from
 * `24169ms for 928ms of audio` to `929ms for 928ms`. ✅ That part worked.
 * ⛔ But the configure write BLOCKS for about 4.2 s at bridge time (build 415:
 * `configure 4152ms`), and on the session thread that delays the pad's input
 * by five seconds at every bridge. ⚠️ A five-second wait before a controller
 * responds is far worse than a confirmation tone that does not play.
 *
 * ⓘ SO THE TONE IS STILL IMPERFECT ON A BRIDGE, KNOWINGLY. 🔗 T-238 carries
 * what was measured and what is left. The handback and the refusal are clean;
 * the bridge is the one that is not. */
#define DS4_BT_CONNECT_SETTLE_MS  500

static void *ds4_bt_connected_thread(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    struct timespec settle = { DS4_BT_CONNECT_SETTLE_MS / 1000,
                               (long)(DS4_BT_CONNECT_SETTLE_MS % 1000) * 1000000L };
    nanosleep(&settle, NULL);
    if (controller_signal_stopping(c)) {
        ctl_log(c, "signal: connected -- not played, the pad was released while settling");
        controller_signal_end(c, NULL);
        return NULL;
    }
    const int rc = ds4_signal_tone_bt(c, 0 /* BTSIG_HANDING_OVER */);
    ctl_log(c, "signal: connected -- tone rc=%d, host audio dropped=%llu, host reports seen=%llu",
            rc,
            (unsigned long long)ctm_controller_type_state(c, DS4_SLOT_AUDIO_DROPPED),
            (unsigned long long)ctm_controller_type_state(c, DS4_SLOT_HOST_SEEN));
    controller_signal_end(c, NULL);
    return NULL;
}

/* ⚠️ ON ITS OWN THREAD: the tone takes about a second and the session thread
 * carries the pad's reports. 🔗 The note beside feedback_play, which learned
 * that the expensive way -- and which build 413 confirmed from the other
 * direction. */
static void ds4_bt_signal_connected(ctm_controller_t *c)
{
    if (!controller_signal_begin(c)) {
        ctl_log(c, "signal: connected -- not played, a signal is still playing "
                   "or the pad is being released");
        return;
    }
    pthread_t sig;
    const int rc = pthread_create(&sig, NULL, ds4_bt_connected_thread, c);
    if (rc == 0) {
        pthread_detach(sig);
    } else {
        ctl_log(c, "signal: connected -- could not start the signal thread rc=%d", rc);
        controller_signal_end(c, NULL);
    }
}

/* signal_unplugging: high then low, falling, coming home.
 * ⓘ Synchronous, like the cabled one, and for the same reason: it is not cut
 * short by the release it announces, because it IS the release. */
static void ds4_bt_signal_unplugging(ctm_controller_t *c, ctm_unplug_reason_t why)
{
    const char *what;
    switch (why) {
    case CTM_UNPLUG_SHUTDOWN: what = "unplugging (shutdown)"; break;
    case CTM_UNPLUG_REPLACED: what = "unplugging (replaced)"; break;
    default:                  what = "unplugging (requested)"; break;
    }
    const int rc = ds4_signal_tone_bt(c, 1 /* BTSIG_HANDED_BACK */);
    ctl_log(c, "signal: %s -- tone rc=%d, host audio dropped=%llu, host reports seen=%llu",
            what, rc,
            (unsigned long long)ctm_controller_type_state(c, DS4_SLOT_AUDIO_DROPPED),
            (unsigned long long)ctm_controller_type_state(c, DS4_SLOT_HOST_SEEN));
}

/* ⓘ Defined further down, beside the cabled pad's signal machinery they
 * share. Declared here because the Bluetooth table comes first in this file
 * and now uses them too. */
static void ds4_signal_connected(ctm_controller_t *c);
static void ds4_signal_unplugging(ctm_controller_t *c, ctm_unplug_reason_t why);

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
    /* ⭐ T-229: the volumes and the route are SENT on a change, because
     * patch_output alone only reaches a pad the host is already talking to. */
    .set_settings = ds4_bt_set_settings,
    /* ⭐ T-238: a Bluetooth DS4 signals at all now. Tone only so far; the
     * light and the pulse want a 0x11 builder that does not exist yet. */
    /* ⭐⭐ LIGHT AND PULSE, BUT NOT THE TONE.
     * The tone is played by ui_bridge.c outside the session, because a write
     * to a bridged pad blocks for seconds. The light and the pulse are a
     * handful of small reports, not a stream, so they are fine from in here --
     * and they must be, because this is also what tells the TV to stand aside
     * (ctm_controller_will_signal_connect reads this table). ⛔ Without it the
     * TV fires its OWN pulse into our tone, which is the 2026-09-18 fault:
     * "the TV pulsed a pad the core was about to sing to". */
    .signal_connected = ds4_signal_connected,
    .signal_unplugging = ds4_signal_unplugging,
    /* ⛔ NO .signal_unplugging either. The handback is played in ui_bridge.c
     * AFTER the session has gone, for the same reason the handover is played
     * before it starts: a tone from inside a live session is starved. Wiring
     * it here too would play a second, broken one first. 🔗 T-238. */
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
/* Which report shape this pad wants. ⓘ ctm_controller_bus answers "USB" or
 * "BT"; the two output reports differ by more than a header. */
static bool ds4_is_bt(const ctm_controller_t *c)
{
    const char *bus = ctm_controller_bus(c);
    return bus != NULL && (bus[0] == 'B' || bus[0] == 'b');
}

static int ds4_signal_write(ctm_controller_t *c, bool bt, uint8_t claims, uint8_t motor,
                            uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t rep[DS4_BT_OUT_LEN];          /* the larger of the two */
    const size_t n = bt
        ? ds4_bt_build_output(rep, sizeof(rep), claims, motor, motor, r, g, b)
        : ds4_build_output(rep, sizeof(rep), claims, motor, motor, r, g, b);
    if (n == 0) return -1;
    /* ⛔ The signature, and the pad drops the report without it. */
    if (bt) ctm_bt_sign_output(rep, n);
    return ctm_controller_write_raw(c, rep, n);
}

/* Play one signal's breaths and pulse, on the calling thread.
 *
 * ⓘ A report per step while the light moves and none while nothing changes, so
 * a rumble-only signal is two reports: the pulse and its stop.
 * ⚠️ A failed write ends the pattern. A pad that refuses one report refuses the
 * rest, and there is nothing to be gained sleeping through them.
 * ⛔ THE PULSE ALWAYS ENDS WITH A STOP, however the pattern ended -- a DS4's
 * motors keep running until a report tells them otherwise.
 *
 * ⭐⭐ AND THE MOTORS ARE LET GO WITH THAT STOP, not when the breath ends (found in
 * review, 2026-09-15). The pulse is 250 ms of a 700 ms breath, and every breath
 * step used to claim the motors at zero -- with a last stop at the end, and the
 * host's motor claims withheld throughout -- so a game that started a rumble in
 * those 450 ms lost it until it next sent one. ➡️ Once the stop is out, later
 * steps claim the light only, and controller_signal_motors_done() tells the
 * patcher to pass the host's motors again. */
static void ds4_signal_play(ctm_controller_t *c, bool bt, const ds4_signal_shape_t *s,
                            uint8_t drives, bool cancellable, ds4_signal_run_t *run)
{
    memset(run, 0, sizeof(*run));
    const bool light = (drives & DS4_OUT_LIGHT) != 0;
    const bool rumble = (drives & DS4_OUT_MOTORS) != 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int last_level = -1, last_motor = -1;
    bool motors_released = !rumble;
    for (long step = 1;; ++step) {
        const long at = ds4_elapsed_ms(&t0);
        if (at >= s->ms || (!light && at >= DS4_PULSE_MS)) break;
        if (cancellable && controller_signal_stopping(c)) {
            run->cut = 1;
            break;
        }
        const int motor = (rumble && at < DS4_PULSE_MS) ? DS4_PULSE_LEVEL : 0;
        const int level = light ? ds4_breath_level(at, s->ms, s->breaths) : 0;
        if (level != last_level || (!motors_released && motor != last_motor)) {
            const uint8_t claims = (uint8_t)((light ? DS4_OUT_LIGHT : 0) |
                                             (motors_released ? 0 : DS4_OUT_MOTORS));
            if (ds4_signal_write(c, bt, claims, (uint8_t)motor,
                                 (uint8_t)((s->r * level) / 255),
                                 (uint8_t)((s->g * level) / 255),
                                 (uint8_t)((s->b * level) / 255)) != 0) {
                ++run->failed;
                break;
            }
            ++run->sent;
            last_level = level;
            last_motor = motor;
            if (!motors_released && motor == 0) {
                /* ⓘ That report was the pulse's stop. */
                motors_released = true;
                controller_signal_motors_done(c);
            }
        }
        ds4_sleep_until(&t0, step * DS4_SIGNAL_STEP_MS);
    }
    if (!motors_released) {
        /* ⓘ Ended inside the pulse -- cut short, a failed write, or a rumble-only
         * signal, which stops stepping when its pulse is over. */
        if (ds4_signal_write(c, bt, DS4_OUT_MOTORS, 0, 0, 0, 0) == 0) ++run->sent;
        else ++run->failed;
        controller_signal_motors_done(c);
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
 * never again would otherwise leave the pad dark for the whole session.
 * ⭐ AND GREEN, NOT DARK, WHEN THE HOST HAS SET NOTHING (rhoquinn8217,
 * 2026-09-15): "have the last green flash persist rather than fade off. That
 * way it has color incase nothing else changes after it bridges and isn't left
 * off." A dark pad read as not connected. The host's first colour replaces it.
 *
 * ⚠️ AND IT ASKS AGAIN UNTIL NOTHING NEWER HAS ARRIVED. A colour the host sets
 * while this one is being written is withheld too, so the signal only lets go
 * once what it wrote is still the latest -- checked in the same step as letting
 * go. ⓘ Bounded: a host changing colour faster than a report can be written
 * sends its next one straight away, and that one passes untouched. */
static void *ds4_connected_thread(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    const bool bt = ds4_is_bt(c);
    const uint8_t drives = ds4_signal_drives();
    ds4_signal_run_t run;
    ds4_signal_play(c, bt, &k_ds4_connected, drives, true, &run);
    ctl_log(c, "signal: connected -- green breath %s, pulse %s: %d report(s), %d failed, %ldms%s",
            (drives & DS4_OUT_LIGHT) ? "on" : "off", (drives & DS4_OUT_MOTORS) ? "on" : "off",
            run.sent, run.failed, run.took_ms, run.cut ? ", cut short by a release" : "");

    uint32_t kept = controller_signal_kept(c);
    for (int round = 1;; ++round) {
        if ((drives & DS4_OUT_LIGHT) && !controller_signal_stopping(c)) {
            const bool seen = (kept & DS4_KEPT_COLOUR) != 0;
            const int rc = ds4_signal_write(c, bt, DS4_OUT_LIGHT, 0,
                                            seen ? (uint8_t)(kept >> 16) : k_ds4_connected.r,
                                            seen ? (uint8_t)(kept >> 8) : k_ds4_connected.g,
                                            seen ? (uint8_t)kept : k_ds4_connected.b);
            if (seen) {
                ctl_log(c, "signal: connected -- light handed back as the host's #%06x%s",
                        (unsigned)(kept & 0xffffffu), rc == 0 ? "" : " (write failed)");
            } else {
                ctl_log(c, "signal: connected -- light left green, the host has set no colour%s",
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
static void ds4_signal_connected(ctm_controller_t *c)
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
static void ds4_signal_unplugging(ctm_controller_t *c, ctm_unplug_reason_t why)
{
    const char *what;
    switch (why) {
    case CTM_UNPLUG_SHUTDOWN: what = "unplugging (shutdown)"; break;
    case CTM_UNPLUG_REPLACED: what = "unplugging (replaced)"; break;
    default:                  what = "unplugging (requested)"; break;
    }
    const bool bt = ds4_is_bt(c);
    const uint8_t drives = ds4_signal_drives();
    if (!drives) {
        ctl_log(c, "signal: %s -- light and rumble are switched off, nothing played", what);
        return;
    }
    ds4_signal_run_t run;
    ds4_signal_play(c, bt, &k_ds4_released, drives, false, &run);
    if ((drives & DS4_OUT_LIGHT) && !run.failed) {
        if (ds4_signal_write(c, bt, DS4_OUT_LIGHT, 0, 0, 0, 0) == 0) ++run.sent;
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
 * ⚠️ NOT A FIXED WINDOW. There used to be one beside this -- a 1.4 s hold from
 * the session opening, on the Bluetooth DualSense path -- and it was removed on
 * 2026-09-15 for never firing: the connected signal beside it runs longer than
 * the window did, so the host's claim always arrived after it closed. This
 * signal is the core's own and ends when it says so; withholding past that
 * would drop a colour the host set with nothing left to hand it back -- and
 * with the signals switched off there is no signal at all to do it.
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
    /* ⓘ The motors only until the pulse's stop is out -- see ds4_signal_play. */
    uint8_t hold = ds4_signal_drives();
    if (!controller_signal_motors_held(c)) hold = (uint8_t)(hold & ~DS4_OUT_MOTORS);
    const uint8_t taken = ds4_withhold_output(data, len, hold);
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

/* read_pad_mac: the pad's own MAC, from its pairing-info feature report 0x12.
 * When: opening the node on a cable, before the identity is chosen.
 *
 * ⭐ WHY. The C1's kernel leaves a cabled DS4's `uniq` empty, so the host was
 * told nothing and a config could not follow the pad. SDL on the same TV reads
 * this report and gets the MAC, so the pad answers it -- and a pad that
 * answers costs no 5 s wait.
 *
 * ⓘ The whole reply is logged, as the DualSense's 0x09 probe logs its own, so a
 * pad that answers in another shape is seen rather than misread. */
static bool ds4_usb_read_pad_mac(ctm_controller_t *c, int fd, char *out, size_t out_len)
{
    uint8_t feature[DS4_FEATURE_PAIRING_INFO_LEN];
    memset(feature, 0, sizeof(feature));
    feature[0] = DS4_FEATURE_PAIRING_INFO;
    if (ioctl(fd, HIDIOCGFEATURE(sizeof(feature)), feature) < 0) {
        ctl_log(c, "probe: feature 0x12 (pairing info) failed errno=%d", errno);
        return false;
    }
    char hex[3 * sizeof(feature) + 1];
    int o = 0;
    for (size_t i = 0; i < sizeof(feature); ++i) {
        o += snprintf(hex + o, sizeof(hex) - (size_t)o, "%02x ", feature[i]);
    }
    ctl_log(c, "probe: feature 0x12 (pairing info) = %s", hex);
    if (!ds4_mac_from_pairing_info(feature, sizeof(feature), out, out_len)) {
        ctl_log(c, "identity: 0x12 gave no usable MAC -- leaving the identity empty");
        return false;
    }
    return true;
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
    .read_pad_mac = ds4_usb_read_pad_mac,
    .on_input_report = ds4_on_input_report,
    .blank_input = ds4_blank_input,
    .patch_output = ds4_usb_patch_output,
    .signal_connected = ds4_signal_connected,
    .signal_unplugging = ds4_signal_unplugging,
};
