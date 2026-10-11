/* Tests for the shapes every signal's light takes.
 *
 * ⭐ WHY THIS EXISTS (rhoquinn8217, 2026-10-08). A release looked different on
 * every pad: two yellow breaths on a cabled DualSense, three very fast flashes
 * over Bluetooth, three breaths on a DS4, three slow flashes from the app. The
 * user chose three quick flashes, and a bridged pad that breathes green until
 * the PC takes its light. These pin src/controllers/signal_shape.inl, which
 * every pad's signal now draws from.
 *
 * ➡️ Build and run:  cc tests/test_signal_shape.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>

#include "../src/controllers/signal_shape.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

int main(void)
{
    printf("\nthree quick flashes\n");
    int flashes = 0, lit_ms = 0, last = 0;
    for (long t = 0; t < SIGNAL_FLASH_MS; ++t) {
        const int l = signal_flash_level(t, SIGNAL_FLASH_MS, SIGNAL_FLASHES);
        if (l && !last) ++flashes;
        if (l) ++lit_ms;
        last = l;
    }
    ok(flashes == 3, "three flashes, no more and no fewer");
    ok(lit_ms == 375, "each one 125 ms on");
    ok(signal_flash_level(0, SIGNAL_FLASH_MS, SIGNAL_FLASHES) == 255, "the first is lit at once");
    ok(signal_flash_level(125, SIGNAL_FLASH_MS, SIGNAL_FLASHES) == 0 &&
       signal_flash_level(250, SIGNAL_FLASH_MS, SIGNAL_FLASHES) == 255, "125 ms on, 125 ms off");
    ok(signal_flash_level(SIGNAL_FLASH_MS, SIGNAL_FLASH_MS, SIGNAL_FLASHES) == 0 &&
       signal_flash_level(-1, SIGNAL_FLASH_MS, SIGNAL_FLASHES) == 0, "dark outside the pattern");
    ok(SIGNAL_FLASH_MS <= 800, "quick: the whole pattern under eight tenths of a second");

    printf("\nthe bridged-and-waiting breath\n");
    ok(signal_ready_level(0) == 255, "starts at full, where the bridge's solid green left it");
    ok(signal_ready_level(SIGNAL_READY_BREATH_MS / 2) == SIGNAL_READY_FLOOR, "falls to the floor half way");
    int lowest = 255, falls = 1, prev = 256, biggest_step = 0;
    for (long t = 0; t <= SIGNAL_READY_BREATH_MS / 2; t += 10) {
        const int l = signal_ready_level(t);
        if (l > prev) falls = 0;
        if (l < lowest) lowest = l;
        prev = l;
    }
    for (long t = 0; t < 2 * SIGNAL_READY_BREATH_MS; t += SIGNAL_READY_STEP_MS) {
        int step = signal_ready_level(t + SIGNAL_READY_STEP_MS) - signal_ready_level(t);
        if (step < 0) step = -step;
        if (step > biggest_step) biggest_step = step;
    }
    ok(falls, "falls without a bounce");
    ok(lowest >= SIGNAL_READY_FLOOR && lowest > 0, "never dark: it breathes, it does not blink");
    ok(biggest_step < 32, "smooth at the step the session paints it: no visible jump");
    ok(signal_ready_level(SIGNAL_READY_BREATH_MS) == 255 &&
       signal_ready_level(5 * SIGNAL_READY_BREATH_MS + 300) == signal_ready_level(300),
       "and goes on, breath after breath, for as long as nothing takes it");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
