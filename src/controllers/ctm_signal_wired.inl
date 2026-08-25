/* --- confirmation signals for a WIRED controller, with no session ------------
 *
 * FORK-ONLY. One call site reaches this; deleting the file and that call
 * removes the feature.
 *
 * WHAT IT IS FOR. A refused plug leaves nothing behind -- no controller
 * object, no open sound device -- which is why the refusal signal on a cable
 * has always been the coarse SDL rumble. It does not have to be: the sound
 * card is created by the KERNEL when the controller is plugged into the TV,
 * not by anything we do, so it is sitting there unopened at the moment a
 * bridge fails.
 *
 * Measured 2026-08-12: with a controller plugged in and NOTHING bridged,
 * /proc/asound/cards already lists it.
 *
 * ⚠️ ONE CONTROLLER ONLY, FOR NOW. With two plugged in there is no way to tell
 * from here which card belongs to which, and buzzing the wrong one is worse
 * than buzzing coarsely. The probe that answers it needs a hidraw handle and a
 * muted microphone, which is a bigger piece; until then this declines and the
 * caller falls back. */

/* ⚠️ WRITES TO A FILE, not just stderr.
 *
 * The first version logged with fprintf(stderr) alone, and the app's stderr
 * is not where the logs we read end up -- so a probe measurement that took a
 * quiet room and three controllers to set up was simply not recorded. The
 * numbers have to land somewhere greppable or the run is wasted. */
