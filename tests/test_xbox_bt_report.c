/* Tests for keeping a Bluetooth Xbox pad's last state report, which the pump
 * sends again while the pad is still.
 *
 * ⭐⭐ WHY THIS EXISTS. A wrong keep does not fail loudly: resend the battery
 * report and the listener's map reads it as the pad's state, or keep nothing
 * and a pad put down leaves the bridge after 15 s again. The reports below were
 * read off a real Xbox One S pad over Bluetooth on the U5s (2026-09-16).
 *
 * ➡️ Build and run:  cc tests/test_xbox_bt_report.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

#include "../src/controllers/xbox_bt_report.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* d-pad right held, sticks at rest: byte 13 is the d-pad */
static const uint8_t k_right[17] = {
    0x01, 0x97, 0x88, 0xd8, 0x84, 0x00, 0x7a, 0xce, 0x86,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
};
/* the same pad a moment later, d-pad centred */
static const uint8_t k_centred[17] = {
    0x01, 0x97, 0x88, 0xd8, 0x84, 0x00, 0x7a, 0xce, 0x86,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* its battery, sent every 20 s */
static const uint8_t k_battery[2] = { 0x04, 0x86 };

static void test_nothing_before_the_first_report(void)
{
    xbox_bt_last_t last;
    memset(&last, 0, sizeof(last));
    uint8_t out[64];
    ok(xbox_bt_current(&last, out, sizeof(out)) == 0, "nothing to send before the pad reports");
    ok(xbox_bt_current(NULL, out, sizeof(out)) == 0, "no kept state (no type context yet) sends nothing");
}

static void test_the_state_report_comes_back_byte_for_byte(void)
{
    xbox_bt_last_t last;
    memset(&last, 0, sizeof(last));
    uint8_t out[64];
    ok(xbox_bt_keep(&last, k_right, sizeof(k_right)) == 1, "a 0x01 report is kept");
    ok(xbox_bt_current(&last, out, sizeof(out)) == sizeof(k_right), "it comes back at its own length, 17");
    ok(memcmp(out, k_right, sizeof(k_right)) == 0, "it comes back byte for byte");
}

static void test_a_newer_state_replaces_the_older(void)
{
    xbox_bt_last_t last;
    memset(&last, 0, sizeof(last));
    uint8_t out[64];
    xbox_bt_keep(&last, k_right, sizeof(k_right));
    xbox_bt_keep(&last, k_centred, sizeof(k_centred));
    ok(xbox_bt_current(&last, out, sizeof(out)) == sizeof(k_centred) &&
       memcmp(out, k_centred, sizeof(k_centred)) == 0,
       "the release replaces the press: a still pad resends the d-pad centred");
}

static void test_the_battery_report_is_not_the_state(void)
{
    xbox_bt_last_t last;
    memset(&last, 0, sizeof(last));
    uint8_t out[64];
    xbox_bt_keep(&last, k_right, sizeof(k_right));
    ok(xbox_bt_keep(&last, k_battery, sizeof(k_battery)) == 0, "the battery report (0x04) is not kept");
    ok(xbox_bt_current(&last, out, sizeof(out)) == sizeof(k_right) &&
       memcmp(out, k_right, sizeof(k_right)) == 0,
       "the last state survives a battery report in between");
}

static void test_what_cannot_be_kept_or_sent(void)
{
    xbox_bt_last_t last;
    memset(&last, 0, sizeof(last));
    uint8_t big[65];
    memset(big, 0, sizeof(big));
    big[0] = 0x01;
    uint8_t one = 0x01;
    uint8_t out[64];
    uint8_t small[16];
    ok(xbox_bt_keep(&last, big, sizeof(big)) == 0, "a report longer than 64 bytes is not kept");
    ok(xbox_bt_keep(&last, &one, 1) == 0, "a report id with no state after it is not kept");
    ok(xbox_bt_current(&last, out, sizeof(out)) == 0, "so there is still nothing to send");
    xbox_bt_keep(&last, k_right, sizeof(k_right));
    ok(xbox_bt_current(&last, small, sizeof(small)) == 0, "a buffer too small for the report gets nothing");
}

int main(void)
{
    test_nothing_before_the_first_report();
    test_the_state_report_comes_back_byte_for_byte();
    test_a_newer_state_replaces_the_older();
    test_the_battery_report_is_not_the_state();
    test_what_cannot_be_kept_or_sent();
    printf("%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
