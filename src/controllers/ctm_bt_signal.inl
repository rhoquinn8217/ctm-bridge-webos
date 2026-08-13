/* --- confirmation signals for a Bluetooth controller -------------------------
 *
 * FORK-ONLY. Nothing upstream calls into this; one call site in
 * ctm_feedback.inl reaches it, and deleting this file plus that call removes
 * the feature entirely.
 *
 * WHAT IT DOES. Builds and sends the controller's own output reports, so a
 * confirmation is produced by the TV alone -- no host, no listener, no
 * network. Light, a felt pulse and a tone travel together in one report.
 *
 * WHY IT EXISTS. The first Bluetooth tone rode on reports the HOST was
 * already sending, and the TV filled in the audio as they passed. That works
 * on a local network and fails over the internet: measured 2026-08-12 on C3
 * and C5, the tone arrived split into two beeps or not at all. This route has
 * no such dependency -- it is as local as the lightbar.
 *
 * ⛔ BLUETOOTH ONLY. These are report 0x36 at 398 bytes. A wired DualSense
 * expects 0x02 at 48, and every byte would land in the wrong field -- done by
 * accident during development, and the controller "went crazy". The caller
 * gates on there being no ALSA device, which is the Bluetooth condition.
 *
 * ⚠️ A BAD REPORT DOES NOT MERELY FAIL. Enough malformed ones and the
 * controller stops acting on anything at all -- no light, no sound -- until it
 * is power cycled. That was observed repeatedly while this was being worked
 * out, and it made three rounds of results meaningless because a wedged
 * controller is indistinguishable from wrong content. Treat every byte here as
 * load-bearing.
 *
 * See bt-tv-side-reports.md for the byte map and how each part was proved. */

#include "ctm_bt_signal_data.inl"

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
#define BTSIG_F2_LED       0x04   /* AllowLedColor, and nothing else */

#define BTSIG_SPK_VOL      0x64   /* full scale; below ~0x3c is inaudible */
#define BTSIG_AUDIO_CTL    0x3c   /* speaker route WITH echo cancel */

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

/* CRC32 over 0xa2 followed by the report's first 394 bytes, little-endian at
 * the end. Reproduced a captured report's checksum exactly. */
static uint32_t btsig_crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int built = 0;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = 1;
    }
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

typedef struct {
    const uint8_t *audio;      /* one Opus frame, or NULL for the silence one */
    int            seq;        /* report number; drives BOTH sequence fields */
    int            haptics;    /* generate a felt pulse into the haptic block */
    int            haptic_n;   /* running sample count, so the wave is smooth */
    int            claim_led;  /* set the lightbar, or leave it to SDL */
    uint8_t        r, g, b;
} btsig_frame_t;

static void btsig_build(uint8_t *out, const btsig_frame_t *f)
{
    memset(out, 0, BTSIG_REPORT_LEN);
    out[0] = BTSIG_REPORT_ID;
    out[1] = (uint8_t)((f->seq << 4) & 0xf0);

    out[BTSIG_STATE_AT]     = 0x90;
    out[BTSIG_STATE_AT + 1] = BTSIG_STATE_LEN;
    out[BTSIG_S_FLAGS1]     = BTSIG_F1_AUDIO;
    out[BTSIG_S_FLAGS2]     = f->claim_led ? BTSIG_F2_LED : 0x00;
    out[BTSIG_S_SPK_VOL]    = BTSIG_SPK_VOL;
    out[BTSIG_S_AUDIO_CTL]  = BTSIG_AUDIO_CTL;
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

    uint8_t seeded[1 + BTSIG_CRC_AT];
    seeded[0] = 0xa2;
    memcpy(&seeded[1], out, BTSIG_CRC_AT);
    uint32_t crc = btsig_crc32(seeded, sizeof(seeded));
    out[BTSIG_CRC_AT + 0] = (uint8_t)(crc & 0xff);
    out[BTSIG_CRC_AT + 1] = (uint8_t)((crc >> 8) & 0xff);
    out[BTSIG_CRC_AT + 2] = (uint8_t)((crc >> 16) & 0xff);
    out[BTSIG_CRC_AT + 3] = (uint8_t)((crc >> 24) & 0xff);
}

/* The two signals, as the light shows them.
 *
 * Magenta going to the host, yellow coming back -- the vocabulary the app's
 * own patterns already used, kept so nothing has to be relearned.
 *
 * ⭐ AND THE PULSE CAN ACTUALLY PULSE NOW. The app's version froze at its
 * brightest and stayed there, because it animated on the UI thread and lost
 * the competition for it. This runs on its own thread at one report every ten
 * milliseconds, so a ramp is smooth and repeating it costs nothing. */
