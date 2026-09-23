/* --- the DS4's Bluetooth confirmation tone ---------------------------------
 *
 * T-238. A refusal, and only a refusal, for now.
 *
 * ⭐⭐ WHY THE REFUSAL FIRST, AND IT IS A SAFETY ARGUMENT RATHER THAN A
 * CONVENIENCE ONE. A refusal happens when a plug FAILED, so the pad is not
 * bridged: no host audio is flowing to it, nothing shares the node, and
 * nothing is interrupted if the pad dislikes what it is sent. It is the one
 * case where getting this wrong costs nothing.
 *
 * ⛔⛔ THE DANGER THIS IS WRITTEN AGAINST. The DualSense's tone is audible only
 * because of a PRIME -- encoded silence sent ahead of the note to start its
 * decoder. On a DS4, sustained silence is the one thing known to have broken
 * the pad: the listener's `underrun_silence` was reverted after rhoquinn8217's
 * test on 2026-07-25, where "ALL audio died ... and the controller misbehaved",
 * suspected as "flooding 0x14s starves/roots the pad".
 * ⭐ WHAT MAKES THIS DIFFERENT, and it is the whole reason it was tried: that
 * failure was a CONTINUOUS silence lane with no end, filling underruns for as
 * long as the session lived. This is a BOUNDED burst that stops -- 600 ms, then
 * the notes, then the fd closes. Those are not the same thing.
 * ⚠️ If a pad ever does misbehave after a refusal, DS4SIG_PRIME_FRAMES is the
 * first thing to shorten, and 0 is a legal value worth trying before anything
 * else is touched.
 *
 * ⓘ Tone only. The DualSense plays its lightbar and its tone together; this
 * does not touch the light, so a failure here cannot darken a pad.
 *
 * 🔗 The byte layout is the listener's map, `ds4_usb_over_ds4_bt.map` LAYOUT B,
 * which is what the host already sends this pad whenever a game makes a sound.
 * Nothing here is derived. */

#include "ds4_signal_data.inl"

#define DS4SIG_REPORT_ID     0x14
#define DS4SIG_REPORT_LEN    270
#define DS4SIG_PAYLOAD_OFF   6
#define DS4SIG_FRAMES_PER_REPORT 2
#define DS4SIG_PACE_US       8000   /* two 4 ms frames in every report */

/* 600 ms, the same as the DualSense's SHORT prime. ⛔ Deliberately not its LONG
 * one: that is 1800 ms and exists for a pad never seen before, which is exactly
 * the length this pad has history with. Start at the short one. */
#define DS4SIG_PRIME_FRAMES  150

/* ⭐ Route 0x02, and the content is the same in both channels. The map's probed
 * model says a plain speaker route is a SPLIT -- SBC channel 0 to the speaker,
 * channel 1 to headphone-L -- and that the low nibble must carry 0x02 or 0x04
 * or the audio is choppy. So 0x02 with duplicated content reaches the speaker
 * AND a plugged headset's left, which is what a refusal wants when nothing is
 * known about the jack. ⚠️ 0x26 would be "both" but sets 0x20, which the same
 * note says means headphones-stereo -- silent with no headset plugged. */
#define DS4SIG_ROUTE         0x02

/* The DualSense's pacer, at this pad's rate. Absolute deadlines rather than a
 * sleep per report, for the reason written out beside btsig_wait_for: a
 * fixed sleep accumulates the write time and the decoder runs dry, which is
 * heard as a crack at the start and a tail cut short. */
static void ds4sig_wait_for(const struct timespec *t0, int n)
{
    long long due_us = (long long)n * DS4SIG_PACE_US;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long gone_us = (long long)(now.tv_sec - t0->tv_sec) * 1000000LL +
                        (now.tv_nsec - t0->tv_nsec) / 1000LL;
    long long wait_us = due_us - gone_us;
    if (wait_us <= 0) return;          /* already late; do not sleep at all */
    struct timespec ts;
    ts.tv_sec  = (time_t)(wait_us / 1000000LL);
    ts.tv_nsec = (long)((wait_us % 1000000LL) * 1000LL);
    nanosleep(&ts, NULL);
}

