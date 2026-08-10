/* --- card matching: the measurement, not the feature ---------------------
 *
 * THE PROBLEM. Two identical controllers each bring a speaker and a
 * microphone, and the TV files those as numbered sound cards. Nothing on the
 * TV says which card belongs to which controller: the card scan takes the
 * first one whose name matches and that it can open, so cards are handed out
 * in the order controllers are BRIDGED while they were created in the order
 * controllers were CONNECTED. Match those orders and everything works;
 * reverse them and every controller gets someone else's speaker and
 * microphone. Reproduced deliberately on 2026-08-08.
 *
 * WHY THE OBVIOUS FIXES ARE ALL DEAD. The card knows its USB bus and device
 * number; the controller can be asked for its own USB port path. Those are
 * different notations for the same socket, and the only thing that converts
 * between them is the part of the filesystem these TVs do not expose. The
 * card's own name embeds the port path -- but the kernel truncates that
 * string at 80 characters and the DualSense's product name is long enough to
 * push the port digits off the end, so the information is destroyed when the
 * card is created rather than merely hidden from us.
 *
 * THE IDEA BEING TESTED HERE (rhoquinn8217, 2026-08-09). Ask the controller to mute
 * its own microphone, then look at the cards. A muted microphone reads
 * EXACTLY ZERO -- proven on hardware 2026-08-04, peaks flat at zero while
 * talking. A live one in a quiet room reads 17 to 44. So the card that falls
 * silent belongs to the controller we just muted.
 *
 * It works where everything else failed because it CROSSES THE GAP: we
 * command one side and observe the other, which is what creates the link.
 * Closing our own capture would not -- that changes what we listen to, not
 * what any controller does, so it tells us nothing about whose card it is.
 *
 * WHAT THIS FILE DOES, AND ONLY THIS. One controller. Read its level, mute
 * it, read again, unmute. Log both. NOTHING ACTS ON THE RESULT.
 *
 * The whole design rests on "live is never zero, muted is always zero", and
 * that has never been measured in a genuinely silent room. If the live
 * reading can touch zero, the idea fails and no amount of code around it
 * helps. So the assumption gets measured before anything is built on it.
 *
 * SAFETY, because a half-finished probe is worse than no probe:
 *   - the mute is ALWAYS cleared on the way out, including on early exit --
 *     a controller left muted looks like a broken microphone later;
 *   - the mute LIGHT is set with the mute in the same report, so it never
 *     disagrees with the microphone it describes;
 *   - only the controller being plugged in is touched. */

/* From ds5-output-report-reference.md, established 2026-08-04, cross-checked
 * against the Linux hid-playstation driver and dualsensectl, then proven on
 * hardware.
 *
 * THE PERMISSION IS IN FLAG PANEL 2, NOT PANEL 1. Every other setting in this
 * project claims its fields in panel 1, so panel 1 is the reflex -- and
 * claiming mute there does nothing at all. That mistake has already been made
 * once and caught before it reached hardware. */
#define DS5_F1_ALLOW_MUTE_LIGHT   0x01   /* byte 2, bit 0 */
#define DS5_F1_ALLOW_AUDIO_MUTE   0x02   /* byte 2, bit 1 */
#define DS5_IDX_MUTE_LIGHT        9
#define DS5_IDX_MUTES             10
#define DS5_MUTE_LIGHT_ON         0x01
#define DS5_MIC_MUTE              0x10   /* byte 10, bit 4 */

#define MATCH_READ_MS             1500   /* long enough for a level to settle */

/* Set or clear the microphone mute, and the light with it.
 *
 * BYTE 10 IS SHARED -- it also carries the speaker mute, the headphone mute,
 * the haptic mute and four power-save switches, and zero is the everything-on
 * state. So bit 4 is set alone and the rest of the byte left at zero.
 * Writing the whole byte silences the controller's speaker, a feature that
 * works today, in a way nobody would connect to a microphone change. */
static void cardmatch_set_mic_mute(ctm_controller_t *c, int muted)
{
    uint8_t rep[DS5_OUT_REPORT_LEN] = {0};
    rep[0] = DS5_OUT_REPORT_ID;
    rep[DS5_IDX_VALID_FLAG1] = DS5_F1_ALLOW_AUDIO_MUTE | DS5_F1_ALLOW_MUTE_LIGHT;
    rep[DS5_IDX_MUTES]       = muted ? DS5_MIC_MUTE : 0x00;
    rep[DS5_IDX_MUTE_LIGHT]  = muted ? DS5_MUTE_LIGHT_ON : 0x00;
    int rc = hid_write_report(c, rep, sizeof(rep));
    ctl_log(c, "cardmatch: mic mute %s rc=%d", muted ? "ON" : "off", rc);
}

