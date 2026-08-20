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
    char mac[64];
} ctm_controller_dev_t;

typedef struct ctm_controller ctm_controller_t;   /* opaque; defined in stage 2 */

typedef struct {
    const char *kind;   /* "ds5" / "ds4" / "xbox" / "steam_puck" / "generic" */

    /* Behaviour flags preserving each path's proven semantics in the shared
     * pump. DS (worker) = all true; puck/xbox/generic (relay) = all false. */
    bool needs_host_config;   /* block for HOST_CONFIG after HELLO (DS pacing) */
    bool grab_evdev;          /* EVIOCGRAB the device's evdev nodes (BT/DS) */
    bool request_bt_mode;     /* send the Sony feature-0x05 full-BT-mode probe */
    bool composite;           /* forward EVERY HID interface, each tagged by its IN
                               * endpoint (puck); host plugs the whole composite. */

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

    /* Peek at each input report as it is relayed, before it goes to the host.
     * Read-only: the report is forwarded unchanged either way. NULL => no peek.
     * When: the controller's own input thread, once per report. Must not block
     * -- it runs in the relay path. */
    void (*on_input_report)(ctm_controller_t *c, const uint8_t *data, size_t len);

    /* Patch an outbound report in place before it reaches the device (DS audio
     * route / volume / CRC). Returns nonzero to DROP the report (suppress the
     * write), 0 to write `*len` bytes. NULL => verbatim forward. */
    int (*patch_output)(ctm_controller_t *c, uint8_t *buf, size_t *len);

    /* Live UI settings update (DS sliders). NULL => ignored. */
    void (*set_settings)(ctm_controller_t *c, const tv_bridge_worker_settings_t *s);
} ctm_controller_ops_t;

/* Live bridging status — read-only snapshot for the UI status panel. */
typedef struct {
    bool connected;              /* a session is up (handshake done, looping) */
    bool transport_enet;         /* true = ENet/UDP, false = TCP */
    unsigned long reports_in;    /* input reports forwarded to the host */
    unsigned long reports_out;   /* output reports written to the device */
    char last_event[96];         /* most recent controller log line */
} ctm_controller_status_t;

/* --- lifecycle (controller_common.c) ----------------------------------------
 * Each controller runs in isolation: its own pump (reader + session threads),
 * HID fd, transport, settings, and per-MAC log file. */
ctm_controller_t *ctm_controller_create(const ctm_controller_dev_t *dev);
int  ctm_controller_plug_in(ctm_controller_t *c, const char *host, int port);
/* Why a controller is being unplugged. The routes mean different things and
 * end in the same place, so the caller says which it was: the log records it,
 * and the controller's own signal can eventually differ. */
typedef enum {
    CTM_UNPLUG_REQUESTED = 0,   /* the user asked -- gesture or overlay button */
    CTM_UNPLUG_SHUTDOWN,        /* everything torn down at once */
    CTM_UNPLUG_REPLACED         /* a stale controller displaced by a new one */
} ctm_unplug_reason_t;

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

/* T-120: the Bluetooth confirmation tone, gated separately so the layered
 * rebuild in aurora can add it last. Bluetooth-only by construction -- it is
 * read inside the alsa_fd < 0 branch of feedback_play(). Wired is untouched.
 * The matching gates for gesture, light and rumble live in aurora's
 * ctm_bridge_gesture.c. */
/* ⚠️ NAMED BT_LAYER_TONE UNTIL STEP 5, WHICH WAS MISLEADING. It gates the
 * core's WHOLE Bluetooth signal -- light, sound and feel together. On Bluetooth
 * the controller's speaker and its haptics are the same audio device and the
 * lightbar rides the same report, so the three arrive as one stream and cannot
 * be gated apart. The old name cost a step to notice: BT_LAYER_RUMBLE could do
 * nothing while this was off, and nobody expected a "tone" switch to silence a
 * rumble. */
#define BT_LAYER_CORE_SIGNAL 1

