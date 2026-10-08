/* Tests for how long a device's input thread sleeps.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). It slept 1 ms at a time for
 * every device, a thousand wake-ups a second each. These pin
 * src/controllers/input_wait.inl: sleep until the keepalive is due and no
 * longer than 20 ms, wake at once when it is due, and never return 0.
 *
 * ➡️ Build and run:  cc tests/test_input_wait.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>

#include "../src/controllers/input_wait.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

int main(void)
{
    const uint64_t now = 1000000000ull;   /* any time well past zero */

    printf("\nno keepalive (a DualSense, which streams): the cap\n");
    ok(input_wait_ms(now, 0, 0) == 20, "20 ms, not 1");
    ok(input_wait_ms(now, now, 0) == 20, "whatever was sent last");

    printf("\nnothing sent yet: at once, as before\n");
    ok(input_wait_ms(now, 0, 4) == 1, "1 ms for a 4 ms keepalive");
    ok(input_wait_ms(now, 0, 50) == 1, "1 ms for a 50 ms keepalive");

    printf("\nan Xbox pad (4 ms): wakes when the keepalive is due\n");
    ok(input_wait_ms(now, now, 4) == 4, "just sent: 4 ms");
    ok(input_wait_ms(now, now - 2500, 4) == 2, "2.5 ms ago: 2 ms (1.5 rounded up)");
    ok(input_wait_ms(now, now - 3999, 4) == 1, "due within the millisecond: 1");
    ok(input_wait_ms(now, now - 4000, 4) == 1, "due now: 1");
    ok(input_wait_ms(now, now - 900000, 4) == 1, "long overdue (nothing to send): 1, never 0");

    printf("\na generic pad (50 ms): the cap, then what is left\n");
    ok(input_wait_ms(now, now, 50) == 20, "just sent: 20 ms");
    ok(input_wait_ms(now, now - 45000, 50) == 5, "45 ms ago: 5 ms");

    printf("\nnever 0 and never past the cap\n");
    int bad = 0;
    for (unsigned k = 0; k <= 100; ++k) {
        for (uint64_t ago = 0; ago <= 120000; ago += 250) {
            const int w = input_wait_ms(now, now - ago, k);
            if (w < 1 || w > 20) ++bad;
        }
    }
    ok(bad == 0, "every keepalive 0..100 ms, every age 0..120 ms");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
