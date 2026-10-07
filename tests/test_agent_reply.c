/* Tests for what a command to the listener returns.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). Callers branch on three
 * answers: 0 it worked, -2 the listener was not reached (try once more), -1
 * anything else, where an empty reply means no answer came. Nothing tested
 * that, and the retry and the "stop what may have started" both depend on it.
 * These pin src/shared/agent_reply.inl.
 *
 * ➡️ Build and run:  cc tests/test_agent_reply.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>

#include "../src/shared/agent_reply.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

int main(void)
{
    printf("\nnot reached: -2, whatever else is passed\n");
    ok(agent_command_rc(false, false, 0, NULL) == -2, "no connection");
    ok(agent_command_rc(false, true, 5, "OK 1") == -2, "even with a stray OK");

    printf("\nreached but not sent: -1\n");
    ok(agent_command_rc(true, false, 0, NULL) == -1, "the send failed");

    printf("\nsent, and no answer: -1\n");
    ok(agent_command_rc(true, true, 0, "") == -1, "the far end closed");
    ok(agent_command_rc(true, true, -1, "") == -1, "the read timed out");

    printf("\nan answer\n");
    ok(agent_command_rc(true, true, 2, "OK") == 0, "OK alone is 0");
    ok(agent_command_rc(true, true, 12, "OK 48060 x\n") == 0, "OK with more after it is 0");
    ok(agent_command_rc(true, true, 19, "ERR bad bridge args") == -1, "an error is -1");
    ok(agent_command_rc(true, true, 2, "ok") == -1, "lower case is not OK");
    ok(agent_command_rc(true, true, 1, "O") == -1, "half an OK is not OK");
    ok(agent_command_rc(true, true, 4, NULL) == -1, "no buffer is not OK");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