/* ⭐⭐ T-120: WHAT THE BRIDGE WRITES TO A CONTROLLER WHILE IT IS BRIDGED.
 *
 * The BT_LAYER_ gates above cover the CONFIRMATION signals -- the things that
 * happen once, at the moment of bridging. These cover the ongoing ones: what
 * the output patcher puts into every report the host sends.
 *
 * ⛔ Separate on purpose. A confirmation that costs five seconds is a bad
 * bridge; an ongoing write that costs anything is a bad SESSION, and the two
 * fail in ways that look nothing alike.
 *
 * ⚠️ THESE ARE NOT BLUETOOTH-ONLY. The patcher runs on the Bluetooth report
 * format, so a cable never reaches it -- but that is a property of the report,
 * not of a check, and it is worth knowing the difference. A wired controller
 * gets its audio through ALSA and its haptics inside the same reports.
 *
 * ⓘ All 1: nothing is switched off. They exist so a layer can be removed for
 * one build and put back, the way the confirmation gates were.
 *
 *   BT_FEAT_AUDIO    block 0x90 -- volumes, routing, echo cancellation
 *                    and 0x93-0x96 -- the speaker's own audio frames
 *   BT_FEAT_LATENCY  block 0x91 -- the audio buffer, host-owned
 *   BT_FEAT_HAPTICS  block 0x92 -- haptics gain
 *
 * ⛔ THE LIGHTBAR HAS NO GATE HERE, and that is not an oversight: the core
 * writes no lightbar at all. It belongs to the player colour from the app side
 * and to the Bluetooth confirmation signal, which BT_LAYER_LIGHT already
 * covers. */
/* ⭐ How long after a session opens the relay withholds the host's lightbar
 * claim, so the app's confirmation pattern has the light to itself.
 *
 * ⓘ Slightly longer than one breath (1100 ms), and far shorter than anything a
 * game would notice. ⛔ Not a policy about who owns the lightbar -- the host
 * does. This is the handover. */
#define LIGHT_HOLD_MS    1400

#define BT_FEAT_AUDIO    1
#define BT_FEAT_LATENCY  1
#define BT_FEAT_HAPTICS  1

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

/* Tell the core a device node has appeared, so a tone can wait for its audio
 * to become usable. ⭐ A cable's speaker takes seconds to work after plug-in. */
void ctm_feedback_note_appeared(const char *node);

/* Should the host's lightbar claim be withheld right now?
 *
 * ⭐ True only for the moment after a session opens, while the app draws its
 * confirmation pattern. See LIGHT_HOLD_MS. ⓘ An accessor because the struct is
 * opaque outside controller_common.c. */
bool ctm_controller_light_held(ctm_controller_t *c);

/* Open the controller's own USB audio playback device, for wired audio and
 * haptics. When: on plug, for a wired DualSense or Edge. Idempotent. */
void ctm_controller_open_alsa_playback(ctm_controller_t *c);

/* Send a report to the device as-is, bypassing the type's patch hook. When: a
 * type needs to say something to the controller itself rather than pass a host
 * report along. */
int ctm_controller_write_raw(ctm_controller_t *c, const uint8_t *data, size_t len);

/* Write a line to this controller's own log (/tmp/ctm-<mac-or-kind>.log), and
 * to the app's console sink if one is set. When: a type wants to record
 * something about its device. Cheap, but it opens a file -- do not call it per
 * report in the relay path without throttling. */
void ctl_log(ctm_controller_t *c, const char *fmt, ...);

/* Ask for this controller to be unplugged. When: a type's on_input_report
 * recognises a local gesture. Sets a flag and notifies the app; it does NOT
 * tear down, because the caller is the input thread and unplugging joins that
 * same thread. */
/* Write a line to /tmp/ctm-gesture.log, the file the app's plug-in watcher
 * also writes, so a gesture reads end to end in one place. `c` may be NULL
 * when the caller holds a key rather than a controller. */
void ctm_gesture_log(const ctm_controller_t *c, const char *fmt, ...);

void ctm_controller_request_unplug(ctm_controller_t *c);
bool ctm_controller_unplug_requested(const ctm_controller_t *c);

/* Told when some controller has requested an unplug, so the app can do it on a
 * thread of its own. Invoked on the requesting controller's input thread, so
 * the handler must only signal, never tear down. */
typedef void (*ctm_controller_unplug_cb)(ctm_controller_t *c);
void ctm_controller_set_unplug_cb(ctm_controller_unplug_cb cb);

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

/* Pick ops for a device: specific types first (puck/ds5/ds4/xbox), generic
 * fallback. Never returns NULL. */
const ctm_controller_ops_t *ctm_controller_ops_for(const ctm_controller_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* CTM_CONTROLLER_H */
