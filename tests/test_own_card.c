/* Tests for finding a pad's own sound card by its USB id (own_card.inl).
 *
 * ⭐ WHY THIS EXISTS (2026-11-03). A cabled second-model DS4 is a USB audio
 * device, and its card is found by the id snd-usb-audio writes for it in
 * /proc/asound/cardN/usbid. A card that is not its own must never be taken:
 * the pad's speaker sound would play through another pad, or the TV's own
 * card. ⭐ And since 2026-11-04, a card with the pad's id on ANOTHER USB device
 * is not its own either: two pads can share an id, one with no card at all.
 *
 * ⛔ Nothing here needs a television, a controller or a stream. The finder
 * reads one small file per card; these tests point it at a scratch directory
 * and write those files themselves.
 *
 * ➡️ Build and run:  cc tests/test_own_card.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sound/asound.h>

/* ⓘ The finder is included from the real file rather than copied, so a change
 * there is seen here. Only its paths are pointed elsewhere. */
static char g_usbid_fmt[160];
static char g_usbbus_fmt[160];
static char g_stream_fmt[160];
static char g_char_fmt[160];
static char g_control_fmt[160];
#define OWN_CARD_USBID_FMT      g_usbid_fmt     /* "<dir>/card%d/usbid" */
#define OWN_CARD_USBBUS_FMT     g_usbbus_fmt    /* "<dir>/card%d/usbbus" */
#define OWN_CARD_STREAM_FMT     g_stream_fmt    /* "<dir>/card%d/stream0" */
#define OWN_CARD_SYSFS_CHAR_FMT g_char_fmt      /* "<dir>/sys/dev/char/%u:%u" */
#define OWN_CARD_CONTROL_FMT    g_control_fmt   /* "<dir>/controlC%d" */

#include "../src/controllers/own_card.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static char g_dir[128];

/* Write card `card`'s usbid as `text`, the way the kernel would. */
static void set_usbid(int card, const char *text)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/card%d", g_dir, card);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/card%d/usbid", g_dir, card);
    FILE *f = fopen(path, "w");
    if (!f) { printf("  (could not write %s: errno=%d)\n", path, errno); return; }
    fputs(text, f);
    fclose(f);
}

/* Write `text` into card `card`'s file `name` (usbbus, stream0). */
static void set_card_file(int card, const char *name, const char *text)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/card%d", g_dir, card);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/card%d/%s", g_dir, card, name);
    FILE *f = fopen(path, "w");
    if (!f) { printf("  (could not write %s: errno=%d)\n", path, errno); return; }
    fputs(text, f);
    fclose(f);
}

/* Make every folder of `rel` under the scratch folder, as mkdir -p would. */
static void make_dirs(const char *rel)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, rel);
    for (char *p = path + strlen(g_dir) + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(path, 0700);
        *p = '/';
    }
    mkdir(path, 0700);
}

/* Write `text` into the file `rel` under the scratch folder. */
static void write_rel(const char *rel, const char *text)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, rel);
    FILE *f = fopen(path, "w");
    if (!f) { printf("  (could not write %s: errno=%d)\n", path, errno); return; }
    fputs(text, f);
    fclose(f);
}

