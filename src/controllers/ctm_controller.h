#ifndef CTM_CONTROLLER_H
#define CTM_CONTROLLER_H

/* Controller abstraction (D2). One mechanism per detected controller: each
 * type supplies an ops vtable; the factory picks the right ops for a device.
 *
 * STAGE 1 (scaffold, this commit): the interface + classification (matches) +
 * the factory. The shared byte pump in controller_common.c and the wiring into
 * plug_in_item are STAGE 2 — until then these ops do not yet drive live
 * sessions; the proven tv_bridge_worker / ctm_hidraw_bridge paths still run.
 *
 * The layer is UI-independent: the app fills a neutral ctm_controller_dev_t
 * from its logical_device_t, so controllers/ does not depend on app/ types. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctm_transport.h"
#include "ctm_settings.h"       /* tv_bridge_worker_settings_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char vid[8];
    char pid[8];
    char bus[8];      /* "USB" / "BT" */
    char name[128];
    char path[64];    /* /dev/hidrawN */
    char mac[64];     /* ⚠️ the kernel's uniq, which is not always a MAC */
    /* ⭐ The identity the host links a config on (device_identity.inl): uniq, or
     * the USB serial where no driver filled uniq. A DualSense replaces it with
     * its own MAC once its session has asked, and so does a cabled DS4 (its
     * type's read_pad_mac). */
    char serial[64];
    /* The kernel driver bound to the device, e.g. "xpad"; empty if unknown. */
    char driver[32];
} ctm_controller_dev_t;

typedef struct ctm_controller ctm_controller_t;   /* opaque; defined in stage 2 */

/* Why a controller is being unplugged. The routes mean different things and
 * end in the same place, so the caller says which it was: the log records it,
 * and the controller's own signal can eventually differ. */
typedef enum {
    CTM_UNPLUG_REQUESTED = 0,   /* the user asked -- gesture or overlay button */
    CTM_UNPLUG_SHUTDOWN,        /* everything torn down at once */
    CTM_UNPLUG_REPLACED         /* a stale controller displaced by a new one */
} ctm_unplug_reason_t;

