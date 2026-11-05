/* own_card.inl -- a pad's own USB sound card that is not a DualSense's.
 *
 * ⭐ WHY (2026-11-03). A second-model DS4 is a USB audio device on a cable, and
 * the DualSense's card match cannot find it: that gathers cards by the name
 * "DualSense", and proves which is whose by muting a DualSense's microphone.
 * This finds a card by its USB id instead, which snd-usb-audio writes as
 * "054c:09cc" in /proc/asound/cardN/usbid, and sets the card's volume.
 *
 * ⭐⭐ AND ON THE PAD'S OWN USB DEVICE, NOT BY ITS ID ALONE (2026-11-04, a
 * review before the merge). Two pads of one model share an id, and a pad with
 * no sound card can have that id too: a GameSir in PlayStation mode is a
 * 054c:09cc with no card. Taking the first card with the id gave the first pad
 * bridged another pad's card: its sound played in the other pad's headset, and
 * the other pad's microphone went to the PC as its own. ➡️ A card is this
 * pad's only when it is on the pad's own USB device: its usbbus is the bus and
 * device sysfs gives for the pad's hidraw node, or, where sysfs cannot be read,
 * its name ends "at <the node's USB path>,". When neither can tell, no card is
 * taken.
 *
 * Its own file so tests/test_own_card.c can drive it against a scratch folder:
 * the OWN_CARD_*_FMT paths are defined first by a test. */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#ifndef OWN_CARD_USBID_FMT
#define OWN_CARD_USBID_FMT "/proc/asound/card%d/usbid"
#endif
#ifndef OWN_CARD_USBBUS_FMT
#define OWN_CARD_USBBUS_FMT "/proc/asound/card%d/usbbus"
#endif
#ifndef OWN_CARD_STREAM_FMT
#define OWN_CARD_STREAM_FMT "/proc/asound/card%d/stream0"
#endif
#ifndef OWN_CARD_SYSFS_CHAR_FMT
#define OWN_CARD_SYSFS_CHAR_FMT "/sys/dev/char/%u:%u"
#endif
#ifndef OWN_CARD_CONTROL_FMT
#define OWN_CARD_CONTROL_FMT "/dev/snd/controlC%d"
#endif
/* Cards 0 to 7, as every other scan in the core reads them. */
#define OWN_CARD_MAX_CARDS 8

/* Does card `card` belong to the USB device vid:pid? Read from the card's
 * usbid, which the kernel prints as two four-digit hex numbers, "054c:09cc".
 * ⓘ Compared as numbers, so the case of either side does not matter. */
static int own_card_usbid_is(int card, unsigned vid, unsigned pid)
{
    char path[96];
    snprintf(path, sizeof(path), OWN_CARD_USBID_FMT, card);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned v = 0, p = 0;
    const int got = fscanf(f, "%x:%x", &v, &p);
    fclose(f);
    return got == 2 && v == vid && p == pid;
}

/* One number from <dir>/<name>, as sysfs writes busnum and devnum. */
static int own_card_read_uint(const char *dir, const char *name, unsigned *out)
{
    char path[PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    const int got = fscanf(f, "%u", out);
    fclose(f);
    return got == 1;
}

/* The USB bus and device a character device is on, as "BBB/DDD", the way
 * /proc/asound/cardN/usbbus prints them. Read from the node's sysfs folder and
 * the folders above it, up to the first with a busnum and a devnum: that is the
 * USB device, since neither its interface nor the HID device below it has them.
 * 0 when sysfs has no such folder. */
static int own_card_busdev_of_char(unsigned maj, unsigned min, char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    char link[128];
    char dir[PATH_MAX];
    snprintf(link, sizeof(link), OWN_CARD_SYSFS_CHAR_FMT, maj, min);
    if (!realpath(link, dir)) return 0;
    for (int up = 0; up < 12; ++up) {
        unsigned bus = 0, dev = 0;
        if (own_card_read_uint(dir, "busnum", &bus) && own_card_read_uint(dir, "devnum", &dev)) {
            snprintf(out, out_len, "%03u/%03u", bus, dev);
            return 1;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) return 0;
        *slash = '\0';
    }
    return 0;
}

/* Is card `card` on the USB device `busdev`, "001/005"? */
static int own_card_usbbus_is(int card, const char *busdev)
{
    if (!busdev || !busdev[0]) return 0;
    char path[96];
    snprintf(path, sizeof(path), OWN_CARD_USBBUS_FMT, card);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[32] = "";
    const int got = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!got) return 0;
    line[strcspn(line, "\r\n")] = '\0';
    return strcmp(line, busdev) == 0;
}

/* The USB path in a HID node's physical path: "usb-generic-ehci.0-1.1.1" out
 * of "usb-generic-ehci.0-1.1.1/input3". 0 for a node that is not on USB. */
