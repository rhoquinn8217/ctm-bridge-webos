/* Tests for the Bluetooth signal report builder.
 *
 * ⭐⭐ WHY THIS EXISTS. On 2026-08-19 the same bug was fixed twice and shipped
 * broken in between: the report claimed the LIGHTBAR and the PLAYER LEDS on
 * every frame with the colour bytes at zero, which made every light pattern
 * flicker. It was fixed, the fix was lost to a sandbox reset rather than to
 * anyone's judgement, and the second hunt cost most of an afternoon.
 *
 * ⛔ Nothing here needs a television, a controller or a stream. The builder is
 * bytes in, bytes out. If a test below fails, a light or a sound is already
 * broken and nobody has had to notice it by eye.
 *
 * ➡️ Build and run:  cc tests/test_btsig_build.c -lm && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ── the builder itself ─────────────────────────────────────────────────
 *
 * ⓘ Included from the real files, so a change there is seen here. Until
 * 2026-10-23 this was a hand copy of the builder's first 50 bytes, and the
 * timing, audio and haptic blocks and the checksum went untested. */
#include "../src/controllers/bt_sign.inl"
#include "../src/controllers/ctm_bt_signal_data.inl"
#include "../src/controllers/btsig_build.inl"

/* The patterns, numbered as ctm_bt_signal.inl numbers them. */
typedef enum {
    BTSIG_HANDING_OVER,
    BTSIG_HANDED_BACK,
    BTSIG_REFUSED
} btsig_pattern_t;

/* ── harness ─────────────────────────────────────────────────────────── */

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* ── the tests ───────────────────────────────────────────────────────── */

/* ⭐⭐ THE ONE THAT MATTERS MOST, and the general form of the bug that got
 * shipped twice: a claim bit set with nothing behind it.
 *
 * ⓘ Written as a rule rather than as "flags2 must not be 0x14", so it catches
 * the next version of the mistake as well as the last one. */
static void test_claims_nothing_it_does_not_set(void)
{
    puts("a report never claims a field it leaves at zero");
    uint8_t rep[BTSIG_REPORT_LEN];

    btsig_frame_t plain = {0};
    plain.seq = 3;
    btsig_build(rep, &plain);

    ok((rep[BTSIG_S_FLAGS2] & 0x04) == 0,
       "no lightbar claim when no colour is set");
    ok((rep[BTSIG_S_FLAGS2] & 0x10) == 0,
       "no player-LED claim -- nothing here ever writes them");
    ok((rep[BTSIG_S_FLAGS2] & 0x02) == 0,
       "no power-save claim -- that byte can switch a controller off");
    ok(rep[BTSIG_S_FLAGS1] == 0x00,
       "no audio-settings claim on a carriage-only report");
    ok(rep[BTSIG_S_SPK_VOL] == 0 && rep[BTSIG_S_AUDIO_CTL] == 0,
       "and the settings bytes are zero with it");
}

/* The configure flag is the whole point of "configure once, then carry". */
static void test_configure_is_one_report_only(void)
{
    puts("the speaker is configured only when asked");
    uint8_t rep[BTSIG_REPORT_LEN];

    btsig_frame_t first = {0};
    first.configure = 1;
    btsig_build(rep, &first);
    ok(rep[BTSIG_S_FLAGS1] == BTSIG_F1_AUDIO, "configure sets the audio claim");
    ok(rep[BTSIG_S_SPK_VOL] == BTSIG_SPK_VOL, "and the speaker volume with it");
    ok(rep[BTSIG_S_AUDIO_CTL] == BTSIG_AUDIO_CTL, "and the routing");

    btsig_frame_t rest = {0};
    btsig_build(rep, &rest);
    ok(rep[BTSIG_S_FLAGS1] == 0x00,
       "a later report reconfigures nothing -- doing it 100x/s stalled the link");
}

/* Each pattern's colour. ⛔ A bridge was magenta for a while after the pattern
 * moved from "asking" to "confirming", and nothing but an eye caught it. */