typedef struct {
    const char *kind;   /* "ds5" / "ds4" / "ds4_usb" / "xbox" / "xpad" / "steam_puck" / "generic" */

    /* Behaviour flags preserving each path's proven semantics in the shared
     * pump. DS (worker) = all true; puck, xbox and xpad = all false; generic
     * grabs keyboards and mice and nothing else. */
    bool needs_host_config;   /* block for HOST_CONFIG after HELLO (DS pacing) */
    /* EVIOCGRAB the device's input nodes, so the TV stops using its input while
     * the host has it. ⛔ Generic was false until 2026-09-13, and a bridged
     * keyboard typed every key twice: once through the bridge, once through the
     * stream, because the TV still read it.
     *
     * ⛔⛔ BUT NEVER A PAD SDL READS THROUGH EVDEV. A grab hides it from SDL,
     * and rhoquinn8217 set that the overlay combo works while bridged. ⓘ A
     * DualSense is grabbed safely only because SDL opens it through hidraw,
     * which a grab does not touch. A handed-over pad needs no grab to stay off
     * the host: Moonlight's pad for it is retired. */
    bool grab_evdev;
    /* With grab_evdev: leave the device alone when its report descriptor says
     * joystick, gamepad or multi-axis controller. For generic, which serves
     * keyboards, mice and pads alike. */
    bool grab_skips_gamepads;
    bool request_bt_mode;     /* send the Sony feature-0x05 full-BT-mode probe */
    bool composite;           /* forward EVERY HID interface, each tagged by its IN
                               * endpoint (puck); host plugs the whole composite. */

    /* ⭐⭐ Does this device speak the DualSense protocol? Its feature report
     * 0x09, its Bluetooth reports 0x31, 0x32 and 0x36, its microphone and its
     * USB sound card. True for the DualSense and the Edge, and nothing else.
     *
     * ⛔ Every session step built for a DualSense asks this before it runs: the
     * identity probe, the speaker wake, the card match, the microphone, the
     * confirmation tone and the microphone-safety check. They used to ask the
     * bus, or whether an audio device was open, and both say yes for things
     * that are not DualSenses. Measured on the U5s 2026-09-13: a keyboard
     * dongle waited 5 s at every bridge for a report only a DualSense answers,
     * and was sent 242 DualSense sound reports at bridge and again at release.
     *
     * ⚠️ Not the same question as grab_evdev or needs_host_config, which a DS4
     * also answers yes to. A DS4 speaks its own protocol, not this one. */
    bool speaks_ds5;

    /* Does this type claim the device? Factory tries specific types first,
     * generic last. */
    bool (*matches)(const ctm_controller_dev_t *dev);

    /* Choose which /dev/hidrawN to bridge for composite devices. NULL => the
     * app default (dev->path). steam_puck overrides to pick the gamepad
     * interface (fixes the §6a "first hidraw wins" bug). */
    int (*select_node)(const ctm_controller_dev_t *dev, char *out, size_t out_len);

    /* Optional one-shot init after plug, before the pump starts (xbox GIP
     * handshake, puck lizard-mode exit). NULL => none. */
    int (*on_plug_init)(ctm_controller_t *c, ctm_transport_t *t);

    /* ⭐ A TYPE'S OWN READ OF ITS PAD'S MAC ON A CABLE, for a type that does not
     * speak the DualSense protocol (a DS4's feature report 0x12; a DualSense's
     * 0x09 is asked through speaks_ds5). Fill `out` with "aa:bb:cc:dd:ee:ff" and
     * return true, or return false when the pad gave nothing usable. The result
     * becomes this pad's identity, as the DualSense's does. NULL => none.
     * When: opening the node, USB only, before the identity the host links a
     * config on is chosen. ⛔ Set it only for a pad that answers: a device that
     * ignores a feature request holds the open for the kernel's 5 s timeout. */
    bool (*read_pad_mac)(ctm_controller_t *c, int fd, char *out, size_t out_len);

    /* Peek at each input report as it is relayed, before it goes to the host.
     * Read-only: the report is forwarded unchanged either way. NULL => no peek.
     * When: the controller's own input thread, once per report. Must not block
     * -- it runs in the relay path. */
    void (*on_input_report)(ctm_controller_t *c, const uint8_t *data, size_t len);

    /* ⭐⭐ Blank an INBOUND report in place: buttons up, sticks centred, triggers
     * released -- and nothing else touched. Used while the TV's own overlay is
     * open, so the host keeps receiving reports at the usual rate and simply
     * sees nothing pressed.
     *
     * ⛔ WHY NOT SIMPLY DROP THE REPORT: a host holds the last state it was
     * given, so a button held when the overlay opened would stay held in the
     * game. Sending a blank report is what releases it. ⓘ It also keeps the
     * cadence steady, so nothing upstream concludes the controller has gone.
     *
     * NULL => the pump blanks a gamepad by a plan built from its report
     * descriptor (pad_blank.inl), and a Pro Controller's full report by its own
     * layout; a report nothing places is relayed unchanged. */
    void (*blank_input)(uint8_t *data, size_t len);

    /* Patch an outbound report in place before it reaches the device (DS audio
     * route / volume / CRC). Returns nonzero to DROP the report (suppress the
     * write), 0 to write `*len` bytes. NULL => verbatim forward. */
    int (*patch_output)(ctm_controller_t *c, uint8_t *buf, size_t *len);

    /* Live UI settings update (DS sliders). NULL => ignored. */
    void (*set_settings)(ctm_controller_t *c, const tv_bridge_worker_settings_t *s);

    /* ⭐⭐ A TYPE'S OWN CONFIRMATION SIGNALS, for a controller that does not
     * speak the DualSense protocol and so gets none of its signals (a cabled
     * DS4's light and rumble). Called only when speaks_ds5 is false. NULL =>
     * no signal, which is what every such type had before.
     *
     *   signal_connected   the session has started. When: the session thread,
     *                      at the spot a DualSense's connected signal starts.
     *                      ⛔ MUST NOT BLOCK -- that thread carries the reports.
     *                      Play on a thread of your own, and claim the
     *                      controller for it with controller_signal_begin first.
     *   signal_unplugging  the controller is being released. When: plug-out,
     *                      before the session stops and the node closes, and
     *                      after any signal thread this controller had running
     *                      has finished. Synchronous: return once it has played. */
    void (*signal_connected)(ctm_controller_t *c);
    void (*signal_unplugging)(ctm_controller_t *c, ctm_unplug_reason_t why);

    /* ⭐⭐ A DEVICE THE KERNEL GIVES ONLY AS AN INPUT DEVICE, NOT HID.
     *
     * A wired Xbox pad under xpad has no hidraw node, so there are no reports
     * to relay. These let a type read its input node and make the reports
     * itself. All NULL for every hidraw type, which keeps the pump exactly as
     * it was for them -- except `current_report` and `keepalive_ms` for a
     * hidraw pad that reports only when something changes: a Bluetooth Xbox
     * pad (controller_xbox.c) keeps its last report and has it sent again, and
     * the generic type sets only `keepalive_ms`, so the pump keeps a gamepad's
     * last report itself. ⭐ Either way, until the pad has sent a report, the
     * pump sends one with nothing pressed built from its descriptor
     * (pad_blank.inl), so a pad bridged and never touched is not dropped.
     *
     *   preflight       can the device be opened; 0 or the errno. Replaces
     *                   the hidraw check before BRIDGE_START.
     *   open_input      open the node, fill caps (no report descriptor),
     *                   return the fd or -1. The pump then GRABS THIS FD
     *                   ITSELF: a grab through another handle would take the
     *                   events from this one too.
     *   read_input      turn what the node has into one report; its length,
     *                   0 for nothing complete yet, -1 on error.
     *   current_report  the report for the present state, with no I/O. Sent
     *                   at once when a session starts and again whenever
     *                   `keepalive_ms` pass with nothing sent, because an
     *                   input node is silent while nothing moves. ⛔ Not only
     *                   for the listener's 15 s silence limit: its cursor,
     *                   scroll and turbo advance when a report arrives, so a
     *                   pad needs the steady stream a hidraw pad sends (every
     *                   4 ms for an Xbox pad).
     *   write_output    act on a report from the host (rumble); 0 or -1. */
    int (*preflight)(const ctm_controller_dev_t *dev);
    int (*open_input)(ctm_controller_t *c, const ctm_controller_dev_t *dev,
                      ctmb_device_caps_t *caps);
    int (*read_input)(ctm_controller_t *c, int fd, uint8_t *report, size_t cap);
    int (*current_report)(ctm_controller_t *c, uint8_t *report, size_t cap);
    int (*write_output)(ctm_controller_t *c, int fd, const uint8_t *report, size_t len);
    unsigned keepalive_ms;
} ctm_controller_ops_t;

