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

/* ⭐⭐ 0x17 -- FOUR frames a report, not 0x14's two, and it is a fix rather
 * than a preference.
 *
 * ⛔ THE FAULT: with 0x14 the tone writes 125 reports a second, and on a pad
 * that is BRIDGED those writes BLOCK -- measured 2026-09-22, build 412:
 * `writes took 10257ms, worst 5119ms` against `0ms / 0ms` on a pad whose
 * session was tearing down. The link cannot carry the pad's input reports and
 * that many writes at once, so the frames arrive far too late and the decoder
 * plays a click.
 *
 * ➡️ 0x17 carries the same audio in HALF the writes: 462 bytes every 16 ms
 * instead of 270 every 8 ms -- 28.9 KB/s against 33.8 KB/s, and more
 * importantly 62 writes a second rather than 125. Fewer, larger writes are
 * what a Bluetooth link wants.
 * ⓘ The three sizes are the listener map's, LAYOUT B: 0x12 = 142 B / 1 frame,
 * 0x14 = 270 B / 2, 0x17 = 462 B / 4. Nothing here is derived. */
/* ⛔ BACK TO 0x14 (two frames) FROM 0x17 (four). 0x17 was tried on the theory
 * that fewer, larger writes would stop the blocking on a bridged pad -- and it
 * was MEASURED not to: `writes took 10221ms, worst 5103ms` at 62 writes a
 * second against `10257ms / 5119ms` at 125. It changed nothing.
 * ⚠️ And every handback HEARD through the microphone tonight was on 0x14;
 * none has been captured since the switch. A change that did not fix what it
 * was for and may have cost something else does not stay. */
#define DS4SIG_REPORT_ID     0x14
#define DS4SIG_REPORT_LEN    270
#define DS4SIG_PAYLOAD_OFF   6
#define DS4SIG_FRAMES_PER_REPORT 2
#define DS4SIG_PACE_US       8000   /* two 4 ms frames in every report */

/* ⭐⭐ 1800 ms -- the DualSense's LONG prime, and it is earned rather than
 * copied. It started at 600 ms, its SHORT one, deliberately: the shortest thing
 * that has ever worked, given this pad's history with silence.
 *
 * ⚠️ WHAT 600 ms COULD NOT DO, measured 2026-09-22 across builds 402-407.
 * rhoquinn8217 heard a "pop" and one short note on a manual bridge, while the
 * SAME tone was correct when the auto-bridge fired at stream start. Three
 * readings settled it:
 *   - stream start: the host's audio has just begun, decoder WARM  -> correct
 *   - manual bridge, nothing playing: decoder IDLE                 -> fragment
 *   - release: warm if sound had been playing, cold if not   -> mostly correct
 * ⛔ And the theory it replaced was WRONG, twice: the host's audio sharing the
 * node. The drop added for that counted `dropped=0` on every tone, so no host
 * audio was arriving at all and there was never anything to compete with.
 *
 * ➡️ So the prime is not about a pad never seen before, as the DualSense's
 * comment frames it. It is about a decoder that has gone IDLE, which happens
 * whenever nothing has played for a while -- far more often than once.
 *
 * ⓘ Cost: the tone runs about 2.1 s instead of 0.9 s. ⭐ Worth revisiting once
 * it is known to work: priming long only when nothing has played recently would
 * give a short tone in the common case, and needs a "last audio seen" timestamp
 * this file does not have yet. */
/* ⛔ THE BASE IS THE SHORT PRIME. The per-pad rule below triples it for a pad
 * this run has not primed, which is where 1800 ms comes from -- exactly the
 * DualSense's pair of numbers.
 * ⚠️ It was briefly 450 here, which the tripling turned into 1350 frames, and
 * THAT measured `716 sent, took 19576ms for 5728ms of audio` -- the pad's link
 * cannot absorb a burst that long and the pacing collapsed to a third of real
 * time. ⭐ A prime long enough to starve the decoder is worse than a short one. */
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
                         const uint8_t *const *f)
{
    memset(out, 0, DS4SIG_REPORT_LEN);
    out[0] = DS4SIG_REPORT_ID;
    out[1] = 0x40;                      /* 0x40 | poll rate 0, HID bit off */
    out[2] = 0xa0;                      /* speaker, no microphone */
    out[3] = (uint8_t)(counter & 0xff); /* LE u16, stepping 2 per report */
    out[4] = (uint8_t)(counter >> 8);
    out[5] = DS4SIG_ROUTE;
    for (int i = 0; i < DS4SIG_FRAMES_PER_REPORT; ++i) {
        memcpy(out + DS4SIG_PAYLOAD_OFF + (size_t)i * DS4SIG_FRAME_BYTES,
               f[i], DS4SIG_FRAME_BYTES);
    }
    /* The pad drops anything whose signature does not match, so this is the
     * step that decides whether the report is heard or silently discarded. */
    ctm_bt_sign_output(out, DS4SIG_REPORT_LEN);
}

