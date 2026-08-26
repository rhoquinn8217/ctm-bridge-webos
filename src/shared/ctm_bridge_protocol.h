#ifndef CTM_BRIDGE_PROTOCOL_H
#define CTM_BRIDGE_PROTOCOL_H

#include <stdint.h>

#define CTMB_MAGIC 0x54424d43u
#define CTMB_VERSION 1u
#define CTMB_FLAG_OK 0x00000001u
#define CTMB_FLAG_PACED 0x00000002u
#define CTMB_MAX_PAYLOAD 65536u

enum ctmb_message_type {
    CTMB_MSG_HELLO = 1,
    CTMB_MSG_HOST_CONFIG = 2,
    CTMB_MSG_INPUT_REPORT = 3,
    CTMB_MSG_OUTPUT_REPORT = 4,
    CTMB_MSG_FEATURE_GET = 5,
    CTMB_MSG_FEATURE_REPORT = 6,
    CTMB_MSG_LOG = 7,
    CTMB_MSG_ERROR = 8,
    CTMB_MSG_FEATURE_SET = 9,
    CTMB_MSG_ENUM = 10,          /* forwarded composite USB enumeration (puck) */
    CTMB_MSG_ISO_AUDIO = 11,     /* raw PCM audio: CTM-USBIP -> aurora-tv for wired ISO passthrough */
    CTMB_MSG_MIC_AUDIO = 12,     /* raw PCM audio: aurora-tv -> CTM-USBIP, the controller microphone */

    /* aurora-tv -> CTM-USBIP: "keep the audio block in your outgoing reports
     * for this many milliseconds". Payload is a little-endian uint16 of ms.
     *
     * WHY IT IS NEEDED. On Bluetooth the controller's speaker rides inside the
     * output report, and the host only emits those reports while it has real
     * audio to send. A controller that has just been bridged -- or is about to
     * be released -- has nothing playing, so no report is emitted at all and a
     * confirmation tone from this side has nowhere to go.
     *
     * The hold is not a window to hit. Reports arrive, the tone overwrites
     * them, and it ends when the frames run out; the only requirement is that
     * the hold outlasts the tone. So neither end has to agree on a duration.
     *
     * An older listener ignores unknown message types silently, so the worst
     * case against one is a tone that does not sound. */
    CTMB_MSG_AUDIO_HOLD = 13
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t flags;
    uint32_t sequence;
    uint64_t timestamp_us;
    uint32_t request_id;
    uint32_t payload_len;
} ctmb_header_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version;
    uint16_t bus;
    uint16_t input_report_len;
    uint16_t output_report_len;
    uint16_t feature_report_len;
    uint16_t flags;
    char path[64];
    char serial[64];
    char product[64];
    char manufacturer[64];
} ctmb_device_caps_t;

typedef struct {
    uint32_t report_descriptor_len;
    uint8_t reserved[28];
} ctmb_hid_descriptor_info_t;

/* ⭐ latency_ms: the DualSense's Bluetooth AUDIO BUFFER, owned by the host.
 *
 * Written into bytes 3..7 of block 0x91 of the 398-byte 0x36 audio output
 * report. Measured on hardware: HIGHER is smoother, LOWER is choppier. It is a
 * buffer, and it costs delay for stability.
 *
 * ⛔ CTMB_LATENCY_UNSET (0xFFFF) MEANS THE HOST HAS NOT SPOKEN, and the TV keeps
 * whatever it had. ⚠️ 0 is a REAL VALUE, deliberately reachable -- the point of
 * host control is to find where the audio stops being recoverable, and a
 * sentinel of 0 would make the interesting end of the range unreachable.
 *
 * ⭐ Applied LIVE. This message is accepted at any point in a session, not only
 * at the handshake, and the patch hook reads the live settings on every
 * outbound report -- so a change takes effect on the next report, with no
 * re-bridge. That matters because the symptom is choppy audio during a game.
 *
 * ⓘ Taken from the reserved block, so the struct size is unchanged and an
 * older build on either side simply ignores it. */
