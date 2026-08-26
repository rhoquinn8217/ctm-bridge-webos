/* Tests for the configured-volume write in ds5_patch_output's AUTO branch.
 *
 * ⭐⭐ WHY THIS EXISTS. On 2026-08-25 the first version of this logic skipped
 * the write whenever the value already matched:
 *
 *     if (data[pos + 7] != auto_speaker) { ...claim and write... }
 *
 * ⛔ For a CONFIGURED VOLUME OF ZERO that is `0 != 0` -- false. So it wrote
 * nothing, AND it shadowed the fallback below it, which does set the claim bit.
 * **It made a working default path stop working**, and the symptom was a
 * controller at full volume with `speaker_volume = 0` in its config.
 *
 * ⚠️ THE THING THE TEST ENCODES: **the claim bit is the point, not the value.**
 * The host was observed sending `volume=00 audio_ctl=00 flags=00` -- volume zero
 * with NOTHING CLAIMED. A zero with no claim bit does not mean "volume zero", it
 * means "not setting volume", and the controller keeps what it had.
 *
 * ⓘ This project's own note, long predating the bug: *"A CLAIMED field is
 * applied even when it is zero, so claiming something we do not intend to set is
 * an active change, not a no-op."* The answer was already written down.
 *
 * ⛔ Nothing here needs a television, a controller or a stream. Bytes in, bytes
 * out. If a test below fails, a configured volume is being silently ignored and
 * nobody has had to notice it by ear.
 *
 * ➡️ Build and run:  cc tests/test_audio_claim.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ── the decision, mirrored from the AUTO branch ──────────────────────────
 *
 * ⓘ Mirrored rather than included: the real function needs a controller, a
 * mutex and a settings snapshot, and none of that is what is being tested. The
 * offsets are the ones the real walk uses -- payload byte 0 at pos+2, speaker
 * volume at pos+7, audio control at pos+9. */

#define CLAIM_SPEAKER_VOLUME  0x20
#define CLAIM_AUDIO_CONTROL   0x80
#define AUDIO_SPEAKER_ON      0x30

/* Returns 1 if the report was changed. `data` is the block, starting at its id. */
static int apply_auto(uint8_t *data, size_t pos, int host_audio_set,
                      uint8_t configured_volume)
{
    if (host_audio_set) {
        const uint8_t want_flags = (uint8_t)(data[pos + 2] | CLAIM_SPEAKER_VOLUME);
        if (data[pos + 2] != want_flags || data[pos + 7] != configured_volume) {
            data[pos + 2] = want_flags;
            data[pos + 7] = configured_volume;
            return 1;
        }
        return 0;
    }
    /* The original fallback: fill in only what the host left blank. */
    if (data[pos + 7] == 0 && data[pos + 9] == 0) {
        data[pos + 2] = (uint8_t)(data[pos + 2] | 0xa0u);
        data[pos + 7] = configured_volume;
        data[pos + 9] = AUDIO_SPEAKER_ON;
        return 1;
    }
    return 0;
}

/* ── harness ─────────────────────────────────────────────────────────────── */

static int g_checks = 0, g_failed = 0;

static void check(int ok, const char *what)
{
    ++g_checks;
    if (!ok) { ++g_failed; printf("  FAIL: %s\n", what); }
}

/* A 0x90 block as the host actually sends it. pos is 0 here for clarity. */
static void make_block(uint8_t *b, uint8_t flags, uint8_t vol, uint8_t ctl)
{
    memset(b, 0, 16);
    b[0] = 0x90;   /* block id      */
    b[1] = 0x3f;   /* payload len   */
    b[2] = flags;  /* claim flags   */
    b[7] = vol;
    b[9] = ctl;
}

int main(void)
{
    uint8_t b[16];

    /* ⭐⭐ THE REGRESSION. Configured 0, and the host already sends 0 with
     * nothing claimed -- exactly what was measured on build 287. The old code
     * saw `0 != 0`, wrote nothing, and the controller stayed loud. */
    make_block(b, 0x00, 0x00, 0x00);
    check(apply_auto(b, 0, 1, 0) == 1,
          "configured 0 with an unclaimed 0 must still change the report");
    check((b[2] & CLAIM_SPEAKER_VOLUME) != 0,
          "configured 0 must SET THE CLAIM BIT -- that is what makes 0 mean 0");
    check(b[7] == 0x00, "configured 0 leaves the volume byte at 0");

    /* ⭐ A configured value the host disagrees with. */
    make_block(b, 0xa0, 0x64, 0x3c);
    check(apply_auto(b, 0, 1, 50) == 1, "configured 50 over a host 100 changes it");
    check(b[7] == 50, "the configured value is written");
    check((b[2] & CLAIM_SPEAKER_VOLUME) != 0, "and stays claimed");

    /* ⭐ Already correct AND already claimed: nothing to do. ⓘ This is the only
     * case the old code got right, and re-signing an unchanged report is waste. */
    make_block(b, CLAIM_SPEAKER_VOLUME, 30, 0x3c);
    check(apply_auto(b, 0, 1, 30) == 0,
          "an already-claimed, already-correct report is left alone");

    /* ⛔ Claimed but wrong value -- must still be corrected. */
    make_block(b, CLAIM_SPEAKER_VOLUME, 99, 0x3c);
    check(apply_auto(b, 0, 1, 30) == 1, "a claimed but wrong value is corrected");
    check(b[7] == 30, "to the configured value");

    /* ⚠️ THE FALLBACK MUST SURVIVE. Nothing configured, host sent nothing --
     * the original behaviour, which the first attempt shadowed. */
    make_block(b, 0x00, 0x00, 0x00);
    check(apply_auto(b, 0, 0, 100) == 1,
          "with nothing configured, the blank-host fallback still fires");
    check(b[7] == 100, "and supplies the default volume");
    check(b[9] == AUDIO_SPEAKER_ON, "and turns the speaker route on");
    check((b[2] & CLAIM_AUDIO_CONTROL) != 0,
          "the fallback claims audio control too -- it writes that byte");

    /* ⭐ Nothing configured and the host HAS asked: AUTO leaves it alone. ⓘ This
     * is what AUTO is for, and it must not become a second Speaker mode. */
    make_block(b, 0xa0, 0x64, 0x3c);
    check(apply_auto(b, 0, 0, 100) == 0,
          "with nothing configured, a host that asked is left untouched");
    check(b[7] == 0x64, "the host's value survives");

    /* ⛔ THE ROUTE IS NOT TOUCHED when a volume is configured. Setting it would
     * make AUTO the second Speaker mode this file warns against. */
    make_block(b, 0x00, 0x00, 0x00);
    apply_auto(b, 0, 1, 40);
    check(b[9] == 0x00, "a configured volume does not change the audio route");
    check((b[2] & CLAIM_AUDIO_CONTROL) == 0,
          "and does not claim a byte it did not write");

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
