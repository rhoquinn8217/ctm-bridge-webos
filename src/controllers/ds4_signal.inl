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

/* ⭐⭐ WARM, NOT "PRIMED ONCE". A DS4's decoder COOLS.
 *
 * ⛔ The old rule was a permanent flag: long prime the first time a node is
 * seen, short prime forever after. That is wrong about the hardware, and this
 * file already said so before it was measured -- *"Primed once is not a
 * property that LASTS on a DS4."*
 *
 * ⚠️ MEASURED ON AN LG OLED83B4PUA, build 435, 2026-09-23. The same pad,
 * the same short prime (150) and `116 sent, 0 failed` EVERY time. What changed
 * was only how long the pad had been silent beforehand:
 *
 *     idle ~2s    495 Hz peak ~2850, spread 1%,  both notes 5 of 5
 *     idle ~15s   495 Hz peak ~2700,             both notes 5 of 5
 *     idle ~30s   495 Hz peak ~1500,             both notes 9 of 10
 *
 * ⭐ So a cold decoder does not simply drop a note -- it plays it at about
 * HALF level, and sometimes not at all. Reliability tracks time since the last
 * tone and nothing else we control.
 *
 * ➡️ Hence a TIMESTAMP rather than a flag: the short prime only when this
 * pad played a tone within DS4SIG_WARM_MS, the long one otherwise. A person
 * bridging and releasing in one gesture pays the short lead-in; a refusal out
 * of the blue pays the long one, which is when it is needed.
 * ⓘ Torn reads are possible without a lock and are harmless: a miss picks
 * the LONG prime, which is the safe way to be wrong. */
#define DS4SIG_WARM_MS   5000
#define DS4SIG_WARM_MAX  8

static struct { char key[40]; long long at_ms; } g_ds4sig_warm[DS4SIG_WARM_MAX];
static int g_ds4sig_warm_n;

static long long ds4sig_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000LL + (long long)(t.tv_nsec / 1000000L);
}

static bool ds4sig_is_warm(const char *key)
{
    if (!key || !key[0]) return false;   /* unknown: prime long */
    const long long now = ds4sig_now_ms();
    for (int i = 0; i < g_ds4sig_warm_n; ++i) {
        if (strcmp(g_ds4sig_warm[i].key, key) == 0) {
            return (now - g_ds4sig_warm[i].at_ms) <= DS4SIG_WARM_MS;
        }
    }
    return false;
}

static void ds4sig_mark_warm(const char *key)
{
    if (!key || !key[0]) return;
    const long long now = ds4sig_now_ms();
    for (int i = 0; i < g_ds4sig_warm_n; ++i) {
        if (strcmp(g_ds4sig_warm[i].key, key) == 0) { g_ds4sig_warm[i].at_ms = now; return; }
    }
    if (g_ds4sig_warm_n < DS4SIG_WARM_MAX) {
        snprintf(g_ds4sig_warm[g_ds4sig_warm_n].key,
                 sizeof g_ds4sig_warm[0].key, "%s", key);
        g_ds4sig_warm[g_ds4sig_warm_n].at_ms = now;
        ++g_ds4sig_warm_n;
    }
}

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