/* Live bridging status — read-only snapshot for the UI status panel. */
typedef struct {
    bool connected;              /* a session is up (handshake done, looping) */
    bool transport_enet;         /* true = ENet/UDP, false = TCP */
    unsigned long reports_in;    /* input reports forwarded to the host */
    unsigned long reports_out;   /* output reports written to the device */
    char last_event[96];         /* most recent controller log line */

    /* ⭐⭐ THE HOST IS GONE AND THE SESSION HAS STOPPED TRYING. T-127.
     *
     * ⛔ NOT the same as `connected` being false. A session disconnects and
     * reconnects routinely -- a listener restart, a stream hiccup -- and the
     * reconnect loop is meant to ride that out. **This says the loop has given
     * up**, after fifteen seconds of a host that will not answer.
     *
     * ⭐ WHY THE CORE CANNOT ACT ON IT ITSELF: releasing a controller is
     * app-side work. The emulated pad has to be retired, Moonlight's input
     * restored for that pad, the panel row updated and the light pulsed. ⓘ None
     * of that is reachable from here, and the app already has a release path
     * that does all of it -- the one the panel button calls.
     *
     * ➡️ So this is a FLAG, and the app's existing status tick acts on it. */
    bool host_gone;
} ctm_controller_status_t;

/* --- lifecycle (controller_common.c) ----------------------------------------
 * Each controller runs in isolation: its own pump (reader + session threads),
 * HID fd, transport, settings, and per-MAC log file. */

