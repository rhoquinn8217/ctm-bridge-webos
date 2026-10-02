/* Tests for the message numbers the television and the listener share.
 *
 * ⭐ WHY THIS EXISTS. The listener is a separate program in a separate
 * repository, and it keeps its own copy of this list BY NUMBER. Nothing checks
 * the two against each other, and an unknown number is ignored silently at the
 * far end, so a value that moved here would not fail anywhere: a feature would
 * just stop. These pin what is already on the wire.
 *
 * ⛔ A number is never reused and never changed. A new message takes the next
 * one, here and in the listener's `src/backend/bridge.inl`.
 *
 * ➡️ Build and run:  cc tests/test_protocol_messages.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>

#include "../src/shared/ctm_bridge_protocol.h"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

int main(void)
{
    printf("=== the numbers on the wire ===\n");
    ok(CTMB_MSG_HELLO == 1, "hello is 1");
    ok(CTMB_MSG_HOST_CONFIG == 2, "host config is 2");
    ok(CTMB_MSG_INPUT_REPORT == 3, "an input report is 3");
    ok(CTMB_MSG_OUTPUT_REPORT == 4, "an output report is 4");
    ok(CTMB_MSG_FEATURE_GET == 5, "feature get is 5");
    ok(CTMB_MSG_FEATURE_REPORT == 6, "a feature report is 6");
    ok(CTMB_MSG_LOG == 7, "log is 7");
    ok(CTMB_MSG_ERROR == 8, "error is 8");
    ok(CTMB_MSG_FEATURE_SET == 9, "feature set is 9");
    ok(CTMB_MSG_ENUM == 10, "enumeration is 10");
    ok(CTMB_MSG_ISO_AUDIO == 11, "audio to the controller is 11");
    ok(CTMB_MSG_MIC_AUDIO == 12, "microphone audio is 12");
    ok(CTMB_MSG_AUDIO_HOLD == 13, "the audio hold is 13");
    ok(CTMB_MSG_OPEN_CONFIG == 14, "open the settings window is 14");

    printf("=== the header both ends frame every message with ===\n");
    ok(sizeof(ctmb_header_t) == 32, "32 bytes, packed");
    ok(CTMB_MAGIC == 0x54424d43u, "the magic number");
    ok(CTMB_VERSION == 1u, "version 1");

    printf("\n%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
