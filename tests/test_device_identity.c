/* Tests for the identity a device is known by.
 *
 * ⭐⭐ WHY THIS EXISTS. On 2026-09-13 rhoquinn8217 set three rules for what the
 * host auto-links a config on and what auto bridge marks: a DualSense is its
 * own MAC, every other controller its serial, and a blank or all-zeros one is
 * no identity. The strings below are the ones read off real devices on the U5s
 * and the C1, so a change to the rules is checked against those devices rather
 * than imagined ones.
 *
 * ➡️ Build and run:  cc tests/test_device_identity.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

#include "../src/shared/device_identity.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static void test_usable(void)
{
    printf("\nwhat counts as an identity\n");
    ok(identity_usable("7c:66:ef:82:10:ed"), "a DualSense's MAC");
    ok(identity_usable("3286967D"), "the GameSir's USB serial");
    ok(identity_usable("3039373031333032333734303237"), "the Series pad's USB serial");
    ok(!identity_usable(""), "blank is not");
    ok(!identity_usable(NULL), "nothing is not");
    ok(!identity_usable("00:00:00:00:00:00"), "an all-zeros MAC is not");
    ok(!identity_usable("0000"), "an all-zeros serial is not");
    ok(!identity_usable("--"), "punctuation alone is not");
}

static void test_same(void)
{
    printf("\none identity however it is written\n");
    ok(identity_same("7c-66-ef-82-10-ed", "7c:66:ef:82:10:ed"),
       "SDL's dashes and the kernel's colons are the same pad");
    ok(identity_same("A0:FA:9C:EF:9B:30", "a0-fa-9c-ef-9b-30"), "case does not matter");
    ok(!identity_same("7c:66:ef:82:10:ed", "a0:fa:9c:ef:9b:30"), "two pads are two identities");
    ok(!identity_same("", ""), "two blanks never match");
    ok(!identity_same("00:00:00:00:00:00", "0000"), "two zero identities never match");
}

static void test_mac_shape(void)
{
    printf("\nwhat looks like a MAC\n");
    ok(identity_mac_shaped("7c:66:ef:82:10:ed"), "the kernel's form");
    ok(identity_mac_shaped("14-3a-9a-cb-f6-9d"), "SDL's form");
    ok(!identity_mac_shaped("948D3F0AD521619B2"), "a DS5dongle's own serial is not (C1, 2026-09-08)");
    ok(!identity_mac_shaped("63635E6BF4E30DE12"), "nor the U5s dongle's (2026-09-13)");
    ok(!identity_mac_shaped("3286967D"), "a USB serial is not");
    ok(!identity_mac_shaped("00:00:00:00:00:00"), "an all-zeros MAC is not a MAC");
    ok(!identity_mac_shaped("7c:66:ef:82:10:eg"), "a letter past f is not");
}

static void test_pick_serial(void)
{
    char out[64];
    printf("\nrule 2: uniq, else the USB serial, else none\n");

    identity_pick_serial("3286967D", "3286967D", out, sizeof(out));
    ok(strcmp(out, "3286967D") == 0, "a cabled HID pad: uniq, which is its USB serial");

    identity_pick_serial("98:7a:14:ce:31:f4", "", out, sizeof(out));
    ok(strcmp(out, "98:7a:14:ce:31:f4") == 0, "a Bluetooth pad: uniq, which is its MAC");

    identity_pick_serial("", "3039373031333032333734303237", out, sizeof(out));
    ok(strcmp(out, "3039373031333032333734303237") == 0,
       "a pad the Xbox driver runs: no uniq, so the USB serial");

    identity_pick_serial("00:00:00:00:00:00", "3286967D", out, sizeof(out));
    ok(strcmp(out, "3286967D") == 0, "an all-zeros uniq falls through to the USB serial");

    identity_pick_serial("", "", out, sizeof(out));
    ok(out[0] == '\0', "the KMA2 dongle: neither, so none");

    identity_pick_serial("0000", "0000", out, sizeof(out));
    ok(out[0] == '\0', "zeros in both, so none");
}

static void test_type_for_usage(void)
{
    printf("\nwhat an interface says it is\n");
    ok(strcmp(identity_type_for_usage(0x01, 0x05), "controller") == 0, "a gamepad is a controller");
    ok(strcmp(identity_type_for_usage(0x01, 0x04), "controller") == 0, "a joystick is a controller");
    ok(strcmp(identity_type_for_usage(0x01, 0x08), "controller") == 0, "a multi-axis device is a controller");
    ok(strcmp(identity_type_for_usage(0x01, 0x06), "keyboard") == 0,
       "a keyboard, like the GameSir's second interface");
    ok(strcmp(identity_type_for_usage(0x0C, 0x01), "keyboard") == 0, "media keys are a keyboard's");
    ok(strcmp(identity_type_for_usage(0x01, 0x80), "keyboard") == 0, "system keys are a keyboard's");
    ok(strcmp(identity_type_for_usage(0x01, 0x02), "mouse") == 0, "a mouse");
    ok(identity_type_for_usage(0xFF00, 0x01)[0] == '\0', "a maker's own interface is not named");
    ok(identity_type_for_usage(0, 0)[0] == '\0', "an unread descriptor is not named");
}

static void test_link_leaf(void)
{
    char out[32];
    printf("\nthe driver named by a sysfs link\n");

    identity_link_leaf("../../../../../../bus/usb/drivers/xpad", out, sizeof(out));
    ok(strcmp(out, "xpad") == 0, "the Xbox driver");

    identity_link_leaf("../../../../bus/hid/drivers/playstation", out, sizeof(out));
    ok(strcmp(out, "playstation") == 0, "the PlayStation driver");

    identity_link_leaf("xpad", out, sizeof(out));
    ok(strcmp(out, "xpad") == 0, "a bare name is its own leaf");

    identity_link_leaf("", out, sizeof(out));
    ok(out[0] == '\0', "an unreadable link names nothing");
}

int main(void)
{
    test_usable();
    test_same();
    test_mac_shape();
    test_pick_serial();
    test_type_for_usage();
    test_link_leaf();
    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
