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

/* --- identifying WHICH card, with more than one free ---------------------
 *
 * The measurement above proves a muted microphone reads exactly zero and a
 * live one does not. This turns that into an answer.
 *
 * Open every free DualSense card at once. Read them all with the controller
 * live, then mute it and read them all again. The card that was speaking and
 * has fallen exactly silent is that controller's. The others carry on
 * reporting room noise, because nothing was done to them.
 *
 * ALL THE READINGS HAPPEN AT THE SAME TIME, in one loop over every card
 * rather than card by card. Reading them in sequence would compare cards
 * measured seconds apart, which room noise alone could decide.
 *
 * "FREE" MEANS OPENABLE. A bridged controller's session is holding its own
 * card, so those refuse to open and drop out by themselves. What is left is
 * exactly the set of cards belonging to unbridged controllers -- which
 * includes the one plugging in.
 *
 * THE NAME IS NOT USED TO DECIDE ANYTHING. Candidates are gathered by name,
 * because that is how to know a card is a DualSense at all, but the ANSWER
 * comes from which one falls silent. That matters with an Edge in the mix:
 * its card is called "DualSense Edge Wireless Control" and a ds5's is
 * "DualSense Wireless Controller", so a name match finds both and cannot
 * tell them apart. Silence can. */

#define CARDMATCH_MAX_CARDS 8

/* ONE PROBE AT A TIME, ACROSS ALL CONTROLLERS.
 *
 * A probe opens every free card and mutes one controller. Two running at once
 * collide twice over: the second finds no free cards because the first is
 * holding them all, and if their muted windows overlap then two cards fall
 * silent and neither probe can say which silence was its own. The ambiguity
 * check would catch that and refuse to answer -- safe, but a failure.
 *
 * Serialising also makes the second probe BETTER rather than merely correct:
 * by the time it runs, the first controller is bridged and holding its own
 * card, so that card is no longer free and drops out of the candidates. One
 * less card to tell apart.
 *
 * The cost is that a second plug-in waits about three seconds. That is a
 * hand-driven action, so the wait is invisible in practice. */
static pthread_mutex_t g_cardmatch_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int card;
    int fd;
    int peak_live;
    int peak_muted;
} cardmatch_slot_t;

/* Open one specific card for capture. The scan version picks a card; this
 * one is told which. Same hardware parameters -- deliberately a copy rather
 * than a refactor of the shared opener, so upstream's function is untouched. */
static int cardmatch_open_card(int card)
{
    char path[128];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0c", card);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct snd_pcm_hw_params hw;
    memset(&hw, 0, sizeof(hw));
    for (int i = 0; i < (int)(sizeof(hw.intervals)/sizeof(hw.intervals[0])); i++) {
        hw.intervals[i].min = 0;
        hw.intervals[i].max = 0xFFFFFFFFU;
    }
    for (int i = 0; i < (int)(sizeof(hw.masks)/sizeof(hw.masks[0])); i++)
        memset(&hw.masks[i], 0xFF, sizeof(hw.masks[i]));
    memset(&hw.masks[0], 0, sizeof(hw.masks[0]));
    hw.masks[0].bits[3 / 32] = 1u << (3 % 32);   /* RW_INTERLEAVED */
    memset(&hw.masks[1], 0, sizeof(hw.masks[1]));
    hw.masks[1].bits[2 / 32] = 1u << (2 % 32);   /* S16_LE */
    hw.intervals[2].min = hw.intervals[2].max = MIC_CAP_CHANNELS;
    hw.intervals[2].integer = 1;
    hw.intervals[3].min = hw.intervals[3].max = MIC_CAP_RATE;
    hw.intervals[3].integer = 1;
    hw.intervals[9].min = 960;
    hw.intervals[9].max = 0xFFFFFFFFU;
    hw.intervals[9].integer = 0;

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0 ||
        ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL) < 0) {
        close(fd);
        return -1;
    }
    ioctl(fd, SNDRV_PCM_IOCTL_START, NULL);
    return fd;
}