/* Can this device be bridged at all? Opens the node the session would open,
 * checks it is the device the row says (a HID device, or the type's own check
 * for an input-node type), and closes it again. Returns 0, or the errno that
 * refused it.
 *
 * When: before BRIDGE_START. ⛔ A node that cannot be read used to be found out
 * on the session thread, after the host had built a session for it and the row
 * already read bridged -- and nothing then told either of them. */
int controller_preflight(const ctm_controller_dev_t *dev);

/* Somewhere for a TYPE to keep its own per-controller state, which the opaque
 * struct otherwise gives it no room for. Freed with free() when the controller
 * is destroyed. When: an input-node type keeps its pad state here. */
void *controller_type_ctx(const ctm_controller_t *c);
void controller_set_type_ctx(ctm_controller_t *c, void *ctx);

/* Is this a Microsoft Xbox pad's product id? Shared by the Bluetooth type and
 * the wired, input-node type. */
bool xbox_known_pid(const char *pid);

ctm_controller_t *ctm_controller_create(const ctm_controller_dev_t *dev);
int  ctm_controller_plug_in(ctm_controller_t *c, const char *host, int port);
/* ⓘ ctm_unplug_reason_t is declared at the top, beside the opaque type: the
 * ops table's signal_unplugging takes one. */

void ctm_controller_plug_out(ctm_controller_t *c);
void ctm_controller_plug_out_reason(ctm_controller_t *c, ctm_unplug_reason_t why);
/* Composite: forwarded enumeration payload (CTMB_MSG_ENUM), sent before HELLO. */
void ctm_controller_set_enum_payload(ctm_controller_t *c, const uint8_t *payload, int len);
/* CONFIRMATION SIGNALS -- ALL OR NOTHING.
 *
 * One switch for every signal on every transport: the tone, the felt pulse,
 * the lightbar patterns, and the SDL fallback. Not a tone switch and a
 * transport rule -- one concept.
 *
 * ⭐ AND IT COVERS THE FAILURE SIGNALS TOO, deliberately. rhoquinn8217, 2026-08-12:
 * "if you are not using the confirmation signals, you are putting on a
 * blindfold and getting angry that you are blindfolded when things go wrong."
 * A switch that silences success but keeps failure sounds thoughtful and is
 * really just a second thing to reason about.
 *
 * It becomes a real setting when the status page exists -- deliberately no
 * button for it yet. */
#define CTM_SIGNALS_ENABLED 1

/* ⓘ T-120 REBUILT THE BLUETOOTH PATH ONE LAYER AT A TIME, behind switches: the
 * core's Bluetooth confirmation signal (BT_LAYER_CORE_SIGNAL) and what the
 * output patcher writes into a bridged pad's reports -- block 0x90 volumes,
 * routing and the speaker's audio frames 0x93-0x96 (BT_FEAT_AUDIO), 0x91 the
 * audio buffer (BT_FEAT_LATENCY), 0x92 haptics gain (BT_FEAT_HAPTICS). Every
 * layer came back on, the switches stayed pinned to 1, and they were removed
 * with the branches they guarded on 2026-09-15. The user's own settings are
 * what decide those writes now. */

/* Signal a REFUSED plug, with no session behind it.
 *
 * A failed plug leaves nothing -- no controller object, no open device -- so
 * the refusal signal has always been the coarse SDL one. It does not have to
 * be:
 *
 *   Bluetooth: the whole signal needs only the device node and bytes, and the
 *     node is still there. Nothing here depends on the host, which is the
 *     point -- a plug fails BECAUSE the host is unreachable.
 *
 *   Wired: the sound card is made by the kernel when the controller is
 *     plugged into the TV, not by anything we do, so it is sitting there
 *     unopened. ⚠️ Declines with more than one DualSense plugged in -- there is
 *     no way to tell from here which card belongs to which, and buzzing the
 *     wrong one is worse than buzzing coarsely.
 *
 * Both open, play, close. Nothing is held afterwards. `pattern` is a
 * btsig_pattern_t. */
