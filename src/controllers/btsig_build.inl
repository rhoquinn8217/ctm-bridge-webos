/* The Bluetooth signal's report, and its builder.
 *
 * ⓘ In a file of its own (2026-10-23) so the test includes THE builder and
 * not a copy of its first 50 bytes: tests/test_btsig_build.c. Included by
 * ctm_bt_signal.inl. It needs ctm_bt_signal_data.inl (BTSIG_FRAME_BYTES and
 * g_btsig_silence) and bt_sign.inl (bt_sign_output) first. */

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The report, as parsed from one the host actually sent.
 *
 *    0        report id
 *    1        sequence, in the high nibble
 *    2..66    block 0x90 len 63   state: flags, volumes, audio control, light
 *   67..75    block 0x91 len  7   timing: latency, and the audio sequence
 *   76..277   block 0x95 len 200  audio: exactly one Opus frame
 *  278..343   block 0x92 len  64  haptics: raw signed samples
 *  344..393   zeros: an end marker, then padding to the fixed length
 *  394..397   CRC32, little-endian
 */
#define BTSIG_REPORT_ID    0x36
#define BTSIG_REPORT_LEN   398

#define BTSIG_STATE_AT     2
#define BTSIG_STATE_LEN    63
#define BTSIG_TIMING_AT    67
#define BTSIG_TIMING_LEN   7
#define BTSIG_AUDIO_AT     76
#define BTSIG_AUDIO_LEN    200
#define BTSIG_HAPTIC_AT    278
#define BTSIG_HAPTIC_LEN   64
#define BTSIG_CRC_AT       394

/* The state block's payload is the WIRED report from its byte 1 onward, so
 * everything in ds5-output-report-reference.md applies with a shift. Confirmed
 * twice: the captured report held 00 ff ff at 48-50, the teal the host was
 * painting, and writing ff 00 00 there turned the light red. */
#define BTSIG_S_FLAGS1     (BTSIG_STATE_AT + 2 + 0)   /* report byte 4  */
#define BTSIG_S_FLAGS2     (BTSIG_STATE_AT + 2 + 1)   /* report byte 5  */
#define BTSIG_S_SPK_VOL    (BTSIG_STATE_AT + 2 + 5)   /* report byte 9  */
#define BTSIG_S_AUDIO_CTL  (BTSIG_STATE_AT + 2 + 7)   /* report byte 11 */
#define BTSIG_S_LED_R      (BTSIG_STATE_AT + 2 + 44)  /* report byte 48 */

/* CLAIM ONLY WHAT WE SET. The report carries the whole state block, so writing
 * one naively fights SDL for the lightbar -- the collision the lightbar work
 * already lost. Panel 1 claims the two audio fields; panel 2 claims the LED
 * only while we are using it, and nothing else ever. */
#define BTSIG_F1_AUDIO     0xa0   /* speaker volume + audio control */
/* ⭐ Present in EVERY report the host sends, and absent from ours until
 * 2026-08-19. What it enables is not documented in anything we have -- it is
 * copied because it is what a working report contains. Captured off the relay
 * while YouTube played through a bridged controller:
 *
 *   host:  90 3f 00 14 00 00 00 00 00 00 00 00
 *   ours:  90 3f a0 00 00 00 00 64 00 3c 00 00
 */
#define BTSIG_F2_HOST      0x14
#define BTSIG_F2_LED       0x04   /* AllowLedColor, and nothing else */

#define BTSIG_SPK_VOL      0x64   /* full scale; below ~0x3c is inaudible */
/* ⭐ Speaker route with echo cancel, and NOISE CANCEL OFF (0x34, was 0x3c).
 *
 * ⓘ This plays on every bridge, so leaving 0x3c here would re-enable the
 * beamforming the rest of the change turns off -- and it would do it at exactly
 * the moment a controller is handed over. ⚠️ See the note in
 * controller_common.c for why bit 3 is out. */
/* ⛔ BACK TO 0x3c 2026-08-24 -- Bluetooth needs noise cancel for its speaker.
 * ⓘ See the note beside DS5_BT_AUDIO_ECHO_NOISE_CANCEL in controller_ds5.c.
 * ⚠️ This plays on every bridge, so leaving it at 0x34 would attenuate the
 * confirmation tone even with the report patcher corrected. */
#define BTSIG_AUDIO_CTL    0x3c

/* The timing block's first byte and its latency values, taken verbatim from a
 * report the controller was accepting. ⚠️ The latency is the user's to set --
 * raising it audibly reduced choppiness -- and belongs on the host side with
 * the rest of the device config. Frozen here until that exists. */
#define BTSIG_T_LEAD       0xfe
#define BTSIG_T_LATENCY    0x60
#define BTSIG_T_AUDIO_SEQ  (BTSIG_TIMING_AT + 8)   /* report byte 75 */

/* Haptics are RAW SIGNED SAMPLES -- no codec, which is why this half was
 * always reachable. 3200 Hz is the host's own hapticOutputRate_, and its
 * buffer is named haptic_pcm_3k2; 64 bytes is 32 stereo frames, ten
 * milliseconds at that rate. */
#define BTSIG_HAPTIC_HZ    3200
#define BTSIG_PULSE_HZ     60
#define BTSIG_PULSE_LEVEL  90

#define BTSIG_PACE_US      10000  /* one report per frame of audio */

typedef struct {
    const uint8_t *audio;      /* one Opus frame, or NULL for the silence one */
    int            seq;        /* report number; drives BOTH sequence fields */
    int            haptics;    /* generate a felt pulse into the haptic block */
    int            haptic_n;   /* running sample count, so the wave is smooth */
    int            claim_led;  /* set the lightbar, or leave it to SDL */
    /* ⭐ Apply the speaker settings, or just carry audio. See btsig_build. */
    int            configure;
    uint8_t        r, g, b;
} btsig_frame_t;

