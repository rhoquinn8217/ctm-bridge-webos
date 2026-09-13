/* Two questions asked of a device node before and during a bridge.
 *
 * ⓘ Included by controller_common.c, and by tests/test_hid_node_checks.c so both
 * can be tested with no television. Needs only the standard headers and
 * <linux/hidraw.h> (or the fallback defines in controller_common.c).
 *
 * 1. CAN THIS NODE BE READ AS A HID DEVICE AT ALL?
 *
 * ⛔ THE FAULT: a wired Xbox pad has no hidraw node. The kernel's xpad driver
 * gives it /dev/input/jsN, which cannot answer HIDIOCGRAWINFO. The session found
 * that out on its own thread, AFTER the host had been asked to build a session
 * and after the row already read bridged, and then stopped without telling
 * anyone: the row stayed bridged and the host's session timed out 30 s later.
 * Measured on the U5s 2026-09-13 with both Xbox pads.
 *
 * ➡️ So the plug asks first, and a node that cannot be read is refused before
 * anything is sent to the host.
 *
 * 2. WHICH INPUT NODES ARE THIS DEVICE'S?
 *
 * ⭐ Grabbing a device's input nodes is what stops the TV using its input while
 * the host has it. ⛔ They used to be found by vendor and product alone, which
 * takes every device with that pair: both halves of a keyboard dongle when one
 * is bridged, and both of two identical controllers.
 *
 * ➡️ Read off the U5s 2026-09-13 (kernel 6.12), what each driver fills in:
 *
 *   hid-playstation (DualSense, DS4)   phys EMPTY      uniq = the pad's MAC
 *   hid-input (the KMA2 dongle)        phys .../input0 (keyboard) or
 *                                           .../input1 (mouse, media keys)
 *                                      uniq empty
 *   xpad (both Xbox pads)              phys .../input0  uniq empty
 *
 * So the serial decides when both sides have one, the physical path decides
 * when both sides have one, and otherwise it is vendor and product as before.
 * ⚠️ Comparing only where BOTH sides are filled is what keeps a wired DualSense
 * on the monitor grabbed: its kernel gives it no serial, and hid-playstation
 * gives its input nodes no path. */

#ifndef HID_NODE_CHECKS_INL
#define HID_NODE_CHECKS_INL

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Returns 0 if `path` opens and answers as a HID device with the wanted vendor
 * and product (0 wants any), or the errno that refused it. Opens and closes;
 * nothing is written to the device. */
static int hid_node_preflight(const char *path, unsigned int want_vid, unsigned int want_pid)
{
    if (!path || !path[0]) return EINVAL;
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno ? errno : EIO;

    struct hidraw_devinfo info;
    memset(&info, 0, sizeof(info));
    int rc = 0;
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0) {
        rc = errno ? errno : ENOTTY;
    } else {
        const unsigned int vid = (unsigned short)info.vendor;
        const unsigned int pid = (unsigned short)info.product;
        if ((want_vid && vid != want_vid) || (want_pid && pid != want_pid)) rc = ENODEV;
    }
    close(fd);
    return rc;
}

static int hid_hex_is(const char *text, unsigned int value)
{
    if (!text || !text[0]) return 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 16);
    return end != text && parsed == value;
}

/* How an input node was matched, for the log. */
typedef enum {
    EVDEV_NOT_OURS = 0,
    EVDEV_BY_SERIAL,
    EVDEV_BY_PATH,
    EVDEV_BY_VENDOR_PRODUCT
} evdev_match_t;

/* Does the input device with these sysfs attributes belong to the HID device
 * whose hidraw node reported `phys` and whose kernel serial is `uniq`? */
static evdev_match_t evdev_input_belongs(unsigned int vid, unsigned int pid,
                                         const char *phys, const char *uniq,
                                         const char *in_vendor, const char *in_product,
                                         const char *in_phys, const char *in_uniq)
{
    if (!hid_hex_is(in_vendor, vid) || !hid_hex_is(in_product, pid)) return EVDEV_NOT_OURS;
    if (uniq && uniq[0] && in_uniq && in_uniq[0]) {
        return strcasecmp(uniq, in_uniq) == 0 ? EVDEV_BY_SERIAL : EVDEV_NOT_OURS;
    }
    if (phys && phys[0] && in_phys && in_phys[0]) {
        return strcmp(phys, in_phys) == 0 ? EVDEV_BY_PATH : EVDEV_NOT_OURS;
    }
    return EVDEV_BY_VENDOR_PRODUCT;
}

static const char *evdev_match_name(evdev_match_t how)
{
    switch (how) {
    case EVDEV_BY_SERIAL:         return "serial";
    case EVDEV_BY_PATH:           return "physical path";
    case EVDEV_BY_VENDOR_PRODUCT: return "vendor and product";
    default:                      return "none";
    }
}

#endif /* HID_NODE_CHECKS_INL */
