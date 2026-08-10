/* --- connection feedback -----------------------------------------------
 *
 * A short tone and a rumble pulse when a controller is bridged, so the
 * controller in your hand says what happened instead of the log saying it
 * afterwards. With three plugged in, the one that beeps is the one that just
 * connected.
 *
 * WHY HERE AND NOT THE LIGHTBAR: nothing on the PC side claims the speaker,
 * the motors or the adaptive triggers -- the user drives them, through a game
 * -- so a signal here BLENDS with whatever else is playing. The lightbar is
 * the opposite: the player's own config, the game, Steam and Windows all
 * outrank us, and every moment we might want to signal is a moment one of
 * them may be driving it.
 *
 * ITS OWN FILE because this is a feature of ours rather than a gap in the
 * wired support, so an upstream change to the Bluetooth path cannot collide
 * with it.
 *
 * The channel layout is not ours to choose: 0/1 are the speaker, 2/3 the
 * haptics. ONE WRITE DRIVES BOTH, which is why the tone and the pulse here
 * are a single buffer rather than two mechanisms.
 *
 * PLAYED AFTER THE CARD PROBE, NOT BEFORE. The probe moves the speaker onto
 * the controller's own card; a tone played before it comes out of whichever
 * controller the scan happened to pick. A confirmation from the wrong
 * controller is worse than none, because it would be believed.
 *
 * WIRED ONLY -- Bluetooth carries audio inside the report patching, a
 * different path entirely.
 *
 * NO FAILURE SIGNAL YET. A refused plug has no session and no open audio
 * device, so it would have to find a card by scanning, which is the guess
 * this project has just finished removing. It can be done properly with the
 * same mute trick the probe uses; it is not done here. */

#define FEEDBACK_RATE         48000
#define FEEDBACK_CHANNELS     4
#define FEEDBACK_TONE_HZ      880    /* a clear beep, above game rumble */
#define FEEDBACK_RUMBLE_HZ    60     /* low enough to be felt, not heard */
#define FEEDBACK_MS           140
#define FEEDBACK_TONE_LEVEL   9000   /* ~28% of full scale: audible, not harsh */
#define FEEDBACK_RUMBLE_LEVEL 14000  /* the motors need more than the speaker */

/* One beep and one pulse, handed to the same write path the host's audio
 * uses, so it inherits the retry and self-heal behaviour rather than
 * repeating it.
 *
 * Both envelopes fade in and out. A square-edged start makes the speaker
 * click and the motors knock, which reads as a fault rather than a
 * confirmation. */
static void feedback_play_connected(ctm_controller_t *c)
{
    if (!c || c->alsa_fd < 0) {
        ctl_log(c, "feedback: no audio device, nothing played");
        return;
    }

    const int frames = (FEEDBACK_RATE * FEEDBACK_MS) / 1000;
    const size_t bytes = (size_t)frames * FEEDBACK_CHANNELS * sizeof(int16_t);
    int16_t *buf = (int16_t *)calloc(1, bytes);
    if (!buf) {
        ctl_log(c, "feedback: allocation failed, nothing played");
        return;
    }

    for (int i = 0; i < frames; ++i) {
        /* Triangular envelope: peak in the middle, silent at both ends. */
        const int half = frames / 2;
        const int rise = (i < half) ? i : (frames - i);
        const double env = (half > 0) ? ((double)rise / (double)half) : 0.0;

        const double t = (double)i / (double)FEEDBACK_RATE;
        const double tone = sin(2.0 * M_PI * FEEDBACK_TONE_HZ * t) * env;
        const double bump = sin(2.0 * M_PI * FEEDBACK_RUMBLE_HZ * t) * env;

        const int16_t s = (int16_t)(tone * FEEDBACK_TONE_LEVEL);
        const int16_t r = (int16_t)(bump * FEEDBACK_RUMBLE_LEVEL);

        buf[i * FEEDBACK_CHANNELS + 0] = s;   /* speaker left  */
        buf[i * FEEDBACK_CHANNELS + 1] = s;   /* speaker right */
        buf[i * FEEDBACK_CHANNELS + 2] = r;   /* haptic left   */
        buf[i * FEEDBACK_CHANNELS + 3] = r;   /* haptic right  */
    }

    write_iso_audio(c, (const uint8_t *)buf, (uint32_t)bytes);
    free(buf);
    ctl_log(c, "feedback: connected - one tone and pulse on card=%d",
            c->matched_card);
}
