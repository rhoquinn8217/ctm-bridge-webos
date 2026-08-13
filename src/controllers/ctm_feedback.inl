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
 * THE FAILURE SIGNAL IS NOT HERE, AND CANNOT BE. A refused plug has no
 * session and no open audio device, so nothing on this side can make a sound.
 * It lives in the app, which still holds the controller through SDL, and is
 * rumble only for that reason. */

#define FEEDBACK_RATE         48000
#define FEEDBACK_CHANNELS     4
#define FEEDBACK_TONE_HZ      880    /* a clear beep, above game rumble */
#define FEEDBACK_RUMBLE_HZ    60     /* low enough to be felt, not heard */
#define FEEDBACK_MS           140
#define FEEDBACK_GAP_MS       90     /* silence between beeps, so two read as two */
/* How long to ask the host to keep the audio stream alive. Comfortably longer
 * than the tone -- 14 Opus frames at Bluetooth pacing take roughly 400 ms to
 * go out, and overshooting costs only a few silent reports. */
#define FEEDBACK_HOLD_MS      1500
/* Longest to hold up a teardown waiting for the tone to leave.
 *
 * MEASURED, not estimated. 800 ms was tried first on the assumption that 14
 * frames at Bluetooth's ~30 ms pacing would take about 400 ms. It does not:
 * the connect tone timed out at 800 ms and then finished shortly after, so
 * the frames leave at roughly 60 ms each -- about 840 ms for the set, just
 * past the old cutoff. The connect survived it because the session carries on
 * afterwards; the unplug did not, because it tears down the moment the wait
 * gives up.
 *
 * Still bounded, so a stalled link cannot hold an unplug open indefinitely,
 * and still shorter than the hold the host was asked for. */
#define FEEDBACK_TONE_WAIT_MS 1200
#define FEEDBACK_TONE_LEVEL   9000   /* ~28% of full scale: audible, not harsh */
#define FEEDBACK_RUMBLE_LEVEL 14000  /* the motors need more than the speaker */

/* Build and play `beeps` beep-and-pulse pairs on this controller's own audio
 * device. Everything below is a thin wrapper on this.
 *
 * Handed to the same write path the host's audio uses, so it inherits the retry and self-heal behaviour rather than
 * repeating it.
 *
 * Both envelopes fade in and out. A square-edged start makes the speaker
 * click and the motors knock, which reads as a fault rather than a
 * confirmation. */
static void feedback_play(ctm_controller_t *c, int beeps, const char *what, bool wait_out, bool is_unplug)
{
    if (!c) return;
    if (!CTM_SIGNALS_ENABLED) return;
    if (c->alsa_fd < 0) {
        /* NO AUDIO DEVICE, SO THIS IS BLUETOOTH -- and the TV does the whole
         * signal itself: light, a felt pulse and a tone, in reports it builds
         * and writes straight to the controller.
         *
         * THIS REPLACES ASKING THE HOST. The first version filled in audio on
         * reports the host was already sending, and needed the host to keep
         * that stream alive so there was something to fill. It worked on a
         * local network and failed over the internet -- measured on C3 and C5
         * on 2026-08-12, the tone arrived split into two beeps or not at all,
         * because fourteen separate reports had to cross the link on time.
         * Here the link is not involved.
         *
         * ⚠️ IT TAKES ABOUT A SECOND AND A HALF, and it must not run on the
         * session thread: that thread carries the reports, and sleeping on it
         * starves the very thing being waited for. An earlier version made
         * things WORSE by waiting longer, which is how that was found. The
         * caller says which thread it is on. */
        if (!wait_out) {
            ctl_log(c, "feedback: %s -- not signalled, wrong thread to wait on", what);
            return;
        }
        /* Magenta going to the host, yellow coming back -- the same
         * vocabulary the app's own patterns used, so nothing is relearned. */
        btsig_play(c, is_unplug ? BTSIG_HANDED_BACK : BTSIG_HANDING_OVER);
        ctl_log(c, "feedback: %s -- signalled from the TV", what);
        return;
    }

    const int frames_per = (FEEDBACK_RATE * FEEDBACK_MS) / 1000;
    const int gap_frames = (FEEDBACK_RATE * FEEDBACK_GAP_MS) / 1000;
    const int frames = beeps * frames_per + (beeps - 1) * gap_frames;
    const size_t bytes = (size_t)frames * FEEDBACK_CHANNELS * sizeof(int16_t);
    int16_t *buf = (int16_t *)calloc(1, bytes);
    if (!buf) {
        ctl_log(c, "feedback: %s -- allocation failed, nothing played", what);
        return;
    }

    for (int b = 0; b < beeps; ++b) {
        const int base = b * (frames_per + gap_frames);
        for (int i = 0; i < frames_per; ++i) {
            /* Triangular envelope: peak in the middle, silent at both ends. */
            const int half = frames_per / 2;
            const int rise = (i < half) ? i : (frames_per - i);
            const double env = (half > 0) ? ((double)rise / (double)half) : 0.0;

            const double t = (double)i / (double)FEEDBACK_RATE;
            const double tone = sin(2.0 * M_PI * FEEDBACK_TONE_HZ * t) * env;
            const double bump = sin(2.0 * M_PI * FEEDBACK_RUMBLE_HZ * t) * env;

            const int16_t s = (int16_t)(tone * FEEDBACK_TONE_LEVEL);
            const int16_t r = (int16_t)(bump * FEEDBACK_RUMBLE_LEVEL);

            const int f = (base + i) * FEEDBACK_CHANNELS;
            buf[f + 0] = s;   /* speaker left  */
            buf[f + 1] = s;   /* speaker right */
            buf[f + 2] = r;   /* haptic left   */
            buf[f + 3] = r;   /* haptic right  */
        }
    }

    write_iso_audio(c, (const uint8_t *)buf, (uint32_t)bytes);
    free(buf);

    /* WAIT FOR IT TO ACTUALLY COME OUT.
     *
     * Writing only hands the samples to the device; the sound emerges over
     * the next tenth of a second. On an unplug the teardown follows
     * immediately and CLOSES the device, throwing away whatever is still
     * queued -- so the tone was a race, and it lost about half the time.
     * Measured on C1 2026-08-10: two controllers unplugged, both tones
     * written and logged, only one heard.
     *
     * A signal that fires most of the time is worse than none, because it
     * teaches you to distrust it.
     *
     * DRAIN WAS TRIED AND MADE IT WORSE -- build 69 played NOTHING at all,
     * where the racy version had at least played about half the time. This
     * device is opened non-blocking, and on a non-blocking handle a drain
     * request is not reliably "wait for the sound to finish"; it appears to
     * discard instead. Exactly the wrong outcome, achieved deliberately.
     *
     * So: just wait. Unconditional, predictable, and long enough for the tone
     * plus a margin. Nothing here needs to be clever.
     *
     * The drain result is still logged, once, because knowing what this
     * device does with the request is worth more than the guess above. */
    struct timespec ts = {0, (long)(FEEDBACK_MS + 40) * 1000000L};
    nanosleep(&ts, NULL);
    ctl_log(c, "feedback: %s - %d tone(s) and pulse(s) on card=%d, waited %dms",
            what, beeps, c->matched_card, FEEDBACK_MS + 40);
}