#define CTMB_LATENCY_UNSET 0xFFFFu

/* ⭐ The same idea one byte wide, for the audio settings below.
 *
 * ⛔⛔ AND THE ZERO CASE IS THE TRAP. An OLDER client sends a zeroed reserved
 * block, and zero is a legal percentage -- SILENT. **A TV that trusted a zero
 * would mute every controller bridged from an unpatched host.**
 * ➡️ So the TV treats 0xFF *and* an all-zero triple as "not set". ⓘ That costs
 * the ability to set all three to zero at once from the host, which is not a
 * configuration anyone wants: it is silent audio routed nowhere. */
#define CTMB_AUDIO_UNSET 0xFFu

typedef struct {
    uint32_t bt_pace_us;
    uint16_t input_report_len;
    uint16_t output_report_len;
    uint16_t feature_report_len;
    uint8_t paced_report_count;
    uint8_t paced_report_ids[16];
    uint16_t latency_ms;          /* CTMB_LATENCY_UNSET = leave the TV's value */

    /* ⭐⭐ THE HOST'S PER-CONTROLLER AUDIO SETTINGS. Added 2026-08-25, T-130.
     *
     * ⛔ THE FAULT THEY FIX: on Windows, `ds5_output_overrides.inl` patches the
     * host's outbound reports -- speaker volume, headset volume, audio routing,
     * rumble gain -- and every one of those overrides begins
     * `if (data[0] != 0x02) return;`. **0x02 is the WIRED report id.** Over
     * Bluetooth the host sends 0x36, so six settings silently did nothing.
     * ⓘ Confirmed by ear: `speaker_volume = 0` left the controller at full.
     *
     * ⚠️ WHY NOT JUST TEACH WINDOWS THE 0x36 LAYOUT: a Bluetooth output report
     * is SIGNED, and the TV re-signs only when IT patched something
     * (`if (patched) ctm_bt_sign_output(...)`). **A report Windows edited but
     * the TV did not would arrive with a stale signature and be dropped by the
     * controller.** ➡️ The TV already walks this block, already knows the
     * offsets, and already re-signs. It only ever lacked the host's numbers.
     *
     * ⭐ SO THEY TRAVEL, exactly as latency_ms does -- same reserved block, same
     * unset convention, same "write into the live settings so the next report
     * carries it" application. ⓘ **That is why audio_latency_ms has always
     * worked over Bluetooth while nothing else did: somebody solved this once,
     * for one setting, and never generalised it.**
     *
     * ⓘ Volumes are PERCENT, matching tv_bridge_worker_settings_t, not the raw
     * byte -- the TV owns the curve and it is not linear. */
    uint8_t speaker_volume_pct;   /* CTMB_AUDIO_UNSET = leave the TV's value */
    uint8_t headset_volume_pct;   /* CTMB_AUDIO_UNSET = leave the TV's value */
    uint8_t audio_mode;           /* CTMB_AUDIO_UNSET = leave the TV's value */
    uint8_t reserved[26];
} ctmb_host_config_t;

/* CTMB_MSG_ENUM payload (puck composite): the device's OWN enumeration, read
 * from sysfs on the TV and forwarded verbatim. Windows replays it (no parsing
 * on the TV). Layout:
 *   [ctmb_enum_info_t]
 *   [descriptors blob: descriptors_len bytes]   (device + config + ifaces + eps)
 *   iface_count x ( [ctmb_enum_iface_t] [report_desc: report_desc_len bytes] )
 * full_speed=1 tells the host to present the virtual device as full-speed so the
 * 64-byte CDC bulk endpoints are legal (the device really is full-speed). */
typedef struct {
    uint16_t descriptors_len;
    uint8_t  iface_count;
    uint8_t  full_speed;
    uint8_t  reserved[28];
} ctmb_enum_info_t;

typedef struct {
    uint8_t  interface_number;
    uint8_t  iface_class;
    uint16_t report_desc_len;
} ctmb_enum_iface_t;
#pragma pack(pop)

#endif
