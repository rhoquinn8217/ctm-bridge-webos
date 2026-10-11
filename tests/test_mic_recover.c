/* Tests for when the microphone's capture recovers, and how long it waits.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). A cable pulled from a cabled
 * DualSense made poll() fail at once, every time, and the error was answered
 * with no pause and no count: a CPU core spun until the pad was released.
 * These pin src/controllers/mic_recover.inl: a poll error always waits, a run
 * of them asks for a reopen within five, and a good read starts over.
 *
 * ➡️ Build and run:  cc tests/test_mic_recover.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>

#include "../src/controllers/mic_recover.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

int main(void)
{
    mic_recover_t r = {0, 0};
    int wait = -1;

    printf("\na cable pulled: poll fails at once, every time\n");
    int reopened_at = 0, never_zero = 1, total_wait = 0;
    for (int i = 1; i <= 10 && !reopened_at; ++i) {
        const mic_action_t a = mic_recover_error(&r, MIC_ERR_POLL, &wait);
        if (wait <= 0) never_zero = 0;
        total_wait += wait;
        if (a == MIC_REOPEN) reopened_at = i;
    }
    ok(never_zero, "every poll error waits before polling again: no spin");
    ok(reopened_at == 5, "the fifth in a row asks for a reopen, which a pulled cable fails, ending it");
    ok(total_wait >= 100, "about 150 ms of waiting before that, not a busy loop");

    printf("\nwaits grow, and stop growing\n");
    mic_recover_ok(&r);
    int last = 0;
    for (int i = 0; i < 4; ++i) {
        mic_recover_error(&r, MIC_ERR_POLL, &wait);
        ok(wait > last, "each wait longer than the last");
        last = wait;
    }
    r.retry_streak = 1000;
    mic_recover_error(&r, MIC_ERR_POLL, &wait);
    ok(wait == MIC_POLL_WAIT_MAX_MS, "capped");

    printf("\na good read starts over\n");
    mic_recover_ok(&r);
    ok(r.consec_fail == 0 && r.retry_streak == 0, "both runs back to zero");
    ok(mic_recover_error(&r, MIC_ERR_POLL, &wait) == MIC_RETRY, "one error after it is only a retry");

    printf("\nthe other errors, as before\n");
    mic_recover_ok(&r);
    ok(mic_recover_error(&r, MIC_ERR_GONE, &wait) == MIC_REOPEN, "a gone device reopens at once");
    mic_recover_ok(&r);
    int a = MIC_RETRY;
    for (int i = 0; i < 4; ++i) a = mic_recover_error(&r, MIC_ERR_OVERRUN, &wait);
    ok(a == MIC_RETRY, "four overruns are retries");
    ok(mic_recover_error(&r, MIC_ERR_OVERRUN, &wait) == MIC_REOPEN, "the fifth reopens");
    mic_recover_ok(&r);
    mic_recover_error(&r, MIC_ERR_AGAIN, &wait);
    ok(wait == 1, "nothing to read yet waits a millisecond, as it did");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