static void wired_sig_log(const char *fmt, ...)
{
    char body[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    fprintf(stderr, "[wiredsig] %s\n", body);

    FILE *f = fopen("/tmp/ctm-signal.log", "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "%lld.%03ld [wiredsig] %s\n",
            (long long)ts.tv_sec, ts.tv_nsec / 1000000L, body);
    fclose(f);
}

#define WIRED_SIG_RATE      48000
#define WIRED_SIG_CHANNELS  4        /* speaker L/R, then haptics L/R */
#define WIRED_SIG_MS        140
#define WIRED_SIG_GAP_MS    40

/* The same three notes as everywhere else. Kept beside the Bluetooth ones in
 * spirit: a cable and a radio must say the same thing, or the sound stops
 * being the message. */
static int wired_sig_hz(btsig_pattern_t pattern, int which)
{
    switch (pattern) {
    case BTSIG_HANDED_BACK: return which == 0 ? 990 : 660;   /* falling */
    case BTSIG_REFUSED:     return which == 0 ? 660 : 495;   /* sinking */
    default:                return which == 0 ? 660 : 990;   /* rising  */
    }
}

static double wired_sig_level(int hz)
{
    if (hz == 990) return 0.95;
    if (hz == 495) return 1.55;
    return 1.25;
}

/* Fill a four-channel buffer with the two notes and the felt pulse under them.
 *
 * The envelope holds and then releases rather than fading to nothing: a low
 * note dwindling on a speaker this small is inaudible before it has finished,
 * which made every signal ending low sound cut off. */
static int16_t *wired_sig_render(btsig_pattern_t pattern, int *frames_out,
                                 size_t *bytes_out, int lead_frames)
{
    const int per = (WIRED_SIG_RATE * WIRED_SIG_MS) / 1000;
    const int gap = (WIRED_SIG_RATE * WIRED_SIG_GAP_MS) / 1000;
    /* ⭐ lead_frames is silence in front, for a device whose audio has only
     * just been opened -- it swallows the start of the first thing written.
     * See the matching note in ctm_feedback.inl. */
    const int frames = lead_frames + per * 2 + gap;
    const size_t bytes = (size_t)frames * WIRED_SIG_CHANNELS * sizeof(int16_t);

    int16_t *buf = (int16_t *)calloc(1, bytes);
    if (!buf) return NULL;

    const int attack  = per / 10;
    const int release = (per * 18) / 100;

    for (int n = 0; n < 2; ++n) {
        const int hz = wired_sig_hz(pattern, n);
        const double level = wired_sig_level(hz);
        const int base = lead_frames + n * (per + gap);

        for (int i = 0; i < per; ++i) {
            double env;
            if (i < attack)             env = (double)i / (double)attack;
            else if (i > per - release) env = (double)(per - i) / (double)release;
            else                        env = 1.0;

            const double t = (double)i / (double)WIRED_SIG_RATE;
            const double tone = sin(2.0 * M_PI * hz * t) * env * level;
            const double bump = sin(2.0 * M_PI * 60.0 * t) * env;

            const int f = (base + i) * WIRED_SIG_CHANNELS;
            buf[f + 0] = buf[f + 1] = (int16_t)(tone * 9000.0);
            buf[f + 2] = buf[f + 3] = (int16_t)(bump * 14000.0);
        }
    }

    *frames_out = frames;
    *bytes_out = bytes;
    return buf;
}

/* --- which card belongs to which controller --------------------------------
 *
 * With one plugged in the question does not arise. With two it does, and the
 * only thing that can answer it is the probe: mute this controller's
 * microphone and see which card goes silent.
 *
 * ⚠️ THE PROBE COSTS ABOUT THREE SECONDS -- 1500 ms per read pass, two passes,
 * plus a settle. That is a long time to wait to be told a plug failed.
 *
 * ⭐ SO THE ANSWER IS REMEMBERED. A card does not change while its controller
 * stays plugged in, so the cost is paid once and every signal afterwards is
 * instant. rhoquinn8217, 2026-08-12: the point is to bridge the controller, and a
 * bridge that eventually succeeds needs this answer anyway -- so finding it on
 * the first refusal is work brought forward rather than work added.
 *
 * ⭐ AND THE WAIT IS DELIBERATE. A signal that is always the same, three
 * seconds late, is more trustworthy than one that is sometimes rich and
 * sometimes coarse -- you would learn to read the fast one as "something is
 * wrong", which is exactly the distrust the vocabulary exists to avoid. */

/* ⓘ THE CACHE LIVES IN ctm_cardmatch.inl NOW, not here.
 *
 * It had its own, which meant a bridge and a refusal answered the same
 * question separately and the three seconds was paid twice for one controller.
 * Whoever answers first writes it down; see the note there. */

/* Mute or unmute this controller's microphone, with no session behind it.
 *
 * A copy of what the session-bound probe does, writing to the node directly.
 * ⚠️ The permission is in FLAG PANEL 2, not panel 1 -- every other setting in
 * this project claims its fields in panel 1, so panel 1 is the reflex, and
 * claiming mute there does nothing at all. */
static void wired_sig_set_mute(const char *node, int muted)
{
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    uint8_t rep[DS5_OUT_REPORT_LEN];
    memset(rep, 0, sizeof(rep));
    rep[0] = DS5_OUT_REPORT_ID;
    rep[DS5_IDX_VALID_FLAG1] = DS5_F1_ALLOW_AUDIO_MUTE | DS5_F1_ALLOW_MUTE_LIGHT;
    rep[DS5_IDX_MUTES]       = muted ? DS5_MIC_MUTE : 0x00;
    rep[DS5_IDX_MUTE_LIGHT]  = muted ? DS5_MUTE_LIGHT_ON : 0x00;
    (void)!write(fd, rep, sizeof(rep));
    close(fd);
}

/* Which sound card is this controller's? Cached after the first answer.
 *
 * ⚠️ THE CALLER MUST HOLD g_cardmatch_lock. Ported from cardmatch_identify,
 * which documents the same requirement: the lock covers the probe AND
 * everything that follows it on the card, not just the reading. */
static int wired_sig_find_card_locked(const char *node)
{
    int cached = cardmatch_cache_get(node);
    if (cached >= 0) return cached;

    /* ⛔ ENUMERATE AND OPEN TOGETHER, skipping cards that will not open.
     *
     * A card a bridged session is holding cannot be opened, and it is not a
     * candidate -- so it must not be COUNTED as one either. Listing the cards
     * first and opening them later, as this used to, meant a busy card
     * inflated the count; and with a single busy card the "only one, no need
     * to probe" shortcut would hand back a card nothing can play on.
     *
     * cardmatch_identify does it in one loop for exactly this reason. */
    cardmatch_slot_t slots[CARDMATCH_MAX_CARDS];
    int n = 0;
    for (int card = 0; card < CARDMATCH_MAX_CARDS && n < CARDMATCH_MAX_CARDS; ++card) {
        if (!cardmatch_is_dualsense(card)) continue;
        int fd = cardmatch_open_card(card);
        if (fd < 0) continue;              /* busy: a bridged session holds it */
        slots[n].card = card;
        slots[n].fd = fd;
        slots[n].peak_live = -1;
        slots[n].peak_muted = -1;
        ++n;
    }

    if (n == 0) {
        wired_sig_log("no free DualSense card for %s", node);
        return -1;
    }

    /* ⭐ ANSWER BY ELIMINATION WHERE THAT IS POSSIBLE.
     *
     * Every card already claimed by ANOTHER controller is not ours. If that
     * leaves exactly one free card unclaimed, it is ours by elimination and
     * there is nothing to probe for.
     *
     * Covers the obvious case -- one controller, one card -- and the less
     * obvious one rhoquinn8217 raised: three plugged in, two of them already answered
     * for, so the third needs no three-second wait to learn what is left.
     *
     * ⓘ A card a bridged session holds never got opened above, so it is not in
     * this list at all -- that case eliminates itself.
     *
     * ⚠️ IT TRUSTS THE CACHE, and the cache is keyed by device node. Nodes are
     * reused: unplug a controller, plug in another, and it may land on the
     * same path with a different card. The cache would then be wrong, and so
     * would an elimination based on it -- playing a signal on someone else's
     * controller, which is the thing this whole probe exists to prevent.
     * ➡️ The proper fix is to forget a node when its controller goes away.
     * Until then the risk is the same one the cache already carries; this
     * does not add a new kind, only more places it matters. */
    int unclaimed = -1, unclaimed_count = 0;
    for (int i = 0; i < n; ++i) {
        int owned_by_other = 0;
        for (int k = 0; k < g_cardmatch_cached; ++k) {
            if (g_cardmatch_cache[k].card == slots[i].card &&
                strcmp(g_cardmatch_cache[k].node, node) != 0) {
                owned_by_other = 1;
                break;
            }
        }
        if (!owned_by_other) { unclaimed = i; ++unclaimed_count; }
    }

    if (unclaimed_count == 1) {
        const int card = slots[unclaimed].card;
        for (int i = 0; i < n; ++i) close(slots[i].fd);
        cardmatch_cache_put(node, card);
        wired_sig_log("%d free cards, %d already spoken for -> %s is card=%d "
                      "by elimination, no probe needed", n, n - 1, node, card);
        return card;
    }

    cardmatch_read_all(slots, n, 0);
    wired_sig_set_mute(node, 1);
    struct timespec settle = {0, 200 * 1000000};
    nanosleep(&settle, NULL);
    cardmatch_read_all(slots, n, 1);
    wired_sig_set_mute(node, 0);                 /* ALWAYS, even on the way out */

    /* ⚠️ THE NUMBERS ARE LOGGED, not just the verdict.
     *
     * The whole design rests on "live is never zero, muted is always zero".
     * The muted half is proven -- a muted controller reads exactly zero, seen
     * host-side and via the button. The live half is the one that could fail:
     * a microphone in a genuinely silent room reading zero would make this
     * probe say nothing matched. Physics says it cannot -- a diaphragm is
     * never still and the electronics have their own noise floor -- but the
     * controller does its own signal processing, and a gate could produce
     * digital silence where physics would not.
     *
     * So every peak goes in the log. If this ever declines, the reason is
     * right there rather than needing another session to find. */
    for (int i = 0; i < n; ++i) {
        wired_sig_log("card=%d live=%d muted=%d",
                      slots[i].card, slots[i].peak_live, slots[i].peak_muted);
    }

    /* Close first, decide second -- deciding first meant an early break left
     * the remaining cards open, and a leaked capture handle is a card the next
     * probe cannot use. */
    for (int i = 0; i < n; ++i) close(slots[i].fd);

    int answer = -1;
    for (int i = 0; i < n; ++i) {
        /* Ours is the one that was live and went silent. */
        if (slots[i].peak_live > 0 && slots[i].peak_muted == 0) {
            if (answer >= 0) { answer = -1; break; }   /* two matched: no answer */
            answer = slots[i].card;
        }
    }

    if (answer >= 0) cardmatch_cache_put(node, answer);
    wired_sig_log("probe of %d free cards for %s -> card=%d", n, node, answer);
    return answer;
}

/* Tell the controller to route to its speaker and turn it up.
 *
 * Without this the tone plays into whatever the controller was last set to,
 * which after a power-on is the headphone jack at a volume too low to hear --
 * the fault that took a week to find on the wired path. Echo cancellation is
 * part of it: the controller suppresses its own speaker without it.
 *
 * ⚠️ CALLED AFTER THE DEVICE IS OPEN, not before. The session does it in that
 * order -- the init is the last thing ctm_controller_open_alsa_playback does
 * -- and the other way round is silently wasted. */
static void wired_sig_prepare(const char *node)
{
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    uint8_t rep[DS5_OUT_REPORT_LEN];
    memset(rep, 0, sizeof(rep));
    rep[0] = DS5_OUT_REPORT_ID;
    rep[DS5_IDX_VALID_FLAG0]    = DS5_F0_ALLOW_SPEAKER_VOLUME |
                                  DS5_F0_ALLOW_AUDIO_CONTROL;
    rep[DS5_IDX_SPEAKER_VOLUME] = 0x64;
    /* ⓘ Echo cancel only -- noise cancel is beamforming and cost us the
     * microphone's second channel. See the note in controller_common.c. */
    rep[DS5_IDX_AUDIO_CONTROL]  = DS5_AUDIO_OUT_PATH_SPEAKER |
                                  DS5_AUDIO_ECHO_NOISE_CANCEL;
    (void)!write(fd, rep, sizeof(rep));
    close(fd);
}

/* Play a signal on a wired controller with no session behind it.
 * Returns 0 if it ran, -1 if it declined or failed. */
int ctm_signal_wired_no_session(const char *node, int pattern)
{
    if (!node || !node[0]) return -1;

    /* ⛔ THE LOCK COVERS THE PROBE *AND* THE PLAY.
     *
     * It used to be released the moment the probe answered, and the card was
     * opened afterwards -- so a bridge starting in that gap could take the
     * card out from under us. The session path holds it across identify, the
     * capture start and the tone for exactly this reason, and says so.
     *
     * ⚠️ A refusal therefore WAITS if a bridge is mid-probe. Correct, and it
     * can look like a hang for a few seconds. */
    pthread_mutex_lock(&g_cardmatch_lock);

    const int card = wired_sig_find_card_locked(node);
    if (card < 0) {
        pthread_mutex_unlock(&g_cardmatch_lock);
        return -1;
    }

    /* ⛔ THE REAL OPENER, AND THE INIT AFTER IT -- both ported from
     * ctm_controller_open_alsa_playback rather than approximated.
     *
     * This used cardmatch_open_playback_card, which exists for the probe's own
     * diagnostic tone, and sent the speaker settings BEFORE opening. The
     * session does the opposite: open, then init, with the init as the last
     * thing that function does.
     *
     * ⭐ And the wrong order had a signature that pointed straight at it: the
     * FIRST signal on a controller was silent and the SECOND played. The first
     * attempt's init was wasted, but it left the controller routed to its
     * speaker at full volume -- so the second played on settings the first one
     * had set. Measured on three controllers, 2026-08-12: silent, then
     * audible, on each of them in turn. */
    int fd = open_ds5_alsa_playback(node, card);
    if (fd < 0) {
        wired_sig_log("card=%d would not open for playback", card);
        pthread_mutex_unlock(&g_cardmatch_lock);
        return -1;
    }

    wired_sig_prepare(node);

    int frames = 0;
    size_t bytes = 0;
    /* ⭐⭐ HOLD UNTIL THE CABLE'S AUDIO IS READY.
     *
     * ⛔ This path could never be fixed by waiting BEFORE the gesture, which is
     * how it was found: waiting fifteen seconds then bridging gave a tone, and
     * waiting then refusing did not. The card is opened at the moment of
     * refusal, so it is always freshly opened -- but the clock that matters
     * started when the CABLE went in, and this asks about that. */
    const long settle_ms = feedback_settle_left_ms(node);
    if (settle_ms > 0) {
        wired_sig_log("wired signal: holding %ldms for %s to settle", settle_ms, node);
        struct timespec sts = {(time_t)(settle_ms / 1000),
                               (long)(settle_ms % 1000) * 1000000L};
        nanosleep(&sts, NULL);
    }
    int16_t *buf = wired_sig_render((btsig_pattern_t)pattern, &frames, &bytes, 0);
    if (!buf) {
        close(fd);
        pthread_mutex_unlock(&g_cardmatch_lock);
        return -1;
    }

    /* ⛔ WRITE IT IN PIECES, AND DO NOT CALL A SHORT WRITE A SUCCESS.
     *
     * The first version handed the whole buffer to the device in one call.
     * The card is opened non-blocking, and a third of a second of audio is far
     * more than its buffer holds -- so it took what fitted, returned, and the
     * result was read as "played". Measured: the log said the signal had been
     * sent and not one of three controllers made a sound.
     *
     * A period at a time, waiting when the device is full, exactly as the
     * session path does. Bounded, so a device that never drains cannot hold
     * this thread forever. */
    const int chunk = 480;                 /* 10 ms */
    int done = 0, stalls = 0, rc = 0;
    while (done < frames && stalls < 400) {
        struct snd_xferi xfer;
        memset(&xfer, 0, sizeof(xfer));
        xfer.buf = buf + (size_t)done * WIRED_SIG_CHANNELS;
        xfer.frames = (snd_pcm_uframes_t)((frames - done) < chunk
                                          ? (frames - done) : chunk);
        rc = ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
        if (rc < 0) {
            if (errno == EPIPE) {          /* underrun: restart and carry on */
                ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
                ++stalls;
                continue;
            }
            if (errno == EAGAIN) {         /* full: wait for room */
                struct timespec w = {0, 1000000};
                nanosleep(&w, NULL);
                ++stalls;
                continue;
            }
            break;                         /* not recoverable */
        }
        done += (xfer.result > 0) ? (int)xfer.result : (int)xfer.frames;
    }
    if (done < frames) rc = -1;

    /* Let it come out before the device closes -- closing throws away whatever
     * is still queued, which is how the tone became a race on the unplug path
     * and lost about half the time. Derived from the buffer so it cannot
     * drift when the signal changes length. */
    const long wait_ms = (long)frames * 1000L / WIRED_SIG_RATE + 40;
    struct timespec ts = {(time_t)(wait_ms / 1000),
                          (long)(wait_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);

    free(buf);
    close(fd);
    pthread_mutex_unlock(&g_cardmatch_lock);

    wired_sig_log("node=%s card=%d pattern=%d rc=%d, %d of %d frames, %d stalls, waited %ldms",
                  node, card, pattern, rc, done, frames, stalls, wait_ms);
    return rc < 0 ? -1 : 0;
}
