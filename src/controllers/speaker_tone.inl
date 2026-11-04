/* Two notes for a pad's own speaker, in whatever format its card takes.
 *
 * ⭐ WHY THIS EXISTS (2026-11-04). A cabled second-model DS4 has a sound card of
 * its own, 2 channels at 32 kHz, and rhoquinn8217 asked for its bridge to sound
 * as every other pad's does. The DualSense's renderers are tied to its card --
 * 4 channels at 48 kHz, with the felt pulse riding channels 2 and 3 -- so this
 * one is handed the format, and the notes, by its caller.
 *
 * ⓘ THE SPEAKER ONLY. A DS4's pulse is its motors, written as reports, so
 * nothing here goes past the first two channels.
 *
 * ⓘ Its own file so a test can pin it: no controller, no card, no device. */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The shape every pad's tone has: two notes with a gap between them, each
 * with an envelope that holds and then releases rather than fading away
 * (🔗 the note in ctm_feedback.inl on why). */
typedef struct {
    int hz[2];          /* the two notes, in the order they play */
    double level[2];    /* each note's lift: a small speaker rolls off low notes */
    int note_ms;
    int gap_ms;
    int peak;           /* a note at level 1.0, in sample units */
} speaker_tone_t;

/* Render `t` for a card of `channels` at `rate`. Returns a buffer the caller
 * frees, its length in frames in *frames_out; NULL when there is no format to
 * render into. */
static int16_t *speaker_tone_render(const speaker_tone_t *t, unsigned rate, unsigned channels,
                                    unsigned long *frames_out)
{
    if (frames_out) *frames_out = 0;
    if (!t || !frames_out || rate == 0 || channels == 0) return NULL;
    const unsigned long per = (unsigned long)rate * (unsigned long)t->note_ms / 1000ul;
    const unsigned long gap = (unsigned long)rate * (unsigned long)t->gap_ms / 1000ul;
    if (per == 0) return NULL;
    const unsigned long frames = per * 2 + gap;
    int16_t *buf = (int16_t *)calloc(frames * channels, sizeof(int16_t));
    if (!buf) return NULL;

    /* Left and right alike, or the one channel a mono card has. */
    const unsigned speakers = channels >= 2 ? 2 : 1;
    const unsigned long attack = per / 10;
    const unsigned long release = (per * 18) / 100;
    for (int n = 0; n < 2; ++n) {
        const unsigned long base = (unsigned long)n * (per + gap);
        for (unsigned long i = 0; i < per; ++i) {
            double env;
            if (i < attack)             env = (double)i / (double)attack;
            else if (i > per - release) env = (double)(per - i) / (double)release;
            else                        env = 1.0;
            double s = sin(2.0 * M_PI * t->hz[n] * ((double)i / (double)rate))
                     * env * t->level[n] * t->peak;
            if (s > 32767.0) s = 32767.0;
            if (s < -32768.0) s = -32768.0;
            for (unsigned ch = 0; ch < speakers; ++ch)
                buf[(base + i) * channels + ch] = (int16_t)s;
        }
    }
    *frames_out = frames;
    return buf;
}