static void btsig_build(uint8_t *out, const btsig_frame_t *f)
{
    memset(out, 0, BTSIG_REPORT_LEN);
    out[0] = BTSIG_REPORT_ID;
    out[1] = (uint8_t)((f->seq << 4) & 0xf0);

    out[BTSIG_STATE_AT]     = 0x90;
    out[BTSIG_STATE_AT + 1] = BTSIG_STATE_LEN;
    /* ⛔⛔ CONFIGURE ONCE, THEN JUST CARRY AUDIO.
     *
     * BTSIG_F1_AUDIO (0xa0) means "apply the speaker volume and audio control
     * in this report". It was set on EVERY report, so the controller was told
     * to reconfigure its audio subsystem a hundred times a second -- and that
     * is what stalled the writes and dropped the link. Measured 2026-08-19:
     * 122 reports took 5243ms for 1220ms of audio, then the controller powered
     * itself off.
     *
     * ⭐ A real report from the host, captured the same day:
     *
     *     90 3f 00 14 00 00 00 00 00 00 00 00
     *          ^^ ^^                ^^    ^^
     *          f1 f2               vol   ctl
     *
     * ⚠️ FLAGS1 IS ZERO and the volume and control bytes are zero WITH it --
     * they are ignored unless the flag asks for them. The host configures the
     * speaker once when the session opens and never again; every report after
     * that is pure carriage.
     *
     * ⛔⛔ FLAGS2 IS NOT COPIED FROM THE HOST, AND THE ATTEMPT IS A WARNING.
     *
     * The host sends 0x14 and it was copied here on the reasoning that the
     * controller must need it. ⚠️ It IS documented -- in the Linux kernel's own
     * DualSense driver, and one search found it:
     *
     *     0x01 mic mute LED     0x08 release LEDs
     *     0x02 POWER SAVE       0x10 player indicator LEDs
     *     0x04 lightbar         0x80 audio control 2
     *
     * So 0x14 asks the controller to apply the PLAYER LEDS and the LIGHTBAR --
     * with the colour bytes beside them at zero, on all 122 frames of a tone.
     * That was the flicker.
     *
     * ⛔ AND IT CAME BACK ONCE. The fix worked at build 184 and was never
     * committed: a sandbox reset discarded it and later deliveries carried the
     * old value back, which cost most of an afternoon chasing handovers and
     * relays. There is a test for it now. */
    out[BTSIG_S_FLAGS1]     = f->configure ? BTSIG_F1_AUDIO : 0x00;
    /* ⛔⛔ CLAIM NOTHING WE DO NOT SET. Removed 2026-08-19, then LOST, then
     * removed again -- see the note on BTSIG_F2_HOST above.
     *
     * ⓘ 0x14 is "apply the player LEDs" and "apply the lightbar". On a pattern
     * that sets the colour bytes it is merely redundant; on one that does not,
     * it paints them black a hundred times a second. */
    out[BTSIG_S_FLAGS2]     = f->claim_led ? BTSIG_F2_LED : 0x00;
    if (f->configure) {
        out[BTSIG_S_SPK_VOL]   = BTSIG_SPK_VOL;
        out[BTSIG_S_AUDIO_CTL] = BTSIG_AUDIO_CTL;
    }
    if (f->claim_led) {
        out[BTSIG_S_LED_R + 0] = f->r;
        out[BTSIG_S_LED_R + 1] = f->g;
        out[BTSIG_S_LED_R + 2] = f->b;
    }

    out[BTSIG_TIMING_AT]     = 0x91;
    out[BTSIG_TIMING_AT + 1] = BTSIG_TIMING_LEN;
    out[BTSIG_TIMING_AT + 2] = BTSIG_T_LEAD;
    for (int i = 3; i <= 7; ++i) out[BTSIG_TIMING_AT + i] = BTSIG_T_LATENCY;

    /* ⛔ THE FIELD THAT DEFEATED FOUR ATTEMPTS. The map says
     * audio_sequence++, and every early attempt sent zero -- so every frame
     * claimed to be the same one and the controller discarded them all, while
     * still obeying the lightbar in the same report. That asymmetry was the
     * clue, and it was visible for three rounds before anyone read it. */
    out[BTSIG_T_AUDIO_SEQ] = (uint8_t)(f->seq & 0xff);

    out[BTSIG_AUDIO_AT]     = 0x95;
    out[BTSIG_AUDIO_AT + 1] = BTSIG_AUDIO_LEN;
    memcpy(&out[BTSIG_AUDIO_AT + 2],
           f->audio ? f->audio : g_btsig_silence, BTSIG_FRAME_BYTES);

    out[BTSIG_HAPTIC_AT]     = 0x92;
    out[BTSIG_HAPTIC_AT + 1] = BTSIG_HAPTIC_LEN;
    if (f->haptics) {
        for (int i = 0; i < 32; ++i) {
            double t = (double)(f->haptic_n + i) / (double)BTSIG_HAPTIC_HZ;
            int v = (int)(sin(2.0 * M_PI * BTSIG_PULSE_HZ * t) * BTSIG_PULSE_LEVEL);
            out[BTSIG_HAPTIC_AT + 2 + i * 2]     = (uint8_t)(int8_t)v;
            out[BTSIG_HAPTIC_AT + 2 + i * 2 + 1] = (uint8_t)(int8_t)v;
        }
    }

    /* CRC32 over 0xa2 followed by the report's first 394 bytes, little-endian
     * at the end. Reproduced a captured report's checksum exactly. The one
     * copy is bt_sign.inl's (code review, 2026-10-05). */
    bt_sign_output(out, BTSIG_CRC_AT + 4);
}