static void remove_card(int card)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/card%d/usbid", g_dir, card);
    unlink(path);
    snprintf(path, sizeof(path), "%s/card%d", g_dir, card);
    rmdir(path);
}

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/ctm-own-card-%d", (int)getpid());
    mkdir(g_dir, 0700);
    snprintf(g_usbid_fmt, sizeof(g_usbid_fmt), "%s/card%%d/usbid", g_dir);
    snprintf(g_usbbus_fmt, sizeof(g_usbbus_fmt), "%s/card%%d/usbbus", g_dir);
    snprintf(g_stream_fmt, sizeof(g_stream_fmt), "%s/card%%d/stream0", g_dir);
    snprintf(g_char_fmt, sizeof(g_char_fmt), "%s/sys/dev/char/%%u:%%u", g_dir);
    snprintf(g_control_fmt, sizeof(g_control_fmt), "%s/controlC%%d", g_dir);

    /* As the C1 had them on 2026-11-03: its own two cards (no usbid at all),
     * then a DS4Dongle's. Then a DualSense's, written in upper case, and one
     * that is not an id. */
    set_usbid(2, "054c:09cc\n");
    set_usbid(3, "054C:0CE6\n");
    set_usbid(4, "not an id\n");

    printf("\nfinding a card by the pad's USB id\n");
    ok(!own_card_usbid_is(0, 0x054c, 0x09cc), "the TV's own card (no usbid) is not a pad's");
    ok(own_card_usbid_is(2, 0x054c, 0x09cc), "a second-model DS4's card is found by its id");
    ok(!own_card_usbid_is(2, 0x054c, 0x05c4), "a first-model DS4's id does not match it");
    ok(!own_card_usbid_is(3, 0x054c, 0x09cc), "a DualSense's card is not the DS4's");
    ok(own_card_usbid_is(3, 0x054c, 0x0ce6), "and upper case reads the same as lower");
    ok(!own_card_usbid_is(4, 0x054c, 0x09cc), "a usbid that is not an id matches nothing");
    ok(!own_card_usbid_is(7, 0x054c, 0x09cc), "a card that is not there matches nothing");

    printf("\nthe volume, with no mixer to set it on\n");
    char said[200];
    own_card_volume_up(2, said, sizeof(said));
    ok(strstr(said, "mixer not opened") != NULL, "says the mixer did not open");
    ok(strstr(said, "volume left as it was") != NULL, "and that nothing was changed");

    /* ⭐ Two pads with one id (2026-11-04): a GameSir in PlayStation mode is a
     * 054c:09cc with no sound card, and a second-model DS4 is one with a card.
     * Whichever bridged first took the card. Here the DS4Dongle's card 2 is on
     * USB device 001/005, and card 5 is a second pad of the same model on
     * 001/006. */
    printf("\nwhich card is the pad's own: the one on its USB device\n");
    set_card_file(2, "usbbus", "001/005\n");
    set_usbid(5, "054c:09cc\n");
    set_card_file(5, "usbbus", "001/006\n");
    ok(own_card_usbbus_is(2, "001/005"), "the card on the pad's own USB device is its own");
    ok(own_card_usbid_is(5, 0x054c, 0x09cc) && !own_card_usbbus_is(5, "001/005"),
       "a second pad of the same model, on another USB device, is not this pad's");
    ok(!own_card_usbbus_is(7, "001/005"), "a card with no usbbus is nobody's");
    ok(!own_card_usbbus_is(2, ""), "and no card is anyone's while the pad's device is not known");

    printf("\nthe pad's USB device, from sysfs\n");
    /* The C1's layout: the root hub, a hub, the pad, its interface 3, the HID
     * device and its hidraw node. Every USB device has busnum and devnum; the
     * interface and the HID device below it do not. */
    make_dirs("sys/devices/usb1/1-1/1-1.1.1/1-1.1.1:1.3/0003:054C:09CC.0005/hidraw/hidraw0");
    write_rel("sys/devices/usb1/busnum", "1\n");
    write_rel("sys/devices/usb1/devnum", "1\n");
    write_rel("sys/devices/usb1/1-1/busnum", "1\n");
    write_rel("sys/devices/usb1/1-1/devnum", "2\n");
    write_rel("sys/devices/usb1/1-1/1-1.1.1/busnum", "1\n");
    write_rel("sys/devices/usb1/1-1/1-1.1.1/devnum", "5\n");
    make_dirs("sys/dev/char");
    {
        char target[512], link[512];
        snprintf(target, sizeof(target),
                 "%s/sys/devices/usb1/1-1/1-1.1.1/1-1.1.1:1.3/0003:054C:09CC.0005/hidraw/hidraw0", g_dir);
        snprintf(link, sizeof(link), "%s/sys/dev/char/247:0", g_dir);
        if (symlink(target, link) != 0) printf("  (could not link %s: errno=%d)\n", link, errno);
    }
    char busdev[16];
    ok(own_card_busdev_of_char(247, 0, busdev, sizeof(busdev)) && strcmp(busdev, "001/005") == 0,
       "a hidraw node's USB device is the first folder up with busnum and devnum, not its hub's");
    ok(!own_card_busdev_of_char(247, 9, busdev, sizeof(busdev)) && busdev[0] == '\0',
       "a node sysfs does not have is on no known device");

    printf("\nor its USB path, where sysfs cannot be read\n");
    char usb_path[64];
    ok(own_card_usb_path_of_phys("usb-generic-ehci.0-1.1.1/input3", usb_path, sizeof(usb_path)) &&
       strcmp(usb_path, "usb-generic-ehci.0-1.1.1") == 0, "the node's USB path, without its interface");
    ok(!own_card_usb_path_of_phys("a0:fa:9c:ef:9b:30", usb_path, sizeof(usb_path)),
       "a Bluetooth node has none");
    ok(!own_card_usb_path_of_phys("", usb_path, sizeof(usb_path)), "nor a node with no path");
    set_card_file(2, "stream0",
                  "Sony Interactive Entertainment Wireless Controller at usb-generic-ehci.0-1.1.1, full speed : USB Audio\n");
    set_card_file(5, "stream0",
                  "Sony Interactive Entertainment Wireless Controller at usb-generic-ehci.0-1.1.10, full speed : USB Audio\n");
    set_card_file(6, "stream0",
                  "Sony Interactive Entertainment Wireless Controller at usb-generic-ehci.0-1.1.1.2\n");
    ok(own_card_on_usb_path(2, "usb-generic-ehci.0-1.1.1"), "the card named for the pad's path is its own");
    ok(!own_card_on_usb_path(2, "usb-generic-ehci.0-1.1"), "a pad on the hub above it is not the card's");
    ok(!own_card_on_usb_path(5, "usb-generic-ehci.0-1.1.1"), "nor a pad on a neighbouring port, 1.1.1 against 1.1.10");
    ok(!own_card_on_usb_path(6, "usb-generic-ehci.0-1.1.1.2"),
       "a name the kernel cut before its comma cannot say: no card");

    remove_card(2);
    remove_card(3);
    remove_card(4);
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
        if (system(cmd) != 0) printf("  (could not remove %s)\n", g_dir);
    }

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
