#ifndef CTM_SETTINGS_H
#define CTM_SETTINGS_H

/* Per-controller tunables (audio route/volume, latency, haptics, DS5 patch
 * nibbles), shared by the UI and the controller layer. Extracted from
 * tv_bridge_worker.h so neither depends on the worker (D2 stage 2 removes it).
 * Type names kept for now; a rename to ctm_settings_t lands when
 * tv_bridge_worker is deleted. */

typedef enum {
    TV_BRIDGE_KIND_HID = 0,
    TV_BRIDGE_KIND_DS4 = 4,
    TV_BRIDGE_KIND_DS5 = 5
} tv_bridge_kind_t;

typedef enum {
    /* Auto: no patching of route/volume/enable bits — let the host (game)
     * drive everything. Only the latency byte (BT 0x91 timing block) is still
     * patched. This is the default. */
    TV_BRIDGE_AUDIO_AUTO = 0,
    TV_BRIDGE_AUDIO_OFF = 1,
    TV_BRIDGE_AUDIO_SPEAKER = 2,
    TV_BRIDGE_AUDIO_HEADSET = 3,
    TV_BRIDGE_AUDIO_BOTH = 4
} tv_bridge_audio_mode_t;

typedef struct {
    tv_bridge_kind_t kind;
    tv_bridge_audio_mode_t audio_mode;
    unsigned int latency_ms;
    unsigned int haptics_gain_centi;
    unsigned int headset_volume_percent;
    unsigned int speaker_volume_percent;

    /* ⭐⭐ THE HOST CONFIG SET THE VOLUMES, as opposed to these being the TV's
     * own slider defaults. T-130, 2026-08-25.
     *
     * ⛔ WHY THE DISTINCTION IS NEEDED AT ALL: in AUTO the TV deliberately
     * defers to the game -- it fills the volume in only when the host left it
     * at zero, and its own comment says so: *"when it HAS asked for something,
     * leave it entirely alone -- that is what AUTO is for, and this must not
     * become a second Speaker mode."*
     *
     * ⚠️ That was written when there was one host. There are now two things
     * asking: the GAME, sending 0x64 in the report, and the USER's
     * per-controller configuration on Windows. **A user who sets a volume
     * means it**, and AUTO refusing them is the fault this flag fixes.
     *
     * ⭐ SET ONLY BY CTMB_MSG_HOST_CONFIG, never by the sliders. So the TV's own
     * defaults still defer to the game exactly as before, and nothing changes
     * for anyone who has not configured a volume.
     *
     * ⓘ This is how latency already behaves -- it overwrites the host's value
     * unconditionally in AUTO. The two settings were never symmetrical, and the
     * header note says as much: AUTO *"touches only the latency block"*. */
    unsigned int host_audio_set;
    unsigned int ds5_patch_high_nibble;
    unsigned int ds5_patch_low_nibble;
    unsigned int ds5_patch2_high_nibble;
    unsigned int ds5_patch2_low_nibble;
} tv_bridge_worker_settings_t;

#endif /* CTM_SETTINGS_H */
