/* own_card.inl -- a pad's own USB sound card that is not a DualSense's.
 *
 * ⭐ WHY (2026-11-03). A second-model DS4 is a USB audio device on a cable, and
 * the DualSense's card match cannot find it: that gathers cards by the name
 * "DualSense", and proves which is whose by muting a DualSense's microphone.
 * This finds a card by its USB id instead, which snd-usb-audio writes as
 * "054c:09cc" in /proc/asound/cardN/usbid, and sets the card's volume.
 *
 * ⚠️ Two cabled pads of one model share an id, so the id alone cannot tell
 * their cards apart: the opener takes the first such card it can open (a card
 * another pad plays through is busy), as the DualSense's scan once did. One
 * cabled DS4 at a time is what has been tested.
 *
 * Its own file so tests/test_own_card.c can drive it against a scratch folder:
 * OWN_CARD_USBID_FMT and OWN_CARD_CONTROL_FMT are the paths, defined first by
 * a test. */

#ifndef OWN_CARD_USBID_FMT
#define OWN_CARD_USBID_FMT "/proc/asound/card%d/usbid"
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