/* ⭐⭐ THE COMBINED REPORT: STATE AND AUDIO IN ONE WRITE (0x15).
 *
 * ⛔ WHY, and it is the fault this whole ticket kept circling. We were using
 * TWO reports -- 0x11 for the light, the motors and the volumes, 0x14 for the
 * audio -- and every way of arranging them was wrong. Sequential makes a
 * refusal 1.7 s long. Concurrent, on a second fd, ate the first note two thirds
 * of the time on an OLED83B4PUA. Neither is fixable by timing, because the
 * problem is that there are two writers at all.
 *
 * ⭐ 0x15 has both: `EnableHID` AND `EnableAudio` set, carrying the same
 * state block as 0x11 followed by an audio block. So the lightbar, the motors,
 * the volumes and the SBC frames go out in ONE report, the way a DualSense's
 * do, which is why the DualSense path here has always been the reliable one.
 *
 * ⓘ Sourced from SensePost's dual-pod-shock, a working DS4 audio streamer
 * (github.com/sensepost/dual-pod-shock), corroborated by the DS4-BT structure
 * tables. ⚠️ The state block MIRRORS 0x11 -- flags at 3, motors at 6 and 7,
 * RGB at 8 to 10, volumes at 21, 22 and 24 -- so those offsets are written as
 * literals here rather than reusing ds4_report.inl's names, which belong to
 * another translation unit.
 *
 * ⭐ AND THE VOLUMES RIDE EVERY REPORT. The old design configured the pad
 * ONCE and then streamed for up to two seconds. Nothing re-established the
 * audio-valid bits if the pad let them lapse -- and this ticket's very first
 * fault was a light report CLEARING those bits. Asserting them every 8 ms costs
 * nothing and removes the whole class.
 *
 * ⓘ 249 bytes are free from the payload offset, so two 109-byte frames fit
 * with room to spare: the same two per report as 0x14, no throughput change. */
#define DS4SIG_COMBO_ID       0x15
#define DS4SIG_COMBO_LEN      334
#define DS4SIG_COMBO_COUNTER  78
#define DS4SIG_COMBO_ROUTE    80
#define DS4SIG_COMBO_PAYLOAD  81

static void ds4sig_build_combined(uint8_t *out, uint16_t counter,
                                  const uint8_t *const *f,
                                  uint8_t claims, uint8_t motor,
                                  uint8_t r, uint8_t g, uint8_t b,
                                  uint8_t hp, uint8_t sp)
{
    memset(out, 0, DS4SIG_COMBO_LEN);
    out[0] = DS4SIG_COMBO_ID;
    out[1] = 0xc0;                 /* HID bit ON: this report carries state too */
    out[2] = 0xa0;
    out[3] = (uint8_t)(claims | 0xb0u);   /* volume-valid bits, always */
    out[21] = hp;
    out[22] = hp;
    out[24] = sp;
    if (claims & 0x01u) { out[6] = motor; out[7] = motor; }   /* DS4_OUT_MOTORS */
    if (claims & 0x02u) { out[8] = r; out[9] = g; out[10] = b; } /* DS4_OUT_LIGHT */
    out[DS4SIG_COMBO_COUNTER]     = (uint8_t)(counter & 0xff);
    out[DS4SIG_COMBO_COUNTER + 1] = (uint8_t)(counter >> 8);
    out[DS4SIG_COMBO_ROUTE]       = DS4SIG_ROUTE;
    for (int i = 0; i < DS4SIG_FRAMES_PER_REPORT; ++i) {
        memcpy(out + DS4SIG_COMBO_PAYLOAD + (size_t)i * DS4SIG_FRAME_BYTES,
               f[i], DS4SIG_FRAME_BYTES);
    }
    ctm_bt_sign_output(out, DS4SIG_COMBO_LEN);
}

/* What to draw into the audio stream while it plays. ⓘ `on` false means the
 * plain 0x14 audio-only report, exactly as before. */
typedef struct {
    int      on;
    uint8_t  r, g, b;
    int      breaths;
    int      solid;
    long     ms;        /* how long the light lasts, from the first TONE report */
    int      rumble;
} ds4sig_visual_t;