typedef enum {
    BTSIG_HANDING_OVER,   /* magenta, pulsing */
    BTSIG_HANDED_BACK     /* yellow, three flashes */
} btsig_pattern_t;

/* Brightness for report `i` of `n`, as the chosen pattern would have it. */
static int btsig_level(btsig_pattern_t p, int i, int n)
{
    if (n <= 0) return 0;
    if (p == BTSIG_HANDED_BACK) {
        /* Three flashes: on for a third of each slot, off for the rest, so
         * they read as three distinct events rather than a flicker. */
        int slot = (n + 2) / 3;
        int within = i % slot;
        return (within < (slot / 2)) ? 255 : 0;
    }
    /* Handing over: SOLID, not pulsing.
     *
     * The app's pre-plug pulse has already swelled the light up to full by the
     * time this runs, so pulsing again animates the same event twice. Holding
     * it also matches wired exactly, where the light and the audio go to
     * different devices and nothing can animate the light during the tone. */
    (void)i;
    return 255;
}

/* Play a confirmation on a Bluetooth controller: light, a felt pulse and a
 * tone, all in one stream of reports it builds itself.
 *
 * Returns 0 if it ran. Takes about a second and a half of wall time, so the
 * caller decides whether it can afford to wait -- see the note in
 * ctm_feedback.inl about never doing this on the session thread. */
static int btsig_play(ctm_controller_t *c, btsig_pattern_t pattern)
{
    if (!c || c->hid_fd < 0) return -1;

    uint8_t rep[BTSIG_REPORT_LEN];
    btsig_frame_t f;
    memset(&f, 0, sizeof(f));
    int seq = 0, hn = 0, sent = 0, failed = 0;

    /* Prime: the decoder needs a stream to start. A burst from cold produces
     * nothing at all, which is how three earlier attempts read as failures. */
    for (int i = 0; i < BTSIG_PRIME_FRAMES; ++i) {
        f.audio = NULL; f.seq = seq++; f.haptics = 0; f.claim_led = 0;
        btsig_build(rep, &f);
        if (write(c->hid_fd, rep, sizeof(rep)) == (ssize_t)sizeof(rep)) ++sent;
        else ++failed;
        usleep(BTSIG_PACE_US);
    }

    /* The signal itself: colour ramping up, a pulse to feel, a tone to hear. */
    const uint8_t R = 0xff;
    const uint8_t G = (pattern == BTSIG_HANDED_BACK) ? 0xff : 0x00;
    const uint8_t B = (pattern == BTSIG_HANDED_BACK) ? 0x00 : 0xff;

    /* The light runs longer than the tone, so the pattern is legible. */
    const int LIT = BTSIG_TONE_FRAMES + 40;

    for (int i = 0; i < BTSIG_TONE_FRAMES; ++i) {
        int lvl = btsig_level(pattern, i, LIT);
        f.audio = g_btsig_tone[i]; f.seq = seq++;
        f.haptics = 1; f.haptic_n = hn; hn += 32;
        f.claim_led = 1;
        f.r = (uint8_t)((R * lvl) / 255);
        f.g = (uint8_t)((G * lvl) / 255);
        f.b = (uint8_t)((B * lvl) / 255);
        btsig_build(rep, &f);
        if (write(c->hid_fd, rep, sizeof(rep)) == (ssize_t)sizeof(rep)) ++sent;
        else ++failed;
        usleep(BTSIG_PACE_US);
    }

    /* Let the light finish its pattern after the tone has stopped, and keep
     * the stream alive so it does not end mid-frame. */
    for (int i = BTSIG_TONE_FRAMES; i < LIT; ++i) {
        int lvl = btsig_level(pattern, i, LIT);
        f.audio = NULL; f.seq = seq++; f.haptics = 0; f.claim_led = 1;
        f.r = (uint8_t)((R * lvl) / 255);
        f.g = (uint8_t)((G * lvl) / 255);
        f.b = (uint8_t)((B * lvl) / 255);
        btsig_build(rep, &f);
        if (write(c->hid_fd, rep, sizeof(rep)) == (ssize_t)sizeof(rep)) ++sent;
        else ++failed;
        usleep(BTSIG_PACE_US);
    }

    /* Hand the light back. Claiming it and never releasing would leave SDL
     * unable to paint, which is the mirror of the fight this avoids. */
    for (int i = 0; i < 10; ++i) {
        f.audio = NULL; f.seq = seq++; f.haptics = 0; f.claim_led = 0;
        btsig_build(rep, &f);
        if (write(c->hid_fd, rep, sizeof(rep)) == (ssize_t)sizeof(rep)) ++sent;
        else ++failed;
        usleep(BTSIG_PACE_US);
    }

    ctl_log(c, "btsig: %d reports sent, %d failed", sent, failed);
    return failed ? -1 : 0;
}