/* ⭐⭐ SILENCE EVERY CONTROLLER'S MICROPHONE BEFORE SDL OPENS ANY OF THEM.
 *
 * ⚠️ MUST BE CALLED BEFORE SDL's controller subsystem starts. A controller
 * told to stream microphone audio keeps doing so when a program dies -- it
 * only forgets when its Bluetooth link drops. So an app that crashed while one
 * was streaming comes back to find SDL reading encoded sound as sticks and
 * buttons, which is exactly what happened on 2026-08-13.
 *
 * Moving this call later would quietly remove the protection. */
void ctm_mic_safety_disarm_all(void);

int ctm_signal_refused_bt(const char *node);
int ctm_signal_wired_no_session(const char *node, int pattern);

void ctm_controller_set_settings(ctm_controller_t *c, const tv_bridge_worker_settings_t *s);

/* The confirmation tone, Bluetooth only: pre-encoded Opus handed to the
 * report builder one frame at a time. Over a cable the tone is generated as
 * it plays and written to the audio device instead. */
void ctm_controller_tone_start(ctm_controller_t *c);
bool ctm_controller_tone_pending(ctm_controller_t *c);
int  ctm_controller_tone_take(ctm_controller_t *c, uint8_t *dst, int n);
void ctm_controller_get_settings(ctm_controller_t *c, tv_bridge_worker_settings_t *out);
void ctm_controller_get_status(ctm_controller_t *c, ctm_controller_status_t *out);
void ctm_controller_destroy(ctm_controller_t *c);

/* Register a sink for controller log lines (e.g. the app's on-screen console).
 * NULL = file + stderr only. The sink must be thread-safe — controllers log
 * from their own threads. When: app startup. */
void ctm_controller_set_log_sink(void (*sink)(const char *line));

/* Append the Sony BT HID output-report CRC32 (seed 0xa2) to the trailing 4
 * bytes. When: a DS patch_output hook after rewriting a report. Defined in
 * controller_common.c so DS4 + DS5 share it. */
void ctm_bt_sign_output(uint8_t *data, size_t len);

/* How this controller is attached: "USB" or "BT". When: a type behaves
 * differently per transport -- report formats differ between the two. */
const char *ctm_controller_bus(const ctm_controller_t *c);

/* Switch the UNBRIDGE chord on or off. ⭐ The app owns the setting and owns the
 * bridge half of the gesture; this is the half it cannot see. Defaults on. */
void ctm_gesture_set_enabled(int on);
/* That same switch, and the DualSense chord's hold in milliseconds, for another
 * type's unbridge chord (a DS4's). ⭐ Read from ctm_gesture_chord.inl, so there
 * is one switch and one hold, not a copy of each that can drift. */
int gesture_chord_enabled(void);
int gesture_chord_hold_ms(void);

/* ⭐⭐ Hold a bridged controller's INPUT while the TV's own overlay is open.
 *
 * ⛔ THE FAULT: with the streaming overlay up, a bridged controller's presses
 * still reached the game behind it, so navigating the panel played the game at
 * the same time. An UNBRIDGED controller does not do this -- the app routes SDL
 * events and stops forwarding them while the overlay is open. A bridged
 * controller produces no SDL events at all; its reports go TV -> USB/IP -> PC
 * and pass nothing that could hold them.
 *
 * ⭐ While held, reports are BLANKED rather than dropped -- see blank_input.
 * ⓘ Everything else keeps flowing: audio, rumble, the lightbar, and the
 * unbridge chord, which is read from the REAL report before it is blanked. */
void ctm_input_set_held(int held);

/* ⭐⭐ WHICH CONFIRMATION SIGNALS THIS SIDE IS ALLOWED TO MAKE.
 *
 * ⓘ The light, the felt pulse and the tone, each on or off, for a handover, a
 * handback AND a refusal. The app owns the settings and hands them in when a
 * stream starts, the same way it hands in the host address.
 *
 * ⚠️ These are "I do not want that" switches -- a bright light in a dark room,
 * a buzz at midnight, a chirp while someone is asleep. ⓘ The battery saving is
 * small for rumble and the tone and negligible for the light, so it is not what
 * they are for.
 *
 * ⛔ WITH ALL THREE OFF A REFUSAL IS INVISIBLE: the chord does nothing, and
 * there is no way to tell that from a gesture that was not recognised. The
 * settings screen says so.
 *
 * ⓘ All default ON, so a core told nothing behaves as it always has. */