#define DS4SIG_VIS_PULSE_MS   250
#define DS4SIG_VIS_PULSE_LVL  0x7f

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
static int ds4sig_play_fd(int fd, int pattern, ctm_controller_t *log_to, const char *node,
                          const ds4sig_visual_t *vis)
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
    /* ⛔⛔ ALWAYS THE LONG PRIME. THE SHORT ONE IS THE LAST FAULT (2026-09-23).
     *
     * ⚠️ Yes, this file records "always long" being tried on 2026-09-22 and
     * reverted. That reading was confounded: it predated the 0x15 combined
     * report, so a second writer was eating notes at the same time, and it was
     * judged without measuring each note. Both of those are fixed now, and the
     * evidence this time is a rate rather than an impression:
     *
     *     signals ~30s apart, prime 450   release 15/15, bridge 13/15
     *     signals ~7s apart,  prime 450   10 of 10
     *     signals ~1-2s apart, prime 150  a silent handback and a bridge
     *                                     that lost its second note, in 3 tries
     *
     * ⭐ rhoquinn8217's own ten-signal run is what found it. Their two
     * silences came in a sequence fired 5 to 6 seconds apart, which is exactly
     * where ds4sig_is_warm handed out the short prime -- and their ears caught a
     * silent RELEASE that a 15-run harness at 30-second spacing had scored 15/15,
     * because that spacing never took the short path at all.
     *
     * ⓘ The warm table stays, and is still marked, because it costs nothing
     * and it is the honest record of when this pad last played. It just no
     * longer decides the prime. ➡️ Every tone is 2.1 s now instead of 0.93 s.
     * Correct and slow beats fast and silent, which this file said from the
     * start. 🔗 DS4SIG_WARM_MS if a short path is ever wanted back: the one
     * condition it measured well in was a REFUSAL 4 s after another refusal
     * (5 of 5, 1% spread), so the gate would need to be far tighter than 5 s and
     * to know which signal it is. */
    const int prime_frames = DS4SIG_PRIME_FRAMES * 3;
    (void) ds4sig_is_warm;

    /* The gap is silence rather than nothing, because the decoder wants a
     * stream rather than a pause. */
    const uint8_t *frames[(DS4SIG_PRIME_FRAMES * 3) + DS4SIG_TONE_FRAMES +
                          DS4SIG_GAP_FRAMES + DS4SIG_TONE_FRAMES];
    int n = 0;
    for (int i = 0; i < prime_frames; i++) frames[n++] = g_ds4sig_silence;
    for (int i = 0; i < DS4SIG_TONE_FRAMES;  i++) frames[n++] = first[i];
    for (int i = 0; i < DS4SIG_GAP_FRAMES;   i++) frames[n++] = g_ds4sig_silence;
    for (int i = 0; i < DS4SIG_TONE_FRAMES;  i++) frames[n++] = second[i];

    /* ⓘ One buffer for either report: 0x15 is the larger of the two. */
    uint8_t rep[DS4SIG_COMBO_LEN];
    const int combo = (vis != NULL && vis->on) ? 1 : 0;
    const size_t rep_len = combo ? (size_t)DS4SIG_COMBO_LEN : (size_t)DS4SIG_REPORT_LEN;
    /* ⭐ The light starts when the NOTES do, not during the prime: the prime is
     * silence, and a red flash over silence would arrive before the sound. */
    const int prime_reports = prime_frames / DS4SIG_FRAMES_PER_REPORT;
    int vis_done = 0;
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
        if (combo) {
            /* Where the light is in its pattern, measured from the first tone
             * report rather than from the start of the prime. */
            const long at = (long)(reports - prime_reports) * (DS4SIG_PACE_US / 1000);
            uint8_t claims = 0;
            uint8_t motor = 0, lr = 0, lg = 0, lb = 0;
            if (reports >= prime_reports && !vis_done) {
                if (at >= vis->ms) {
                    vis_done = 1;               /* one last dark report below */
                } else {
                    const int level = vis->solid ? 255
                                                 : ds4_signal_breath(at, vis->ms, vis->breaths);
                    claims = 0x02u;             /* DS4_OUT_LIGHT */
                    lr = (uint8_t)((vis->r * level) / 255);
                    lg = (uint8_t)((vis->g * level) / 255);
                    lb = (uint8_t)((vis->b * level) / 255);
                    if (vis->rumble && at < DS4SIG_VIS_PULSE_MS) {
                        claims |= 0x01u;        /* DS4_OUT_MOTORS */
                        motor = DS4SIG_VIS_PULSE_LVL;
                    } else if (vis->rumble) {
                        claims |= 0x01u;        /* claim it to send the STOP */
                        motor = 0;
                    }
                }
            }
            ds4sig_build_combined(rep, counter, &frames[i], claims, motor,
                                  lr, lg, lb, cfg_hp ? (uint8_t)cfg_hp : 0x4f,
                                  cfg_sp ? (uint8_t)cfg_sp : 0x4f);
        } else {
            ds4sig_build(rep, counter, &frames[i]);
        }
        counter = (uint16_t)(counter + DS4SIG_FRAMES_PER_REPORT);
        struct timespec wa, wb;
        clock_gettime(CLOCK_MONOTONIC, &wa);
        const ssize_t w = write(fd, rep, rep_len);
        clock_gettime(CLOCK_MONOTONIC, &wb);
        const long wms = (long)((wb.tv_sec - wa.tv_sec) * 1000L +
                                (wb.tv_nsec - wa.tv_nsec) / 1000000L);
        if (wms > worst_write_ms) worst_write_ms = wms;
        total_write_ms += wms;
        if (w == (ssize_t)rep_len) sent++;
        else failed++;
        reports++;
        ds4sig_wait_for(&t0, reports);
    }

    /* ⛔ THE PULSE AND THE LIGHT ALWAYS END WITH A STOP, however the stream
     * ended -- a DS4's motors keep running until a report says otherwise, and
     * nothing owns the lightbar after a refusal. ⓘ One more report of silence
     * costs 8 ms and cannot leave a pad buzzing in the dark. */
    if (combo) {
        const uint8_t *quiet[DS4SIG_FRAMES_PER_REPORT];
        for (int q = 0; q < DS4SIG_FRAMES_PER_REPORT; ++q) quiet[q] = g_ds4sig_silence;
        ds4sig_build_combined(rep, counter, quiet,
                              (uint8_t)(0x01u | 0x02u), 0, 0, 0, 0,
                              cfg_hp ? (uint8_t)cfg_hp : 0x4f,
                              cfg_sp ? (uint8_t)cfg_sp : 0x4f);
        if (write(fd, rep, rep_len) == (ssize_t)rep_len) sent++; else failed++;
    }

    /* The decoder is warm only if the prime actually went out. */
    if (prime_frames > 0 && failed == 0) ds4sig_mark_warm(node);

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
    return ds4sig_play_fd(c->hid_fd, pattern, c, c->dev.path, NULL);
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
/* ⛔ ONE TONE AT A TIME. ds4_signal_tone_node opens the node and streams for
 * two seconds with no claim on the controller, so nothing stopped two of them
 * overlapping -- an auto-bridge firing into a handback, or a person bridging and
 * releasing faster than a tone lasts. Two audio streams interleaved on one
 * hidraw node is the same fault as a light report cutting in, and it produced a
 * silent handback in 1 of 3 forced attempts on 2026-09-23.
 * ⓘ A plain mutex rather than a try-lock: a confirmation that waits its turn
 * is still a confirmation, and both tones are wanted. ⚠️ Held across the
 * whole play, so it must never be taken by anything that can block on the pad. */