/* Open ONE named card for playback, without touching the controller's current
 * device. The ordinary opener refuses when a device is already open, which is
 * exactly the case here: the point is to have the replacement in hand before
 * letting go of what we have. */
static int cardmatch_open_playback_card(ctm_controller_t *c, int card)
{
    char path[128];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0p", card);
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        ctl_log(c, "cardmatch: card=%d playback busy or absent (errno=%d)",
                card, errno);
        return -1;
    }

    struct snd_pcm_hw_params hw;
    memset(&hw, 0, sizeof(hw));
    for (int i = 0; i < (int)(sizeof(hw.intervals)/sizeof(hw.intervals[0])); i++) {
        hw.intervals[i].min = 0;
        hw.intervals[i].max = 0xFFFFFFFFU;
    }
    for (int i = 0; i < (int)(sizeof(hw.masks)/sizeof(hw.masks[0])); i++)
        memset(&hw.masks[i], 0xFF, sizeof(hw.masks[i]));
    memset(&hw.masks[0], 0, sizeof(hw.masks[0]));
    hw.masks[0].bits[3 / 32] = 1u << (3 % 32);   /* RW_INTERLEAVED */
    memset(&hw.masks[1], 0, sizeof(hw.masks[1]));
    hw.masks[1].bits[2 / 32] = 1u << (2 % 32);   /* S16_LE */
    hw.intervals[2].min = hw.intervals[2].max = 4;   /* speaker + haptics */
    hw.intervals[2].integer = 1;
    hw.intervals[3].min = hw.intervals[3].max = MIC_CAP_RATE;
    hw.intervals[3].integer = 1;
    hw.intervals[9].min = 960;
    hw.intervals[9].max = 0xFFFFFFFFU;
    hw.intervals[9].integer = 0;

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0 ||
        ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL) < 0) {
        ctl_log(c, "cardmatch: card=%d hw_params failed (errno=%d)", card, errno);
        close(fd);
        return -1;
    }
    return fd;
}

/* Does this card belong to a DualSense of any kind? Name only -- used to
 * gather candidates, never to choose between them. */
static int cardmatch_is_dualsense(int card)
{
    char path[128];
    snprintf(path, sizeof(path), "/proc/asound/card%d/stream0", card);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "DualSense")) { found = 1; break; }
    }
    fclose(f);
    return found;
}

/* Read every open card at once for MATCH_READ_MS, recording each one's peak.
 * `which` selects the field to fill so the same loop serves both passes. */
static void cardmatch_read_all(cardmatch_slot_t *slots, int n, int muted_pass)
{
    int16_t buf[MIC_CAP_FRAMES * MIC_CAP_CHANNELS];
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < n; ++i) {
        if (muted_pass) slots[i].peak_muted = -1;
        else            slots[i].peak_live  = -1;
        /* Start from an empty buffer. Without this the reading is really a
         * measure of what the room sounded like a moment ago -- the fault
         * that made the first version of this look like a pass. */
        ioctl(slots[i].fd, SNDRV_PCM_IOCTL_DROP, NULL);
        ioctl(slots[i].fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
        ioctl(slots[i].fd, SNDRV_PCM_IOCTL_START, NULL);
    }

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec) * 1000 +
                  (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms >= MATCH_READ_MS) break;

        struct pollfd pfd[CARDMATCH_MAX_CARDS];
        for (int i = 0; i < n; ++i) {
            pfd[i].fd = slots[i].fd;
            pfd[i].events = POLLIN;
            pfd[i].revents = 0;
        }
        if (poll(pfd, (nfds_t)n, 50) <= 0) continue;

        for (int i = 0; i < n; ++i) {
            if (!(pfd[i].revents & POLLIN)) continue;
            struct snd_xferi xfer;
            memset(&xfer, 0, sizeof(xfer));
            xfer.buf = buf;
            xfer.frames = MIC_CAP_FRAMES;
            if (ioctl(slots[i].fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xfer) < 0) {
                if (errno == EPIPE) {
                    ioctl(slots[i].fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
                    ioctl(slots[i].fd, SNDRV_PCM_IOCTL_START, NULL);
                }
                continue;
            }
            long got = (long)xfer.result;
            if (got <= 0) continue;
            int *peak = muted_pass ? &slots[i].peak_muted : &slots[i].peak_live;
            if (*peak < 0) *peak = 0;
            for (long k = 0; k < got * MIC_CAP_CHANNELS; ++k) {
                int v = buf[k] < 0 ? -buf[k] : buf[k];
                if (v > *peak) *peak = v;
            }
        }
    }
}