/* ⭐⭐ THE CONFIGURE REPORT, AND IT IS THE PIECE THAT WAS MISSING.
 *
 * rhoquinn8217, 2026-09-22: *"are you trying the primer method we did for
 * ds5?"* ⛔ The answer was no. The DualSense's prime is not silence alone --
 * `f.configure = (i == 0)` makes its FIRST report a configure: claim the
 * audio, set the volumes, set the routing. 🔗 btsig_play_fd, and the suite's
 * "the speaker is configured only when asked". This tone had no such step; it
 * simply began firing 0x14 frames at a pad it had never told to listen.
 *
 * ⛔ IT IS AN EFFECTS REPORT, NOT AN AUDIO ONE: 0x11 with the HID bit ON
 * (0xc0), where the audio frames are 0x14 with it OFF. Byte 3's high bits are
 * the volume-valid flags -- 0x10/0x20 headphone L/R, 0x80 speaker -- and its
 * low nibble is left at zero so the rumble and the lightbar are not claimed.
 * 🔗 The same shape ds4_bt_send_audio uses, which the pad is known to obey.
 *
 * ⓘ Once, at the head of the prime, exactly as the DualSense does it. The
 * suite's note beside that -- "doing it 100x/s stalled the link" -- is why. */
#define DS4SIG_CFG_LEN  78