void ctm_signals_set_enabled(int light, int rumble, int tone);
/* Is the felt pulse allowed right now? For a type that confirms with a rumble
 * of its own rather than the DualSense signal. */
int signals_rumble_on(void);

/* ⭐⭐ WILL THE CORE PLAY A CONNECTED SIGNAL FOR THIS DEVICE? (T-212)
 *
 * ⛔ THE FAULT THIS EXISTS FOR. The TV stands aside at a bridge and lets the
 * core signal the pad -- and for a Bluetooth Xbox pad or a Bluetooth DS4 the
 * core has nothing to play, so NOBODY signals and the bridge is silent. The TV
 * could not tell the difference: it asked whether the pad was plugged, which is
 * true either way.
 *
 * ⭐ So it asks this instead, and the answer is computed from the SAME two
 * fields the core's own branch uses -- `speaks_ds5`, which plays the DualSense's
 * tone and light on both transports, and `signal_connected`, which only a cabled
 * DS4 has. ⚠️ Keep it that way: a copy of the rule on the TV side would drift
 * from the branch in controller_common.c the first time a type gains a signal. */
int ctm_controller_will_signal_connect(const ctm_controller_dev_t *dev);
/* Is the light allowed right now? The same, for a type that paints a lightbar
 * of its own. ⓘ Like the pulse's, it answers no when CTM_SIGNALS_ENABLED is off. */
int signals_light_on(void);

/* Capture the controller's microphone while it is bridged. ⭐ Only useful for
 * voice chat through the controller itself. ⓘ Defaults on. */
void ctm_mic_capture_set_enabled(int on);

/* Open the controller's own USB audio playback device, for wired audio and
 * haptics. When: on plug, for a wired DualSense or Edge. Idempotent. */
void ctm_controller_open_alsa_playback(ctm_controller_t *c);

/* Send a report to the device as-is, bypassing the type's patch hook. When: a
 * type needs to say something to the controller itself rather than pass a host
 * report along. */
int ctm_controller_write_raw(ctm_controller_t *c, const uint8_t *data, size_t len);

/* ⭐⭐ A TYPE'S CONFIRMATION SIGNAL ON A THREAD OF ITS OWN, and what keeps that
 * thread from outliving the node it writes to.
 *
 * ⛔ THE HAZARD: plug-out closes the node and the caller frees the controller
 * straight after, so a signal thread still writing when a release lands would
 * write to a closed -- or already reused -- descriptor and read freed memory.
 * ➡️ So a signal thread holds the controller from begin to end, and plug-out
 * waits for it before closing anything.
 *
 *   controller_signal_begin        claim the controller for a signal, on the
 *                                  session thread, before starting the thread.
 *                                  False when one is still running or plug-out
 *                                  has begun -- and then start nothing.
 *   controller_signal_end          the thread's LAST touch of the controller.
 *                                  With `gave_back`, it ends only if the host
 *                                  has asked for nothing newer than that value
 *                                  since; false means give the newer one back
 *                                  and ask again. NULL ends it regardless.
 *   controller_signal_stopping     plug-out is waiting: stop at the next step.
 *   controller_signal_host_report  a host report is passing: keep `value` as
 *                                  the host's latest when `keep`, and say
 *                                  whether a signal holds the controller -- one
 *                                  is playing, or plug-out has begun.
 *   controller_signal_kept         the host's latest kept value, 0 if none.
 *   controller_signal_motors_done  the signal thread has sent its pulse's stop
 *                                  and no longer drives the motors.
 *   controller_signal_motors_held  whether the host's motor claims are still to
 *                                  be withheld: a signal is playing and has not
 *                                  let the motors go, or plug-out has begun.
 *
 * ⓘ What the value means is the type's: a DS4 keeps the lightbar colour the
 * host last set, so its signal can hand the light back as it found it.
 * ⓘ The motors go back sooner than the light: a pulse is a fraction of a breath,
 * and a game's rumble should not wait for the colour to finish. */