/* Work out which free card belongs to this controller, and say so.
 *
 * LOGS THE ANSWER AND CHANGES NOTHING. It also records which card the
 * ordinary scan would have taken, so the two can be compared: when they
 * disagree, that is the misrouting caught in the act rather than reported
 * afterwards. */
static void cardmatch_identify(ctm_controller_t *c)
{
    if (!c || strcmp(ctm_controller_bus(c), "USB") != 0) return;

    /* The lock is held by the CALLER, across this, the capture start and the
     * tone -- see the note at that call. */

    cardmatch_slot_t slots[CARDMATCH_MAX_CARDS];
    int n = 0;
    for (int card = 0; card < CARDMATCH_MAX_CARDS && n < CARDMATCH_MAX_CARDS; ++card) {
        if (!cardmatch_is_dualsense(card)) continue;
        int fd = cardmatch_open_card(card);
        if (fd < 0) continue;      /* busy: a bridged session holds it */
        slots[n].card = card;
        slots[n].fd = fd;
        slots[n].peak_live = -1;
        slots[n].peak_muted = -1;
        n++;
    }

    if (n == 0) {
        ctl_log(c, "cardmatch: no free DualSense card to identify");
        return;
    }

    /* What the ordinary scan would take: the lowest-numbered free card. That
     * is the whole bug in one line -- it is first-free, not whose. */
    const int scan_would_pick = slots[0].card;

    /* The reading runs even with a single free card, where the answer is
     * already known. It costs three seconds and it is the only place the
     * live-versus-muted numbers get recorded -- and the assumption those
     * numbers test, that a live microphone never reads zero, has not yet
     * been checked in a genuinely silent room. Cheap insurance against
     * finding out the hard way. */
    cardmatch_read_all(slots, n, 0);

    cardmatch_set_mic_mute(c, 1);
    struct timespec settle = {0, 200 * 1000000};
    nanosleep(&settle, NULL);

    cardmatch_read_all(slots, n, 1);
    cardmatch_set_mic_mute(c, 0);

    /* The answer: was speaking, now exactly silent. Both halves matter -- a
     * card that read zero all along proves nothing, and a card still making
     * noise is somebody else's. */
    int answer = -1, ambiguous = 0;
    for (int i = 0; i < n; ++i) {
        if (slots[i].peak_live > 0 && slots[i].peak_muted == 0) {
            if (answer >= 0) ambiguous = 1;
            else answer = slots[i].card;
        }
    }

    /* NO "IF ONLY ONE CARD IS FREE IT MUST BE OURS" SHORTCUT.
     *
     * That was here, and it produced a confidently wrong answer in the one
     * case that matters. Measured on C1 2026-08-09 with three controllers:
     * the last to bridge found a single free card and was told it was its
     * own -- but its real card had already been taken by the controller that
     * bridged first, and the leftover belonged to someone else. The probe
     * read `card4=51/71`: nothing fell silent, which was the truth, and the
     * shortcut overrode it.
     *
     * THE LAST FREE CARD IS NOT YOURS. It is whatever nobody else grabbed.
     *
     * So a silence that never came is reported as no answer. "My card is
     * taken" is real information -- it is the case where the right move is to
     * take our own card back rather than accept the leftover. */

    char detail[192];
    int o = 0;
    for (int i = 0; i < n && o < (int)sizeof(detail) - 1; ++i) {
        o += snprintf(detail + o, sizeof(detail) - (size_t)o, "card%d=%d/%d ",
                      slots[i].card, slots[i].peak_live, slots[i].peak_muted);
    }

    if (answer < 0 && !ambiguous) {
        ctl_log(c, "cardmatch: no card fell silent -- our own card is taken");
    }

    if (ambiguous) {
        /* More than one card fell silent. Something else muted at the same
         * moment -- the TV's own kernel driver also writes this, and it is a
         * writer we cannot see. Say so rather than pick one. */
        ctl_log(c, "cardmatch: AMBIGUOUS -- more than one card fell silent");
        answer = -1;
    }

    alsa_log("[cardmatch]", "node=%s answer=%d scan=%d %s%s",
             c->dev.path, answer, scan_would_pick, detail,
             (answer >= 0 && answer != scan_would_pick) ? "<-- SCAN WOULD BE WRONG" : "");
    ctl_log(c, "cardmatch: answer=%d scan_would_pick=%d %s",
            answer, scan_would_pick, detail);

    c->matched_card = answer;

    for (int i = 0; i < n; ++i) close(slots[i].fd);

    /* Move the SPEAKER onto the right card.
     *
     * The speaker is opened during session setup, which happens before this
     * runs, so by now it is already holding whatever the scan gave it. The
     * capture side has no such problem -- it starts after this and simply
     * uses the answer.
     *
     * Reopening rather than reordering: the self-heal already closes and
     * reopens this device on a live session, so it is a proven path rather
     * than a new one, and it leaves upstream's setup order alone.
     *
     * Done unconditionally when there is an answer, without checking which
     * card is currently held. Knowing that would mean threading a card number
     * back out of upstream's opener; reopening a device that was already
     * correct costs a few milliseconds and cannot be wrong. */
    /* OPEN THE SPEAKER HERE, and nowhere earlier.
     *
     * This is the first moment the right card is known. Opening at plug time
     * meant grabbing whatever was free and swapping later, which crossed two
     * controllers so thoroughly that neither could get its own card back.
     *
     * `matched_card` is already set above, so the ordinary opener picks the
     * right card by itself -- and it is the ordinary opener deliberately, so
     * the settings report that follows a fresh handle stays in one place. */
    if (c->alsa_fd < 0) {
        ctm_controller_open_alsa_playback(c);
        ctl_log(c, "cardmatch: speaker opened on card=%d (fd=%d)",
                answer, c->alsa_fd);
    } else if (answer >= 0) {
        /* OPEN THE NEW CARD BEFORE CLOSING THE OLD ONE.
         *
         * This closed first, and on 2026-08-10 that cost a controller its
         * speaker for a whole session: the card it had been identified as
         * owning was still held by the OTHER controller, which had not yet
         * moved off it. The open failed, the working device was already
         * gone, and the log read:
         *     speaker closed (moving to card=3), fd=78
         *     speaker reopened on card=3 (fd=-1)
         *     connected -- no audio device, nothing played
         * No game audio, no haptics, no tone, for the rest of the session.
         *
         * Now the wrong card is kept when the right one cannot be taken.
         * That is still wrong -- audio comes from another controller -- but
         * it is recoverable and audible, where nothing is neither. */
        int fresh = cardmatch_open_playback_card(c, answer);
        if (fresh < 0) {
            ctl_log(c, "cardmatch: card=%d not available, keeping fd=%d",
                    answer, c->alsa_fd);
        } else {
            if (c->alsa_fd >= 0) {
                ctl_log(c, "alsa: speaker closed (moved to card=%d), fd=%d",
                        answer, c->alsa_fd);
                close(c->alsa_fd);
            }
            c->alsa_fd = fresh;
            ctl_log(c, "cardmatch: speaker reopened on card=%d (fd=%d)",
                    answer, c->alsa_fd);
            /* A fresh handle knows nothing about volume or routing, so the
             * settings report has to go again -- the same thing the ordinary
             * opener does after opening. */
            ctm_controller_send_speaker_init(c);
        }
    }
}