static pthread_mutex_t g_ds4sig_tone_lock = PTHREAD_MUTEX_INITIALIZER;

int ds4_signal_tone_node(const char *node, int pattern)
{
    /* ⭐⭐ THE LIGHT AND THE PULSE TRAVEL WITH THE TONE, on every pattern.
     *
     * ⚠️ MEASURED ON AN OLED83B4PUA, build 437, ten runs in a verified-quiet
     * room: the RELEASE was clean 10 of 10 and the BRIDGE lost its second note
     * 2 of 10. rhoquinn8217 called where to look -- *"test bridge tones while
     * the controller is bridging because that's when a lot of things are
     * happening"* -- and it is the same two-writer fault as the refusal: the
     * tone goes out as 0x14 from ui_bridge.c while the controller writes its
     * 700ms green as 0x11 from another thread.
     * ➡️ One report carries both now, so there is nothing to race.
     *
     * ⛔ The controller's own light is SUPPRESSED for a Bluetooth DS4 to
     * match (🔗 ds4_signal_connected). Leaving it would draw the pattern
     * twice from two writers, which is the fault, not a belt and braces. */
    if (!node || !node[0]) return -1;
    /* ⭐⭐ THE HANDBACK CARRIES ITS LIGHT NOW TOO (2026-09-23).
     * rhoquinn8217: *"try to get the tone to sound earlier if possible so that
     * the tone plays at the same time the light pattern starts."*
     * ➡️ They meet in the middle. The tone moved EARLIER -- ui_bridge.c plays it
     * before BRIDGE_STOP rather than after, which is a network round trip the
     * pad's link has nothing to do with -- and the light moved from the release
     * REQUEST into these reports. ⓘ Simultaneous by construction, not by timing.
     * ⚠️ The visible answer to a release therefore lands a little later than it
     * used to; that was the trade rhoquinn8217 asked for. 🔗 ds4_signal_unplugging,
     * which no longer draws it on Bluetooth. */
    const int lit = 1;
    ds4sig_visual_t vis;
    memset(&vis, 0, sizeof vis);
    if (lit) {
        vis.on = 1;
        vis.rumble = 1;
        int solid = 0;
        ds4_signal_shape_of(pattern, &vis.r, &vis.g, &vis.b, &vis.breaths, &solid, &vis.ms);
        vis.solid = solid;
    }
    pthread_mutex_lock(&g_ds4sig_tone_lock);
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) { pthread_mutex_unlock(&g_ds4sig_tone_lock); return -1; }
    int rc = ds4sig_play_fd(fd, pattern, NULL, node, lit ? &vis : NULL);
    close(fd);
    pthread_mutex_unlock(&g_ds4sig_tone_lock);
    return rc;
}

