/* Tests for the two node checks: can a node be read as HID, and which input
 * nodes belong to a device.
 *
 * ⭐⭐ WHY THIS EXISTS. On 2026-09-13 a bridged keyboard dongle typed every key
 * twice, because nothing took its input away from the TV, and the only
 * available fix -- the grab the DualSense already had -- found input nodes by
 * vendor and product. On that dongle that takes BOTH halves when one is
 * bridged. The rows below are the attributes read off the U5s that day, so a
 * change to the rules is checked against real devices rather than imagined ones.
 *
 * ⛔ Nothing here needs a television. The preflight cannot be shown succeeding
 * without a real hidraw node; it is shown REFUSING, which is the half that was
 * broken.
 *
 * ➡️ Build and run:  cc tests/test_hid_node_checks.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/hidraw.h>
#endif
#ifndef HIDIOCGRAWINFO
struct hidraw_devinfo { unsigned int bustype; short vendor; short product; };
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)
#endif

#include "../src/controllers/hid_node_checks.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* ── which input nodes are a device's own ─────────────────────────────── */

static void test_keyboard_dongle_halves(void)
{
    printf("\nthe KMA2 dongle: keyboard on input0, mouse and media keys on input1\n");
    const unsigned vid = 0x04ca, pid = 0x00c3;
    const char *kbd = "usb-1c7a0000.xhci-2.1.3/input0";
    const char *mouse = "usb-1c7a0000.xhci-2.1.3/input1";

    ok(evdev_input_belongs(vid, pid, kbd, "", "04ca", "00c3", kbd, "") == EVDEV_BY_PATH,
       "bridging the keyboard half takes its keyboard node");
    ok(evdev_input_belongs(vid, pid, kbd, "", "04ca", "00c3", mouse, "") == EVDEV_NOT_OURS,
       "bridging the keyboard half leaves the mouse on the TV");
    ok(evdev_input_belongs(vid, pid, mouse, "", "04ca", "00c3", mouse, "") == EVDEV_BY_PATH,
       "bridging the second half takes the mouse, media and system nodes");
    ok(evdev_input_belongs(vid, pid, mouse, "", "04ca", "00c3", kbd, "") == EVDEV_NOT_OURS,
       "bridging the second half leaves the keyboard on the TV");
    ok(evdev_input_belongs(vid, pid, kbd, "", "04ca", "00c3",
                           "usb-1c7a0000.xhci-2.2/input0", "") == EVDEV_NOT_OURS,
       "an identical dongle in another port is not ours");
}

static void test_dualsense_by_serial(void)
{
    printf("\nhid-playstation: no physical path on its input nodes, the MAC as serial\n");
    const unsigned vid = 0x054c, pid = 0x0ce6;
    const char *mac = "a0:fa:9c:ef:9b:30";

    ok(evdev_input_belongs(vid, pid, "", mac, "054c", "0ce6", "", mac) == EVDEV_BY_SERIAL,
       "a Bluetooth DualSense takes its own nodes");
    ok(evdev_input_belongs(vid, pid, "", mac, "054c", "0ce6", "", "A0:FA:9C:EF:9B:30") == EVDEV_BY_SERIAL,
       "the serial compares without regard to case");
    ok(evdev_input_belongs(vid, pid, "", mac, "054c", "0ce6", "", "7c:66:ef:82:10:ed") == EVDEV_NOT_OURS,
       "a second DualSense is left on the TV");
    ok(evdev_input_belongs(vid, pid, "usb-1c7a0000.xhci-2.3/input3", mac,
                           "054c", "0ce6", "", mac) == EVDEV_BY_SERIAL,
       "a hidraw path with no path on the input node still matches by serial");
}

static void test_parts_sharing_a_serial(void)
{
    printf("\none USB device, several parts: every part carries the device's serial\n");
    const unsigned vid = 0x1532, pid = 0x0094;
    const char *mouse = "usb-1c7a0000.xhci-2.1.3/input0";
    const char *keys = "usb-1c7a0000.xhci-2.1.3/input1";
    const char *serial = "000000000000";

    ok(evdev_input_belongs(vid, pid, mouse, serial, "1532", "0094", mouse, serial) == EVDEV_BY_SERIAL,
       "a part takes its own nodes");
    ok(evdev_input_belongs(vid, pid, mouse, serial, "1532", "0094", keys, serial) == EVDEV_NOT_OURS,
       "a part leaves its sibling's nodes on the TV, although the serial matches");
    ok(evdev_input_belongs(0x3537, 0x1014, "usb-1c7a0000.xhci-2.1.4/input1", "3286967D",
                           "3537", "1014", "usb-1c7a0000.xhci-2.1.4/input0", "") == EVDEV_NOT_OURS,
       "a GameSir's keyboard part leaves its Xbox pad part alone");
}

static void test_wired_dualsense_without_serial(void)
{
    printf("\na wired DualSense on the monitor: no serial from the kernel, no path on its nodes\n");
    ok(evdev_input_belongs(0x054c, 0x0ce6, "usb-1c7a0000.xhci-2.3/input3", "",
                           "054c", "0ce6", "", "") == EVDEV_BY_VENDOR_PRODUCT,
       "still grabbed, by vendor and product, exactly as before");
}

static void test_xbox_pad(void)
{
    printf("\nxpad: a physical path, no serial\n");
    const char *series = "usb-1c7a0000.xhci-2.4/input0";
    ok(evdev_input_belongs(0x045e, 0x0b12, series, "", "045e", "0b12", series, "") == EVDEV_BY_PATH,
       "the Series pad matches by path");
    ok(evdev_input_belongs(0x045e, 0x0b12, series, "", "045e", "02ea",
                           "usb-1c7a0000.xhci-2.1.4/input0", "") == EVDEV_NOT_OURS,
       "the One S pad is a different product and is not ours");
}

static void test_vendor_product_must_match(void)
{
    printf("\nvendor and product always have to match\n");
    ok(evdev_input_belongs(0x054c, 0x0ce6, "", "a0:fa:9c:ef:9b:30",
                           "054c", "05c4", "", "a0:fa:9c:ef:9b:30") == EVDEV_NOT_OURS,
       "a matching serial on another product is not ours");
    ok(evdev_input_belongs(0x04ca, 0x00c3, "", "", "", "", "", "") == EVDEV_NOT_OURS,
       "an input node with no ids is not ours");
    ok(strcmp(evdev_match_name(EVDEV_BY_PATH), "physical path") == 0 &&
       strcmp(evdev_match_name(EVDEV_NOT_OURS), "none") == 0,
       "the log names how a node was matched");
}

/* ── can a node be read as HID ────────────────────────────────────────── */

static void test_preflight_refuses(void)
{
    printf("\nthe preflight refuses what is not a HID device\n");
    ok(hid_node_preflight("/dev/input/does-not-exist-here", 0x045e, 0x0b12) == ENOENT,
       "a node that is not there is refused with ENOENT");
    ok(hid_node_preflight("", 0, 0) == EINVAL, "an empty path is refused");

    char path[] = "/tmp/ctm-preflight-XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) {
        close(fd);
        const int rc = hid_node_preflight(path, 0x045e, 0x0b12);
        ok(rc != 0, "a file that opens but is not HID is refused, the way js7 was");
        unlink(path);
    } else {
        ok(0, "could not make a scratch file");
    }
}

int main(void)
{
    test_keyboard_dongle_halves();
    test_dualsense_by_serial();
    test_parts_sharing_a_serial();
    test_wired_dualsense_without_serial();
    test_xbox_pad();
    test_vendor_product_must_match();
    test_preflight_refuses();

    printf("\n%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