/* Read the loudest sample seen over MATCH_READ_MS. Returns -1 if the card
 * could not be read at all, which is a different answer from "silent" and
 * must not be confused with one. */
static int cardmatch_peak(int fd)
{
    int16_t buf[MIC_CAP_FRAMES * MIC_CAP_CHANNELS];
    int peak = -1;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec) * 1000 +
                  (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms >= MATCH_READ_MS) break;

        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0) continue;

        struct snd_xferi xfer;
        memset(&xfer, 0, sizeof(xfer));
        xfer.buf = buf;
        xfer.frames = MIC_CAP_FRAMES;
        if (ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xfer) < 0) {
            if (errno == EPIPE) {
                ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
                ioctl(fd, SNDRV_PCM_IOCTL_START, NULL);
            }
            continue;
        }
        long got = (long)xfer.result;
        if (got <= 0) continue;
        if (peak < 0) peak = 0;
        for (long i = 0; i < got * MIC_CAP_CHANNELS; ++i) {
            int v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > peak) peak = v;
        }
    }
    return peak;
}

/* The measurement. Runs once per plug-in, before capture starts, so the card
 * is still free. Costs about three seconds and changes nothing. */
static void cardmatch_probe(ctm_controller_t *c)
{
    if (!c || strcmp(ctm_controller_bus(c), "USB") != 0) return;

    int card = -1;
    int fd = open_ds5_alsa_capture(&card, c->dev.path);
    if (fd < 0) {
        ctl_log(c, "cardmatch: no capture card to probe");
        return;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_START, NULL) < 0) {
        ctl_log(c, "cardmatch: START failed errno=%d (first read may start it)", errno);
    }

    /* Same reasoning for the live reading: whatever accumulated between
     * opening the card and reaching this line is stale by the time it is
     * measured. Start both readings from a known-empty buffer so the two are
     * comparable. */
    ioctl(fd, SNDRV_PCM_IOCTL_DROP, NULL);
    ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
    ioctl(fd, SNDRV_PCM_IOCTL_START, NULL);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int live = cardmatch_peak(fd);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long live_ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;

    cardmatch_set_mic_mute(c, 1);

    /* THROW AWAY WHAT IS ALREADY IN THE BUFFER BEFORE MEASURING.
     *
     * The capture buffer holds about a second of audio -- 49152 frames at
     * 48 kHz. Reading straight after the mute drains audio the microphone
     * captured BEFORE it was muted, so the "muted" figure is really a
     * measure of how loud the room was a moment ago.
     *
     * Measured 2026-08-09 on C1: a quiet run reported muted_peak=0 and looked
     * like a pass, while a run with talking reported 212 and a third 54 --
     * the number tracking the noise that preceded the mute, not any sound
     * after it. The quiet result was luck.
     *
     * DROP discards everything queued; PREPARE and START begin again from the
     * microphone as it is now. The short sleep first gives the controller
     * time to act on the report, since the mute is a request over HID rather
     * than something that takes effect the instant we return. */
    struct timespec settle = {0, 200 * 1000000};   /* 200 ms */
    nanosleep(&settle, NULL);
    ioctl(fd, SNDRV_PCM_IOCTL_DROP, NULL);
    ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
    ioctl(fd, SNDRV_PCM_IOCTL_START, NULL);

    int muted = cardmatch_peak(fd);
    /* Cleared here and not at the end: everything below this point is
     * logging, and an early return past an un-mute is how a controller ends
     * up muted for good. */
    cardmatch_set_mic_mute(c, 0);

    /* THE NUMBER THAT DECIDES THE WHOLE APPROACH is `muted`. It must be
     * exactly 0. And `live` must never be 0, or a silent room is
     * indistinguishable from a muted microphone. */
    ctl_log(c, "cardmatch: card=%d node=%s live_peak=%d muted_peak=%d read_ms=%ld",
            card, c->dev.path, live, muted, live_ms);
    alsa_log("[cardmatch]", "card=%d node=%s live_peak=%d muted_peak=%d read_ms=%ld",
             card, c->dev.path, live, muted, live_ms);

    close(fd);
}