int ds4_signal_refused_bt(const char *node)
{
    /* ⭐⭐ ALL THREE IN ONE STREAM. The light, the pulse and the sinking pair
     * of notes now ride the SAME reports (0x15), so they are simultaneous by
     * construction and there is no second writer to race.
     *
     * ⛔ Both earlier arrangements are recorded because both were wrong.
     * Sequential (light finishes, then the tone) made a refusal 1.7 s long.
     * Concurrent on its own thread and fd ate the first note two thirds of the
     * time on an OLED83B4PUA, measured over 20 runs. ➡️ Neither is a timing
     * problem: two writers to one hidraw node is the problem.
     *
     * ⓘ A refusal has no controller, so the shape comes across the TU
     * boundary from ds4_signal_shape_of. 🔗 ds4sig_build_combined for the
     * layout and where it is sourced from. */
    ds4sig_visual_t vis;
    memset(&vis, 0, sizeof vis);
    vis.on = 1;
    vis.rumble = 1;
    int solid = 0;
    ds4_signal_shape_of(2 /* BTSIG_REFUSED */, &vis.r, &vis.g, &vis.b,
                        &vis.breaths, &solid, &vis.ms);
    vis.solid = solid;

    if (!node || !node[0]) return -1;
    pthread_mutex_lock(&g_ds4sig_tone_lock);   /* 🔗 one tone at a time */
    const int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) { pthread_mutex_unlock(&g_ds4sig_tone_lock); return -1; }
    const int rc = ds4sig_play_fd(fd, 2 /* BTSIG_REFUSED */, NULL, node, &vis);
    close(fd);
    pthread_mutex_unlock(&g_ds4sig_tone_lock);
    ctl_log(NULL, "ds4sig: refusal all-in-one rc=%d -- node %s", rc, node ? node : "?");
    return rc;
}