/* One pure-audio report. ⛔ Byte 1 has the HID bit OFF (0x40, not 0xC0): a
 * 0x14 carries no effects section, and effects stay in 0x11. Getting that bit
 * wrong is how a tone report would be read as a malformed effects report. */
static void ds4sig_build(uint8_t *out, uint16_t counter,
                         const uint8_t *f0, const uint8_t *f1)
{
    memset(out, 0, DS4SIG_REPORT_LEN);
    out[0] = DS4SIG_REPORT_ID;
    out[1] = 0x40;                      /* 0x40 | poll rate 0, HID bit off */
    out[2] = 0xa0;                      /* speaker, no microphone */
    out[3] = (uint8_t)(counter & 0xff); /* LE u16, stepping 2 per report */
    out[4] = (uint8_t)(counter >> 8);
    out[5] = DS4SIG_ROUTE;
    memcpy(out + DS4SIG_PAYLOAD_OFF, f0, DS4SIG_FRAME_BYTES);
    memcpy(out + DS4SIG_PAYLOAD_OFF + DS4SIG_FRAME_BYTES, f1, DS4SIG_FRAME_BYTES);
    /* The pad drops anything whose signature does not match, so this is the
     * step that decides whether the report is heard or silently discarded. */
    ctm_bt_sign_output(out, DS4SIG_REPORT_LEN);
}

/* ⭐ TWO NOTES, AND THEIR ORDER IS THE MESSAGE -- the DualSense's vocabulary,
 * at the same three frequencies, so one pad does not mean something different
 * from the other. 🔗 btsig_pattern_t, whose values these are. */
static void ds4sig_notes_for(int pattern,
                             const uint8_t (**first)[DS4SIG_FRAME_BYTES],
                             const uint8_t (**second)[DS4SIG_FRAME_BYTES])
{
    switch (pattern) {
    case 1:  /* BTSIG_HANDED_BACK -- high then low, falling, coming home */
        *first = g_ds4sig_high;  *second = g_ds4sig_low;   break;
    case 2:  /* BTSIG_REFUSED -- low then LOWER, sinking, it did not happen */
        *first = g_ds4sig_low;   *second = g_ds4sig_lower; break;
    default: /* BTSIG_HANDING_OVER -- low then high, rising, going to the host */
        *first = g_ds4sig_low;   *second = g_ds4sig_high;  break;
    }
}

/* Opens nothing and closes nothing: the caller owns the fd.
 *
 * ⚠️ WHAT IS DIFFERENT FOR A BRIDGE OR A HANDBACK, AND IS NOT YET MEASURED.
 * A refusal has the node to ITSELF -- the pad is not bridged, so no host audio
 * is flowing. A bridge and a handback happen around a pad that IS bridged, and
 * the host's own audio reaches it as 0x14 reports on this same node. Two
 * writers means interleaved frames. ⭐ SBC frames are independent, each with
 * its own header and scale factors, so interleaving should sound like a mix
 * rather than corrupt anything -- but "should" is doing work in that sentence
 * and nobody has listened to it yet. ⓘ The connect tone fires the instant a
 * bridge forms, which is the quietest moment available. */