bool controller_signal_begin(ctm_controller_t *c);
bool controller_signal_end(ctm_controller_t *c, const uint32_t *gave_back);
bool controller_signal_stopping(ctm_controller_t *c);
/* Asked from inside a signal's own write loop: is this a claimed signal that a
 * plug-out is now waiting on? A release tone, which runs after the close, says
 * no -- see the note on the definition. */
bool controller_signal_cancelled(ctm_controller_t *c);
bool controller_signal_host_report(ctm_controller_t *c, bool keep, uint32_t value);
uint32_t controller_signal_kept(ctm_controller_t *c);
void controller_signal_motors_done(ctm_controller_t *c);
bool controller_signal_motors_held(ctm_controller_t *c);

/* Write a line to this controller's own log (/tmp/ctm-<mac-or-kind>.log), and
 * to the app's console sink if one is set. When: a type wants to record
 * something about its device. Cheap, but it opens a file -- do not call it per
 * report in the relay path without throttling. */
void ctl_log(ctm_controller_t *c, const char *fmt, ...);

/* Ask for this controller to be unplugged. When: a type's on_input_report
 * recognises a local gesture. Sets a flag and notifies the app; it does NOT
 * tear down, because the caller is the input thread and unplugging joins that
 * same thread. */
/* Write a line to ctm-gesture.log in the app's logs directory (see
 * ctm_log_path), the file the app's plug-in watcher also writes, so a gesture
 * reads end to end in one place. `c` may be NULL when the caller holds a key
 * rather than a controller. */
void ctm_gesture_log(const ctm_controller_t *c, const char *fmt, ...);

void ctm_controller_request_unplug(ctm_controller_t *c);
bool ctm_controller_unplug_requested(const ctm_controller_t *c);

/* Told when some controller has requested an unplug, so the app can do it on a
 * thread of its own. Invoked on the requesting controller's input thread, so
 * the handler must only signal, never tear down. */
typedef void (*ctm_controller_unplug_cb)(ctm_controller_t *c);
void ctm_controller_set_unplug_cb(ctm_controller_unplug_cb cb);

/* ⭐ Told when a bridged keyboard presses the streaming overlay's shortcut,
 * Ctrl+Alt+Shift+O, which its own grab keeps from the app. When: the keyboard's
 * input thread, so the app hands the work to its main thread. */
typedef void (*overlay_request_cb)(void);
void controller_set_overlay_cb(overlay_request_cb cb);

/* Scratch value owned by the controller's TYPE, one per controller. When: a
 * type needs to remember something between reports -- gesture timing, say.
 * Deliberately per-controller: a file-level variable here would be shared by
 * every controller's thread, which is the fault that took four sessions to
 * find in the capture path. */
#define CTM_TYPE_STATE_SLOTS 3
uint64_t ctm_controller_type_state(const ctm_controller_t *c, int slot);
void ctm_controller_set_type_state(ctm_controller_t *c, int slot, uint64_t v);

/* Per-type ops tables, defined in controller_<kind>.c. */
extern const ctm_controller_ops_t ctm_controller_ds5_ops;
extern const ctm_controller_ops_t ctm_controller_ds5e_ops;
extern const ctm_controller_ops_t ctm_controller_ds4_ops;
extern const ctm_controller_ops_t ctm_controller_xbox_ops;
extern const ctm_controller_ops_t ctm_controller_steam_puck_ops;
extern const ctm_controller_ops_t ctm_controller_generic_ops;
/* A wired Xbox pad, read through its input node (controller_xpad.c). */
extern const ctm_controller_ops_t controller_xpad_ops;
/* A cabled DualShock 4 (controller_ds4.c). */
extern const ctm_controller_ops_t controller_ds4_usb_ops;

/* Pick ops for a device: specific types first (puck/ds5/ds4/xbox), generic
 * fallback. Never returns NULL. */
const ctm_controller_ops_t *ctm_controller_ops_for(const ctm_controller_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* CTM_CONTROLLER_H */
