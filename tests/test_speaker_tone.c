/* Tests for the two notes played on a pad's own speaker (speaker_tone.inl).
 *
 * ⭐ WHY THIS EXISTS (2026-11-04). A cabled second-model DS4 plays the bridge
 * tone on its own sound card, 2 channels at 32 kHz, where the DualSense's
 * renderers know only its own 4 channels at 48 kHz. These pin what the notes
 * must be on any card: their order (rising for a bridge, falling for a
 * handback), their length, the silence between them, and that nothing lands
 * past the speaker's two channels.
 *
 * ⛔ Nothing here needs a television, a controller or a sound card.
 *
 * ➡️ Build and run:  cc tests/test_speaker_tone.c -lm && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <stdlib.h>

#include "../src/controllers/speaker_tone.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* The notes every pad plays (🔗 wired_sig_hz and wired_sig_level). */
static speaker_tone_t tone(int first, int second)
{
    speaker_tone_t t;
    t.hz[0] = first;
    t.hz[1] = second;
    t.level[0] = first == 990 ? 0.95 : 1.25;
    t.level[1] = second == 990 ? 0.95 : 1.25;
    t.note_ms = 140;
    t.gap_ms = 90;
    t.peak = 9000;
    return t;
}

/* A note's pitch, from its sign changes on one channel. */
static double pitch(const int16_t *buf, unsigned channels, unsigned ch,
                    unsigned long from, unsigned long len, unsigned rate)
{
    int last = 0;
    unsigned long changes = 0;
    for (unsigned long i = from; i < from + len; ++i) {
        const int s = buf[i * channels + ch];
        const int sign = s > 0 ? 1 : s < 0 ? -1 : 0;
        if (sign != 0 && last != 0 && sign != last) ++changes;
        if (sign != 0) last = sign;
    }
    return (double)changes / 2.0 / ((double)len / (double)rate);
}

static int near(double got, double want)
{
    return got > want * 0.97 && got < want * 1.03;
}

static int peak_of(const int16_t *buf, unsigned channels, unsigned ch,
                   unsigned long from, unsigned long len)
{
    int p = 0;
    for (unsigned long i = from; i < from + len; ++i) {
        int s = buf[i * channels + ch];
        if (s < 0) s = -s;
        if (s > p) p = s;
    }
    return p;
}

int main(void)
{
    const unsigned rate = 32000;
    const unsigned long per = 32000ul * 140ul / 1000ul;   /* 4480 */
    const unsigned long gap = 32000ul * 90ul / 1000ul;    /* 2880 */

    printf("\na second-model DS4's card: 2 channels at 32 kHz\n");
    speaker_tone_t rising = tone(660, 990);
    unsigned long frames = 0;
    int16_t *buf = speaker_tone_render(&rising, rate, 2, &frames);
    ok(buf != NULL, "rendered");
    if (!buf) return 1;
    ok(frames == per * 2 + gap, "two notes of 140 ms and a gap of 90 ms: 11840 frames");
    ok(near(pitch(buf, 2, 0, 0, per, rate), 660.0), "the first note is the low one");
    ok(near(pitch(buf, 2, 0, per + gap, per, rate), 990.0), "and the second the high one: rising");
    ok(peak_of(buf, 2, 0, per, gap) == 0, "the gap between them is silent");
    int same = 1;
    for (unsigned long i = 0; i < frames; ++i)
        if (buf[i * 2] != buf[i * 2 + 1]) { same = 0; break; }
    ok(same, "left and right are the same");
    ok(buf[0] == 0, "it starts from silence, with no click");
    ok(peak_of(buf, 2, 0, per - 20, 20) < 9000 / 10, "the first note releases before the gap");
    ok(peak_of(buf, 2, 0, frames - 20, 20) < 9000 / 10, "and the second before the end");
    const int loudest = peak_of(buf, 2, 0, 0, frames);
    ok(loudest > 9000 && loudest <= (int)(9000 * 1.25) + 1, "as loud as the other pads' tone: the low note lifted, nothing clipped");
    free(buf);

    printf("\na handback falls\n");
    speaker_tone_t falling = tone(990, 660);
    buf = speaker_tone_render(&falling, rate, 2, &frames);
    ok(buf != NULL, "rendered");
    if (!buf) return 1;
    ok(near(pitch(buf, 2, 1, 0, per, rate), 990.0), "high first");
    ok(near(pitch(buf, 2, 1, per + gap, per, rate), 660.0), "then low");
    free(buf);

    printf("\nthe speaker's channels only\n");
    buf = speaker_tone_render(&rising, 48000, 4, &frames);
    ok(buf != NULL, "a four-channel card is rendered");
    if (!buf) return 1;
    ok(frames == 48000ul * 140ul / 1000ul * 2 + 48000ul * 90ul / 1000ul, "its length follows its rate");
    ok(peak_of(buf, 4, 0, 0, frames) > 0, "the notes are on channel 0");
    ok(peak_of(buf, 4, 2, 0, frames) == 0 && peak_of(buf, 4, 3, 0, frames) == 0,
       "and nothing on channels 2 and 3, a DualSense's haptics");
    free(buf);
    buf = speaker_tone_render(&rising, 16000, 1, &frames);
    ok(buf != NULL && peak_of(buf, 1, 0, 0, frames) > 0, "a mono card gets the notes on its one channel");
    free(buf);

    printf("\nno format, no notes\n");
    ok(speaker_tone_render(&rising, 0, 2, &frames) == NULL && frames == 0, "no rate");
    ok(speaker_tone_render(&rising, 32000, 0, &frames) == NULL && frames == 0, "no channels");
    ok(speaker_tone_render(NULL, 32000, 2, &frames) == NULL, "no tone");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