static int own_card_usb_path_of_phys(const char *phys, char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!phys || strncmp(phys, "usb-", 4) != 0) return 0;
    const char *end = strstr(phys, "/input");
    const size_t n = end ? (size_t)(end - phys) : strlen(phys);
    if (n == 0 || n >= out_len) return 0;
    memcpy(out, phys, n);
    out[n] = '\0';
    return 1;
}

/* Is card `card` on the USB path `usb_path`, by its name? The first line of
 * its stream0 reads "<name> at <USB path>, full speed : USB Audio", and the
 * comma after the path says the path is whole. ⚠️ The kernel cuts a card's name
 * at 79 characters, so behind deep hubs the comma can be lost, and then this
 * cannot tell: 0, and no card. */
static int own_card_on_usb_path(int card, const char *usb_path)
{
    if (!usb_path || !usb_path[0]) return 0;
    char path[96];
    snprintf(path, sizeof(path), OWN_CARD_STREAM_FMT, card);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256] = "";
    const int got = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!got) return 0;
    char want[128];
    snprintf(want, sizeof(want), " at %s,", usb_path);
    return strstr(line, want) != NULL;
}

/* ⭐⭐ SET THE CARD'S PLAYBACK VOLUME, AS A COMPUTER DOES WHEN THE PAD IS
 * PLUGGED IN. Measured on the C1 2026-11-03 through a DS4Dongle: every tone
 * written to its card was silent until the card's volume had been set once
 * (`amixer -c 2 sset PCM 60`), and then 3 of 3 were heard. The dongle tells
 * its pad the volume only when a host sets it, which macOS and Windows do at
 * plug-in and the TV never had. ⓘ On a real pad this is its own volume
 * control set to the top, which the host's sound can only be quieter than.
 *
 * ⚠️ TWO WRITES, ONE STEP BELOW THE TOP AND THEN THE TOP. snd-usb-audio sends
 * a value to the device only when it differs from the one it already holds,
 * and it read the top when the card appeared: a single write of the top
 * would send nothing at all.
 *
 * Every "Playback Volume" control goes to its top and every "Playback Switch"
 * on (unmuted). Writes what it did into `said`, for the session's log line. */
static void own_card_volume_up(int card, char *said, size_t said_len)
{
    if (!said || said_len == 0) return;
    said[0] = '\0';
    char path[64];
    snprintf(path, sizeof(path), OWN_CARD_CONTROL_FMT, card);
    const int fd = open(path, O_RDWR);
    if (fd < 0) {
        snprintf(said, said_len, "mixer not opened (errno=%d), volume left as it was", errno);
        return;
    }
    struct snd_ctl_elem_list list;
    memset(&list, 0, sizeof(list));
    struct snd_ctl_elem_id ids[64];
    memset(ids, 0, sizeof(ids));
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
        snprintf(said, said_len, "controls not listed (errno=%d), volume left as it was", errno);
        close(fd);
        return;
    }
    list.space = list.count < 64 ? list.count : 64;
    list.pids = ids;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
        snprintf(said, said_len, "controls not listed (errno=%d), volume left as it was", errno);
        close(fd);
        return;
    }
    size_t at = 0;
    int touched = 0;
    for (unsigned i = 0; i < list.used; ++i) {
        if (ids[i].iface != SNDRV_CTL_ELEM_IFACE_MIXER) continue;
        const char *name = (const char *)ids[i].name;
        const int is_volume = strstr(name, "Playback Volume") != NULL;
        const int is_switch = strstr(name, "Playback Switch") != NULL;
        if (!is_volume && !is_switch) continue;

        struct snd_ctl_elem_info info;
        memset(&info, 0, sizeof(info));
        info.id = ids[i];
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) continue;
        const unsigned count = info.count < 8 ? info.count : 8;
        struct snd_ctl_elem_value v;
        memset(&v, 0, sizeof(v));
        v.id = info.id;

        int rc;
        long top = 1;
        if (is_switch && info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
            for (unsigned ch = 0; ch < count; ++ch) v.value.integer.value[ch] = 1;
            rc = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
        } else if (is_volume && info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
            top = info.value.integer.max;
            const long below = (top > info.value.integer.min) ? top - 1 : top;
            for (unsigned ch = 0; ch < count; ++ch) v.value.integer.value[ch] = below;
            (void)ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
            for (unsigned ch = 0; ch < count; ++ch) v.value.integer.value[ch] = top;
            rc = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
        } else {
            continue;
        }
        ++touched;
        if (at < said_len) {
            const int n = snprintf(said + at, said_len - at, "%s%s=%ld%s",
                                   at ? ", " : "", name, top,
                                   rc < 0 ? " (refused)" : "");
            if (n > 0) at += (size_t)n;
        }
    }
    close(fd);
    if (!touched) snprintf(said, said_len, "no playback volume control on the card");
}