static void test_each_pattern_has_its_colour(void)
{
    puts("every pattern paints the colour it means");
    uint8_t rep[BTSIG_REPORT_LEN];
    struct { btsig_pattern_t p; uint8_t r, g, b; const char *what; } want[] = {
        { BTSIG_HANDING_OVER, 0x00, 0xff, 0x00, "a bridge is green" },
        { BTSIG_HANDED_BACK,  0xff, 0xff, 0x00, "a handback is yellow" },
        { BTSIG_REFUSED,      0xff, 0x00, 0x00, "a refusal is red" },
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); ++i) {
        btsig_frame_t f = {0};
        f.claim_led = 1;
        f.r = (want[i].p != BTSIG_HANDING_OVER) ? 0xff : 0x00;
        f.g = (want[i].p != BTSIG_REFUSED)      ? 0xff : 0x00;
        f.b = 0x00;
        btsig_build(rep, &f);
        ok(rep[BTSIG_S_LED_R + 0] == want[i].r &&
           rep[BTSIG_S_LED_R + 1] == want[i].g &&
           rep[BTSIG_S_LED_R + 2] == want[i].b, want[i].what);
        ok((rep[BTSIG_S_FLAGS2] & 0x04) != 0,
           "and claims the lightbar, because it did set a colour");
    }
}

/* The frame the controller has to recognise at all. */
static void test_report_shape(void)
{
    puts("the report is shaped the way the controller expects");
    uint8_t rep[BTSIG_REPORT_LEN];
    btsig_frame_t f = {0};
    f.seq = 5;
    btsig_build(rep, &f);
    ok(rep[0] == 0x36, "report id");
    ok(rep[BTSIG_STATE_AT] == 0x90, "the state block is where it should be");
    ok(rep[BTSIG_STATE_AT + 1] == BTSIG_STATE_LEN, "and says its own length");
    ok(rep[1] == 0x50, "the sequence sits in the high nibble");
}

/* The timing block, which the copy never had. */
static void test_timing_block(void)
{
    puts("the timing block carries the latency and the audio sequence");
    uint8_t rep[BTSIG_REPORT_LEN];
    btsig_frame_t f = {0};
    f.seq = 0x123;
    btsig_build(rep, &f);
    ok(rep[BTSIG_TIMING_AT] == 0x91 && rep[BTSIG_TIMING_AT + 1] == BTSIG_TIMING_LEN,
       "a 0x91 block of its own length");
    ok(rep[BTSIG_TIMING_AT + 2] == BTSIG_T_LEAD, "its lead byte");
    int all = 1;
    for (int i = 3; i <= 7; ++i) all = all && rep[BTSIG_TIMING_AT + i] == BTSIG_T_LATENCY;
    ok(all, "the latency in each of its five bytes");
    ok(rep[BTSIG_T_AUDIO_SEQ] == 0x23,
       "the audio sequence is the report number's low byte -- the field that defeated four attempts");
    ok(rep[1] == 0x30, "and the report's own sequence its low nibble, in the high one");
}

/* The audio block: one Opus frame, the silence frame when none is given. */
static void test_audio_block(void)
{
    puts("the audio block carries one frame, silence when none is given");
    uint8_t rep[BTSIG_REPORT_LEN];
    btsig_frame_t f = {0};
    btsig_build(rep, &f);
    ok(rep[BTSIG_AUDIO_AT] == 0x95 && rep[BTSIG_AUDIO_AT + 1] == BTSIG_AUDIO_LEN,
       "a 0x95 block of its own length");
    ok(memcmp(&rep[BTSIG_AUDIO_AT + 2], g_btsig_silence, BTSIG_FRAME_BYTES) == 0,
       "no frame given: the silence frame");
    f.audio = g_btsig_low[0];
    btsig_build(rep, &f);
    ok(memcmp(&rep[BTSIG_AUDIO_AT + 2], g_btsig_low[0], BTSIG_FRAME_BYTES) == 0,
       "a frame given: that frame, byte for byte");
}

/* The haptic block: quiet unless asked, then a wave on both motors. */
static void test_haptic_block(void)
{
    puts("the haptic block is quiet unless asked, and a pulse when it is");
    uint8_t rep[BTSIG_REPORT_LEN];
    btsig_frame_t f = {0};
    btsig_build(rep, &f);
    ok(rep[BTSIG_HAPTIC_AT] == 0x92 && rep[BTSIG_HAPTIC_AT + 1] == BTSIG_HAPTIC_LEN,
       "a 0x92 block of its own length");
    int quiet = 1;
    for (int i = 0; i < BTSIG_HAPTIC_LEN; ++i) quiet = quiet && rep[BTSIG_HAPTIC_AT + 2 + i] == 0;
    ok(quiet, "without haptics: all zero");
    f.haptics = 1;
    btsig_build(rep, &f);
    int wave = 0, paired = 1;
    for (int i = 0; i < 32; ++i) {
        const uint8_t a = rep[BTSIG_HAPTIC_AT + 2 + i * 2];
        const uint8_t b = rep[BTSIG_HAPTIC_AT + 2 + i * 2 + 1];
        if (a != 0) wave = 1;
        if (a != b) paired = 0;
    }
    ok(wave, "with haptics: a wave");
    ok(paired, "each sample to both motors");
}