/* Bridged. */
/* Runs a connect signal off the session thread. See the note at the call. */
static void *feedback_signal_thread(void *arg)
{
    feedback_play((ctm_controller_t *)arg, 1, "connected", true, false);
    return NULL;
}

static void feedback_play_connected(ctm_controller_t *c)
{
    /* ON THE SESSION THREAD, where the signal must not run: it takes about a
     * second and a half, and that thread carries the controller's reports.
     * Hand it to a short-lived thread instead, so the session keeps moving.
     *
     * Detached deliberately -- nobody waits for a confirmation, and joining
     * would put the wait back where it must not be. */
    pthread_t sig;
    if (pthread_create(&sig, NULL, feedback_signal_thread, c) == 0) {
        pthread_detach(sig);
    } else {
        ctl_log(c, "feedback: connected -- could not start the signal thread");
    }
}

/* Coming back to us.
 *
 * PLAYED BEFORE THE UNPLUG, NOT AFTER, and it has to be: the audio device is
 * torn down by the unplug, so afterwards there is nothing left to play
 * through.
 *
 * WHICH MEANS IT CAN LIE. If the unplug then fails, the controller will have
 * announced something that did not happen. Accepted deliberately for now
 * (rhoquinn8217) -- and it is useful while it lasts, because hearing the tone and
 * then finding the controller still bridged is itself a report of a failed
 * unplug that would otherwise be silent.
 *
 * Generic, one beep, same as connecting -- the two are not yet meant to be
 * told apart. */
static void feedback_play_unplugging(ctm_controller_t *c, ctm_unplug_reason_t why)
{
    /* All three sound the SAME for now, deliberately. The signals have to be
     * heard before a vocabulary is committed to, and today connecting and
     * unplugging are already indistinguishable from each other -- so encoding
     * meaning would be building on nothing.
     *
     * What the reason buys today is the LOG: it names which of the three
     * routes actually fired. They are not interchangeable -- a user asking is
     * not the same event as everything being torn down -- and until now
     * nothing recorded which had happened. */
    const char *what;
    switch (why) {
    case CTM_UNPLUG_SHUTDOWN: what = "unplugging (shutdown)"; break;
    case CTM_UNPLUG_REPLACED: what = "unplugging (replaced)"; break;
    default:                  what = "unplugging (requested)"; break;
    }
    /* Waits: the teardown follows immediately, and this runs on the gesture
     * worker rather than the session thread. */
    feedback_play(c, 1, what, true, true);
}