static int ds4sig_play_fd(int fd, int pattern, ctm_controller_t *log_to, const char *node)
{
    if (fd < 0) return -1;

    /* ⛔⛔ THE TONE SWITCH, AND IT BELONGS HERE RATHER THAN AT EACH CALLER.
     * One check covers the bridge, the handback AND the refusal; a gate per
     * caller is a gate someone forgets. ⚠️ The refusal shipped earlier today
     * WITHOUT this and played whatever the switch said -- caught by
     * rhoquinn8217: *"We need to make sure that rumble lightbar and tone are
     * also gated by the USB Bridge settings like the dual sense."*
     * 🔗 The DualSense does the same thing in btsig_play_fd. */
    if (!ctm_sig_tone_on()) {
        ctl_log(log_to, "ds4sig: not played, the tone is switched off");
        return 0;             /* switched off is not a failure */
    }

    const uint8_t (*first)[DS4SIG_FRAME_BYTES];
    const uint8_t (*second)[DS4SIG_FRAME_BYTES];
    ds4sig_notes_for(pattern, &first, &second);

    /* The gap is silence rather than nothing, because the decoder wants a
     * stream rather than a pause. */
    const uint8_t *frames[DS4SIG_PRIME_FRAMES + DS4SIG_TONE_FRAMES +
                          DS4SIG_GAP_FRAMES + DS4SIG_TONE_FRAMES];
    int n = 0;
    for (int i = 0; i < DS4SIG_PRIME_FRAMES; i++) frames[n++] = g_ds4sig_silence;
    for (int i = 0; i < DS4SIG_TONE_FRAMES;  i++) frames[n++] = first[i];
    for (int i = 0; i < DS4SIG_GAP_FRAMES;   i++) frames[n++] = g_ds4sig_silence;
    for (int i = 0; i < DS4SIG_TONE_FRAMES;  i++) frames[n++] = second[i];

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint8_t rep[DS4SIG_REPORT_LEN];
    uint16_t counter = 0;
    int sent = 0, failed = 0, reports = 0;

    for (int i = 0; i + 1 < n; i += DS4SIG_FRAMES_PER_REPORT) {
        ds4sig_build(rep, counter, frames[i], frames[i + 1]);
        counter = (uint16_t)(counter + DS4SIG_FRAMES_PER_REPORT);
        const ssize_t w = write(fd, rep, sizeof rep);
        if (w == (ssize_t)sizeof rep) sent++;
        else failed++;
        reports++;
        ds4sig_wait_for(&t0, reports);
    }

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const long took_ms = (long)((t1.tv_sec - t0.tv_sec) * 1000LL +
                                (t1.tv_nsec - t0.tv_nsec) / 1000000LL);
    const long audio_ms = (long)reports * (DS4SIG_PACE_US / 1000);

    /* ⭐ One line, whatever happened, and it names the node -- there is no
     * controller object to name instead. ⚠️ `took` far above `audio` is a
     * decoder running dry, which is the shape that sounds cracked. */
    const char *what = (pattern == 1) ? "handback"
                     : (pattern == 2) ? "refusal" : "bridge";
    ctl_log(log_to, "ds4sig: %s, %d sent, %d failed, took %ldms for %ldms "
                    "of audio (prime %d) -- node %s",
            what, sent, failed, took_ms, audio_ms, DS4SIG_PRIME_FRAMES,
            node ? node : "?");

    return (failed == 0 && sent > 0) ? 0 : -1;
}

/* A REFUSAL ON A BLUETOOTH DS4, which has no session to play through.
 *
 * Opens, plays, closes -- nothing is held afterwards. The same shape as
 * ctm_signal_refused_bt next door, and for the same reason: a plug that failed
 * leaves no controller object behind, but the device node is still there.
 *
 * ⛔ BLUETOOTH ONLY. A cabled DS4 reaches its speaker through a USB sound card,
 * and the pads here have none -- so `ds4_usb` must never arrive at this
 * function. 🔗 T-229 item C. */
/* The tone for a pad that IS bridged -- a handover or a handback -- played
 * down the session's own fd rather than a freshly opened node.
 *
 * ⚠️ CALLER'S JOB, NOT THIS FUNCTION'S: the claim and the thread. A tone
 * takes about a second, and the session thread carries the pad's reports, so
 * sleeping on it starves the very thing being waited for. 🔗 The note beside
 * feedback_play, which learned that the expensive way.
 * ⓘ The tone switch is checked inside ds4sig_play_fd, so every caller gets it. */
int ds4_signal_tone_bt(ctm_controller_t *c, int pattern)
{
    if (!c || c->hid_fd < 0) return -1;
    /* ⓘ Slot 3 counts host audio reports the patch hook dropped while this
     * signal held the pad. Zeroed here and read by the caller's log line, so a
     * run says whether the drop fired rather than leaving it to be assumed. */
    ctm_controller_set_type_state(c, 3 /* DS4_SLOT_AUDIO_DROPPED */, 0);
    return ds4sig_play_fd(c->hid_fd, pattern, c, c->dev.path);
}

int ds4_signal_refused_bt(const char *node)
{
    if (!node || !node[0]) return -1;
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = ds4sig_play_fd(fd, 2 /* BTSIG_REFUSED */, NULL, node);
    close(fd);
    return rc;
}