static void ds4sig_build_configure(uint8_t *out, uint8_t headphone, uint8_t speaker)
{
    memset(out, 0, DS4SIG_CFG_LEN);
    out[0]  = 0x11;
    out[1]  = 0xc0;   /* HID bit ON: an effects report */
    out[2]  = 0xa0;
    out[3]  = 0xb0;   /* volumes valid; rumble and light NOT claimed */
    out[21] = headphone;
    out[22] = headphone;
    out[24] = speaker;
    ctm_bt_sign_output(out, DS4SIG_CFG_LEN);
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


    /* ⛔⛔ ALWAYS THE LONG PRIME. The DualSense's per-pad memory is WRONG for
     * this pad, and the logs say so plainly: every handback that was heard
     * tonight carried `prime 450`, and every silent one carried `prime 150` --
     * same pad, same build, same perfect delivery (`929ms for 928ms`, writes
     * 0ms).
     * ➡️ "Primed once" is not a property that LASTS on a DS4. Its decoder goes
     * idle between signals, so a table that remembers forever hands a short
     * prime to a cold decoder and the tone is simply not played.
     * ⓘ The cost is a 1.8 s lead-in on every tone. ⭐ Worth revisiting with a
     * LAST-HEARD TIMESTAMP rather than a flag -- short prime only if a tone
     * played in the last few seconds -- which needs state this file does not
     * keep yet. Correct and slow beats fast and silent. */
    /* ⛔ BACK TO THE PER-PAD RULE, build 414's. Long for a pad this run has not
     * primed, short afterwards. ⚠️ I replaced it with "always long" on 2026-09-22
     * because good handbacks logged `prime 450` and silent ones `prime 150` --
     * and rhoquinn8217's C3 showed that correlation was confounded: there the
     * delivery is PERFECT (`writes took 0ms`) and the release still went from
     * heard-regularly to hardly-ever. ➡️ The change did not buy what it claimed
     * and it cost the release, so it goes back. */
    const int primed = btsig_is_primed(node);
    const int prime_frames = primed ? DS4SIG_PRIME_FRAMES
                                    : (DS4SIG_PRIME_FRAMES * 3);

    /* The gap is silence rather than nothing, because the decoder wants a
     * stream rather than a pause. */
    const uint8_t *frames[(DS4SIG_PRIME_FRAMES * 3) + DS4SIG_TONE_FRAMES +
                          DS4SIG_GAP_FRAMES + DS4SIG_TONE_FRAMES];
    int n = 0;
    for (int i = 0; i < prime_frames; i++) frames[n++] = g_ds4sig_silence;
    for (int i = 0; i < DS4SIG_TONE_FRAMES;  i++) frames[n++] = first[i];
    for (int i = 0; i < DS4SIG_GAP_FRAMES;   i++) frames[n++] = g_ds4sig_silence;
    for (int i = 0; i < DS4SIG_TONE_FRAMES;  i++) frames[n++] = second[i];

    uint8_t rep[DS4SIG_REPORT_LEN];
    uint16_t counter = 0;
    int sent = 0, failed = 0, reports = 0;
    /* ⛔ TIMED, because `took 23953ms for 928ms of audio` on a bridged pad says
     * the writes are BLOCKING and the decoder is being starved to nothing. The
     * pacer sleeps against an absolute deadline and returns at once when late,
     * so it cannot be the one spending that time. This says so rather than
     * leaving it inferred. */
    long worst_write_ms = 0, total_write_ms = 0, cfg_ms = 0;
    unsigned cfg_hp = 0, cfg_sp = 0;

    /* The configure report, first and once. ⓘ Full volume when there is no
     * controller to ask -- a refusal has none, and a confirmation nobody hears
     * is worse than a loud one. */
    {
        uint8_t hp = 0x4f, sp = 0x4f;
        if (log_to) {
            tv_bridge_worker_settings_t st;
            ctm_controller_get_settings(log_to, &st);
            const unsigned h = st.headset_volume_percent, k = st.speaker_volume_percent;
            /* ⛔⛔ A ZERO IS NOT A SETTING HERE, IT IS AN ABSENCE.
             * The connect tone now runs BEFORE `active host=` -- before the
             * host's audio settings have arrived -- so these read whatever the
             * worker was created with. Configuring the pad to volume 0 makes a
             * perfectly paced tone inaudible, which is indistinguishable from
             * every other failure this hunt has produced.
             * ⭐ A confirmation nobody can hear is worse than a loud one, so an
             * absent or zero value means FULL rather than silent. */
            if (h > 0 && h <= 100) hp = (uint8_t)(h > 0x4fu ? 0x4fu : h);
            if (k > 0 && k <= 100) sp = (uint8_t)(k > 0x4fu ? 0x4fu : k);
        }
        cfg_hp = hp; cfg_sp = sp;
        uint8_t cfg[DS4SIG_CFG_LEN];
        ds4sig_build_configure(cfg, hp, sp);
        struct timespec ca, cb;
        clock_gettime(CLOCK_MONOTONIC, &ca);
        if (write(fd, cfg, sizeof cfg) != (ssize_t)sizeof cfg) ++failed;
        clock_gettime(CLOCK_MONOTONIC, &cb);
        cfg_ms = (long)((cb.tv_sec - ca.tv_sec) * 1000L +
                        (cb.tv_nsec - ca.tv_nsec) / 1000000L);
    }

    /* ⛔⛔ THE PACING CLOCK STARTS *AFTER* THE CONFIGURE WRITE, AND THAT IS
     * NOT A DETAIL.
     *
     * ⚠️ It started before, and build 413 measured the consequence:
     * `took 4068ms for 928ms of audio` while `writes took 81ms`. The configure
     * write had blocked for seconds, every one of the 116 deadlines had already
     * passed by the time the loop began, and ds4sig_wait_for -- which returns
     * at once when late, by design -- slept for none of them. So the whole tone
     * went out as a BURST in 81 ms.
     * ⭐ A decoder starved by slow writes and one drowned by a burst sound the
     * same from the outside: a click. Only the timings tell them apart. */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i + DS4SIG_FRAMES_PER_REPORT <= n; i += DS4SIG_FRAMES_PER_REPORT) {
        ds4sig_build(rep, counter, &frames[i]);
        counter = (uint16_t)(counter + DS4SIG_FRAMES_PER_REPORT);
        struct timespec wa, wb;
        clock_gettime(CLOCK_MONOTONIC, &wa);
        const ssize_t w = write(fd, rep, sizeof rep);
        clock_gettime(CLOCK_MONOTONIC, &wb);
        const long wms = (long)((wb.tv_sec - wa.tv_sec) * 1000L +
                                (wb.tv_nsec - wa.tv_nsec) / 1000000L);
        if (wms > worst_write_ms) worst_write_ms = wms;
        total_write_ms += wms;
        if (w == (ssize_t)sizeof rep) sent++;
        else failed++;
        reports++;
        ds4sig_wait_for(&t0, reports);
    }

    /* The decoder is warm only if the prime actually went out. */
    if (prime_frames > 0 && failed == 0) btsig_mark_primed(node);

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
                    "of audio (prime %d, configured); writes took %ldms, worst %ldms, configure %ldms (headphone=%u speaker=%u) -- node %s",
            what, sent, failed, took_ms, audio_ms, prime_frames,
            total_write_ms, worst_write_ms, cfg_ms, cfg_hp, cfg_sp,
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
    ctm_controller_set_type_state(c, 4 /* DS4_SLOT_HOST_SEEN */, 0);
    return ds4sig_play_fd(c->hid_fd, pattern, c, c->dev.path);
}

