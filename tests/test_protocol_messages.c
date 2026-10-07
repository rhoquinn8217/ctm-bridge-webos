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

#include <stddef.h>
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
    ok(offsetof(ctmb_header_t, payload_len) == 28, "the payload length last, at 28");

    /* ⛔ NOTHING PINNED THESE (code review, 2026-10-05). Both ends copy each
     * structure byte for byte: a field added, moved or resized on one side
     * reads as garbage on the other, with no error anywhere. The listener's
     * src/backend/bridge.inl asserts the same numbers when it compiles. */
    printf("=== the structures both ends copy byte for byte ===\n");
    ok(sizeof(ctmb_device_caps_t) == 272, "device caps: 272 bytes");
    ok(offsetof(ctmb_device_caps_t, serial) == 80, "  the serial at 80");
    ok(sizeof(ctmb_hid_descriptor_info_t) == 32, "descriptor info: 32 bytes");
    ok(sizeof(ctmb_host_config_t) == 58, "host config: 58 bytes");
    ok(offsetof(ctmb_host_config_t, latency_ms) == 27, "  the latency at 27");
    ok(offsetof(ctmb_host_config_t, speaker_volume_pct) == 29, "  the speaker volume at 29");
    ok(offsetof(ctmb_host_config_t, headset_volume_pct) == 30, "  the headset volume at 30");
    ok(offsetof(ctmb_host_config_t, audio_mode) == 31, "  the audio mode at 31");
    ok(sizeof(ctmb_enum_info_t) == 32, "enumeration info: 32 bytes");
    ok(sizeof(ctmb_enum_iface_t) == 4, "an enumerated interface: 4 bytes");

    printf("\n%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