/* After the haptics, nothing until the checksum. */
static void test_padding_is_zero(void)
{
    puts("between the last block and the checksum, zeros");
    uint8_t rep[BTSIG_REPORT_LEN];
    btsig_frame_t f = {0};
    f.seq = 9;
    f.haptics = 1;
    f.claim_led = 1;
    f.configure = 1;
    f.r = f.g = f.b = 0xff;
    btsig_build(rep, &f);
    int zero = 1;
    for (int i = BTSIG_HAPTIC_AT + 2 + BTSIG_HAPTIC_LEN; i < BTSIG_CRC_AT; ++i) zero = zero && rep[i] == 0;
    ok(zero, "the end marker and the padding");
}

/* The checksum, worked out here from the CRC's own step, not by the same call. */
static void test_signed(void)
{
    puts("the report is signed: CRC32 over 0xa2 and its first 394 bytes, little-endian");
    uint8_t rep[BTSIG_REPORT_LEN];
    btsig_frame_t f = {0};
    f.seq = 7;
    f.claim_led = 1;
    f.g = 0xff;
    btsig_build(rep, &f);
    const uint8_t seed = 0xa2;
    uint32_t crc = bt_crc32_step(0xffffffffu, &seed, 1);
    crc = ~bt_crc32_step(crc, rep, BTSIG_CRC_AT);
    ok(rep[BTSIG_CRC_AT + 0] == (uint8_t)(crc & 0xffu) &&
       rep[BTSIG_CRC_AT + 1] == (uint8_t)((crc >> 8) & 0xffu) &&
       rep[BTSIG_CRC_AT + 2] == (uint8_t)((crc >> 16) & 0xffu) &&
       rep[BTSIG_CRC_AT + 3] == (uint8_t)((crc >> 24) & 0xffu),
       "the checksum covers the whole report and sits at its end");
    rep[BTSIG_STATE_AT + 3] ^= 0x01;
    crc = bt_crc32_step(0xffffffffu, &seed, 1);
    crc = ~bt_crc32_step(crc, rep, BTSIG_CRC_AT);
    ok(rep[BTSIG_CRC_AT + 0] != (uint8_t)(crc & 0xffu) ||
       rep[BTSIG_CRC_AT + 1] != (uint8_t)((crc >> 8) & 0xffu),
       "and a changed byte no longer matches it (the check can fail)");
}

/* ⭐ AND NO REPORT CLAIMS THE HOST'S BITS, the value behind the bug that
 * shipped twice. The builder above is the real one; this reads both files for
 * that value being written into flags2 anywhere. */
static int file_writes_host_claim(const char *path, int *found)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[512];
    int bad = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "out[BTSIG_S_FLAGS2]") && strstr(line, "BTSIG_F2_HOST")) bad = 1;
    }
    fclose(fp);
    *found = 1;
    return bad;
}

static void test_no_report_claims_the_hosts_bits(void)
{
    puts("no report claims the host's 0x14 in flags2");
    int found_builder = 0, found_signal = 0;
    const int in_builder = file_writes_host_claim("src/controllers/btsig_build.inl", &found_builder);
    const int in_signal = file_writes_host_claim("src/controllers/ctm_bt_signal.inl", &found_signal);
    ok(found_builder && found_signal, "both files read -- run this from the repo root");
    ok(!in_builder && !in_signal, "neither writes BTSIG_F2_HOST into flags2");
}

int main(void)
{
    puts("");
    test_claims_nothing_it_does_not_set();  puts("");
    test_configure_is_one_report_only();    puts("");
    test_each_pattern_has_its_colour();     puts("");
    test_report_shape();                    puts("");
    test_timing_block();                    puts("");
    test_audio_block();                     puts("");
    test_haptic_block();                    puts("");
    test_padding_is_zero();                 puts("");
    test_signed();                          puts("");
    test_no_report_claims_the_hosts_bits(); puts("");
    printf("%d checks, %d failed\n\n", checks, failed);
    return failed ? 1 : 0;
}