/* ⭐⭐ A TONE ON A NODE THAT NO SESSION OWNS -- and after tonight this is the
 * ONLY reliable way to play one.
 *
 * ⛔ MEASURED REPEATEDLY, 2026-09-22: a write to a pad that is BRIDGED blocks
 * for about five seconds, every time, and `worst 5103ms` / `worst 5119ms` /
 * `worst 5120ms` across builds says TIMEOUT rather than congestion. Halving the
 * write rate (0x14 to 0x17, 125 writes a second to 62) changed nothing. The
 * link is only free when no session is running on the pad -- which is exactly
 * why the refusal has been clean from the first attempt.
 *
 * ➡️ So a bridge tone is played HERE, before the session opens, rather than
 * from inside a session that makes it impossible. */
int ds4_signal_tone_node(const char *node, int pattern)
{
    if (!node || !node[0]) return -1;
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = ds4sig_play_fd(fd, pattern, NULL, node);
    close(fd);
    return rc;
}

/* Carries a node and a pattern to the light thread below. */
typedef struct {
    const char *node;
    int pattern;
    int rc;
} ds4sig_light_job_t;

static void *ds4sig_light_thread(void *arg)
{
    ds4sig_light_job_t *j = (ds4sig_light_job_t *) arg;
    j->rc = ds4_signal_light_pulse_node(j->node, j->pattern);
    return NULL;
}

int ds4_signal_refused_bt(const char *node)
{
    /* ⭐ ALL THREE AT ONCE, like the DualSense: red light, a felt pulse and
     * the sinking pair of notes, together rather than one after the other.
     *
     * ⛔ SEQUENTIAL WAS WRONG, and measuring said so. A DualSense carries
     * the light and the audio in ONE report, so its three signals are
     * simultaneous by construction. A DS4 needs two -- 0x11 for the light and
     * the motors, 0x14 for the audio -- and running them in turn made a
     * refusal 1.7 seconds long: 800ms of red, and only then the tone. Measured
     * on the rooted monitor 2026-09-23, the DS4's own BRIDGE already overlaps
     * them (a 701ms light inside a 929ms tone), so back-to-back was the odd one
     * out rather than the rule.
     *
     * ⓘ A thread, and its own fd: two writers to one hidraw node, each
     * write a whole report, which is what a bridge already does. ⚠ The
     * interleaving is the exact case T-238 broke on -- the light report used to
     * clear the pad's audio-valid bits and kill the tone -- so this is only
     * safe because ds4_bt_build_output carries the volumes now. If a refusal
     * ever goes silent again, look there first. */
    ds4sig_light_job_t job = { node, 2 /* BTSIG_REFUSED */, -1 };
    pthread_t th;
    const bool threaded = (pthread_create(&th, NULL, ds4sig_light_thread, &job) == 0);
    if (!threaded) {
        /* ⓘ No thread: still light it, just before the tone as it was. */
        job.rc = ds4_signal_light_pulse_node(node, 2);
    }
    const int trc = ds4_signal_tone_node(node, 2);
    if (threaded) pthread_join(th, NULL);
    ctl_log(NULL, "ds4sig: refusal light/pulse rc=%d (%s), tone rc=%d -- node %s",
            job.rc, threaded ? "alongside" : "before", trc, node ? node : "?");
    return trc;
}
