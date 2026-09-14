/* A wired Xbox pad, read through its input node.
 *
 * ⭐ The kernel's xpad driver gives a wired Xbox pad /dev/input/jsN and eventN
 * and no hidraw node, so the relay every other type uses has nothing to read.
 * Measured on the U5s 2026-09-13: both the Series pad (045e:0b12) and the One S
 * pad (045e:02ea) were refused for exactly that. This type reads the event node
 * instead, keeps the pad's state, and sends the host the Xbox Bluetooth report
 * its Xbox map already reads -- see xpad_report.inl for the bytes and why.
 *
 * ⓘ I/O only. Everything that decides a byte lives in xpad_report.inl, where it
 * is tested against the map. */

#define _GNU_SOURCE

#include "ctm_controller.h"
#include "device_identity.inl"
#include "xpad_report.inl"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>

typedef struct {
    xpad_state_t state;
    int fd;            /* the event node, which the session also owns */
    int resync;        /* SYN_DROPPED: events are unreliable until the next frame */
    int effect_id;     /* the host's rumble effect, or -1 */
    int pulse_id;      /* the confirmation pulse, or -1 */
    int playing;
    int rumble_logged;
    int rumble_broken;
} xpad_ctx_t;

static bool xpad_matches(const ctm_controller_dev_t *dev)
{
    /* ⓘ Only a node under /dev/input: a Bluetooth Xbox pad arrives as hidraw and
     * stays with the Bluetooth type. */
    if (!dev || strncmp(dev->path, "/dev/input/", 11) != 0) return false;

    /* ⭐⭐ ANY PAD THE XBOX DRIVER RUNS, WHOEVER MADE IT. rhoquinn8217,
     * 2026-09-13: "If regular USBIP would have accepted it we shouldn't be
     * refusing it either." The driver gives every one of them the same buttons
     * and axes, which is all xpad_report.inl reads.
     *
     * ⛔ Until then this took only Microsoft's vendor id with a known product or
     * "xbox" in the name. That refused a GameSir in its Xbox mode (vendor 3537,
     * "Generic X-Box pad") -- and Microsoft's own wired 360 pad, whose product
     * was missing from the list and whose driver spells it "X-Box". */
    if (strcmp(dev->driver, "xpad") == 0) return true;

    /* ⓘ The driver could not be read: the old rule, so nothing that worked stops. */
    return dev->driver[0] == '\0' &&
           strcmp(dev->vid, "045e") == 0 &&
           (xbox_known_pid(dev->pid) || (dev->name[0] && strcasestr(dev->name, "xbox")));
}

/* The event node beside a joystick node: both are children of the same
 * /sys/class/input/inputN.
 *
 * ⛔ Walked through the class directory, not resolved with realpath: the
 * realpath of sysfs links is flaky inside the dev-mode jail (see
 * resolve_usb_device_dir in controller_common.c). */
static int xpad_event_node_for(const char *node, char *out, size_t out_len)
{
    if (!node || !node[0]) return -1;
    const char *leaf = strrchr(node, '/');
    leaf = leaf ? leaf + 1 : node;
    if (strncmp(leaf, "event", 5) == 0) {
        snprintf(out, out_len, "%s", node);
        return 0;
    }
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return -1;
    int rc = -1;
    struct dirent *ent;
    while (rc != 0 && (ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char path[320];
        snprintf(path, sizeof(path), "/sys/class/input/%s/%s", ent->d_name, leaf);
        if (access(path, F_OK) != 0) continue;
        snprintf(path, sizeof(path), "/sys/class/input/%s", ent->d_name);
        DIR *in = opendir(path);
        if (!in) continue;
        struct dirent *child;
        while ((child = readdir(in)) != NULL) {
            if (strncmp(child->d_name, "event", 5) == 0) {
                snprintf(out, out_len, "/dev/input/%s", child->d_name);
                rc = 0;
                break;
            }
        }
        closedir(in);
    }
    closedir(dir);
    return rc;
}

/* Open the pad's event node and check it is the pad the row says. Read-write
 * for rumble; read-only if that is all the TV allows, and then there is simply
 * no rumble. Sets errno and returns -1 on failure. */
static int xpad_open_node(const ctm_controller_dev_t *dev, char *event_path, size_t len,
                          int *writable)
{
    if (xpad_event_node_for(dev->path, event_path, len) != 0) {
        errno = ENOENT;
        return -1;
    }
    *writable = 1;
    int fd = open(event_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && errno == EACCES) {
        *writable = 0;
        fd = open(event_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0) return -1;
    struct input_id id;
    if (ioctl(fd, EVIOCGID, &id) < 0) {
        const int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    const unsigned long want_vid = strtoul(dev->vid, NULL, 16);
    const unsigned long want_pid = strtoul(dev->pid, NULL, 16);
    if ((want_vid && id.vendor != want_vid) || (want_pid && id.product != want_pid)) {
        close(fd);
        errno = ENODEV;
        return -1;
    }
    return fd;
}

static int xpad_preflight(const ctm_controller_dev_t *dev)
{
    char event_path[64];
    int writable = 0;
    const int fd = xpad_open_node(dev, event_path, sizeof(event_path), &writable);
    if (fd < 0) return errno ? errno : EIO;
    close(fd);
    return 0;
}

/* Read the whole present state from the driver: at open, and after the kernel
 * dropped events. */
static void xpad_read_state(int fd, xpad_state_t *s)
{
    static const int sticks[4] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
    static const int triggers[2] = { ABS_Z, ABS_RZ };
    struct input_absinfo ai;
    for (int i = 0; i < 4; ++i) {
        if (ioctl(fd, EVIOCGABS(sticks[i]), &ai) == 0) {
            s->stick_range[i].min = ai.minimum;
            s->stick_range[i].max = ai.maximum;
            s->stick[i] = ai.value;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (ioctl(fd, EVIOCGABS(triggers[i]), &ai) == 0) {
            s->trigger_range[i].min = ai.minimum;
            s->trigger_range[i].max = ai.maximum;
            s->trigger[i] = ai.value;
        }
    }
    if (ioctl(fd, EVIOCGABS(ABS_HAT0X), &ai) == 0) xpad_apply_event(s, EV_ABS, ABS_HAT0X, ai.value);
    if (ioctl(fd, EVIOCGABS(ABS_HAT0Y), &ai) == 0) xpad_apply_event(s, EV_ABS, ABS_HAT0Y, ai.value);

    unsigned char keys[(KEY_MAX / 8) + 1];
    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
        static const unsigned codes[] = {
            BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
            BTN_THUMBL, BTN_THUMBR, BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY2,
            BTN_TRIGGER_HAPPY3, BTN_TRIGGER_HAPPY4,
        };
        for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
            const int down = (keys[codes[i] / 8] >> (codes[i] % 8)) & 1;
            xpad_apply_event(s, EV_KEY, codes[i], down);
        }
    }
}

static int xpad_open_input(ctm_controller_t *c, const ctm_controller_dev_t *dev,
                           ctmb_device_caps_t *caps)
{
    char event_path[64];
    int writable = 0;
    const int fd = xpad_open_node(dev, event_path, sizeof(event_path), &writable);
    if (fd < 0) {
        ctl_log(c, "xpad: cannot open the input node behind %s errno=%d", dev->path, errno);
        return -1;
    }
    xpad_ctx_t *x = (xpad_ctx_t *)calloc(1, sizeof(*x));
    if (!x) {
        close(fd);
        return -1;
    }
    xpad_state_init(&x->state);
    x->fd = fd;
    x->effect_id = -1;
    x->pulse_id = -1;
    x->rumble_broken = writable ? 0 : 1;
    xpad_read_state(fd, &x->state);
    free(controller_type_ctx(c));
    controller_set_type_ctx(c, x);

    struct input_id id;
    memset(&id, 0, sizeof(id));
    (void)ioctl(fd, EVIOCGID, &id);
    memset(caps, 0, sizeof(*caps));
    caps->vendor_id = id.vendor;
    caps->product_id = id.product;
    caps->version = id.version;
    caps->bus = id.bustype;
    caps->input_report_len = XPAD_BT_REPORT_LEN;
    caps->output_report_len = XPAD_RUMBLE_REPORT_LEN;
    caps->feature_report_len = 0;
    caps->flags = 1;
    snprintf(caps->path, sizeof(caps->path), "%s", event_path);
    /* ⭐ Its serial (device_identity.inl). The Xbox driver fills no uniq, so this
     * is the USB serial number the device list read beside it -- an Xbox pad has
     * no MAC to give over USB. Blank or all zeros sends nothing. */
    identity_pick_serial(dev->serial, dev->mac, caps->serial, sizeof(caps->serial));
    snprintf(caps->manufacturer, sizeof(caps->manufacturer), "input");
    /* ⭐ The kernel's own model name, e.g. "Microsoft Xbox Series S|X
     * Controller", rather than the family name the TV's list shows. */
    if (ioctl(fd, EVIOCGNAME(sizeof(caps->product) - 1), caps->product) < 0 || !caps->product[0]) {
        snprintf(caps->product, sizeof(caps->product), "%s", dev->name[0] ? dev->name : "Xbox Controller");
    }

    ctl_log(c, "xpad: reading %s (%s, driver %s) as the Xbox Bluetooth report, not grabbed; "
            "rumble %s; the host is told %s",
            event_path, caps->product, dev->driver[0] ? dev->driver : "unknown",
            writable ? "available" : "unavailable (read-only node)",
            caps->serial[0] ? caps->serial : "nothing (no usable serial)");
    return fd;
}

static int xpad_read_input(ctm_controller_t *c, int fd, uint8_t *report, size_t cap)
{
    xpad_ctx_t *x = (xpad_ctx_t *)controller_type_ctx(c);
    if (!x) {
        errno = EINVAL;
        return -1;
    }
    struct input_event ev;
    for (;;) {
        /* ⓘ One event per read, so a frame is sent the moment it completes
         * and nothing after it in a larger read is left behind. */
        const ssize_t n = read(fd, &ev, sizeof(ev));
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        if (n != (ssize_t)sizeof(ev)) return 0;

        if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
            /* The kernel's buffer overflowed: what follows until the next
             * frame is incomplete, so skip it and ask the driver instead. */
            x->resync = 1;
            continue;
        }
        if (x->resync) {
            if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                xpad_read_state(fd, &x->state);
                x->resync = 0;
                return (int)xpad_build_report(&x->state, report, cap);
            }
            continue;
        }
        if (xpad_apply_event(&x->state, ev.type, ev.code, ev.value) == 1) {
            return (int)xpad_build_report(&x->state, report, cap);
        }
    }
}

static int xpad_current_report(ctm_controller_t *c, uint8_t *report, size_t cap)
{
    const xpad_ctx_t *x = (const xpad_ctx_t *)controller_type_ctx(c);
    return x ? (int)xpad_build_report(&x->state, report, cap) : 0;
}

static int xpad_write_output(ctm_controller_t *c, int fd, const uint8_t *report, size_t len)
{
    xpad_ctx_t *x = (xpad_ctx_t *)controller_type_ctx(c);
    xpad_rumble_t m;
    if (!x || xpad_parse_rumble(report, len, &m) != 0) return 0;   /* not rumble: nothing to do */
    if (x->rumble_broken) return 0;

    if (m.strong == 0 && m.weak == 0) {
        if (x->playing && x->effect_id >= 0) {
            struct input_event stop;
            memset(&stop, 0, sizeof(stop));
            stop.type = EV_FF;
            stop.code = (uint16_t)x->effect_id;
            stop.value = 0;
            (void)!write(fd, &stop, sizeof(stop));
            x->playing = 0;
        }
        return 0;
    }

    /* ⓘ One effect, uploaded once and then updated in place: EVIOCSFF with an
     * existing id changes a playing effect without restarting it. Length 0
     * plays until stopped; closing the node at the end of the session erases
     * it, so a host that vanishes mid-rumble cannot leave the pad buzzing. */
    struct ff_effect e;
    memset(&e, 0, sizeof(e));
    e.type = FF_RUMBLE;
    e.id = (int16_t)x->effect_id;
    e.u.rumble.strong_magnitude = m.strong;
    e.u.rumble.weak_magnitude = m.weak;
    if (ioctl(fd, EVIOCSFF, &e) < 0) {
        ctl_log(c, "xpad: rumble upload refused errno=%d -- no rumble for this session", errno);
        x->rumble_broken = 1;
        return -1;
    }
    x->effect_id = e.id;
    if (!x->playing) {
        struct input_event play;
        memset(&play, 0, sizeof(play));
        play.type = EV_FF;
        play.code = (uint16_t)e.id;
        play.value = 1;
        if (write(fd, &play, sizeof(play)) != (ssize_t)sizeof(play)) {
            ctl_log(c, "xpad: rumble play refused errno=%d", errno);
            return -1;
        }
        x->playing = 1;
    }
    if (!x->rumble_logged) {
        x->rumble_logged = 1;
        ctl_log(c, "xpad: first rumble from the host, strong=%u weak=%u", m.strong, m.weak);
    }
    return 0;
}

/* ⭐ THE CONFIRMATION: one short pulse the moment the host has the pad.
 *
 * ⛔ An Xbox pad has no light and no speaker, so the DualSense signal cannot
 * reach it, and the app skips its own SDL rumble for a wired node whose
 * session signals itself. ➡️ So it is played here, through the node this
 * session holds, as the session starts. ✅ Felt on the U5s 2026-09-13.
 * ⓘ Uploaded once and replayed on a reconnect; closing the node erases it. No
 * sleep: the kernel stops it after replay.length. */
static int xpad_on_plug_init(ctm_controller_t *c, ctm_transport_t *t)
{
    (void)t;
    xpad_ctx_t *x = (xpad_ctx_t *)controller_type_ctx(c);
    if (!x || x->fd < 0 || x->rumble_broken || !signals_rumble_on()) return 0;
    struct ff_effect e;
    memset(&e, 0, sizeof(e));
    e.type = FF_RUMBLE;
    e.id = (int16_t)x->pulse_id;
    e.u.rumble.strong_magnitude = 0x7000;
    e.u.rumble.weak_magnitude = 0x7000;
    e.replay.length = 220;
    if (ioctl(x->fd, EVIOCSFF, &e) < 0) {
        ctl_log(c, "xpad: confirmation pulse refused errno=%d", errno);
        return 0;
    }
    x->pulse_id = e.id;
    struct input_event play;
    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = (uint16_t)e.id;
    play.value = 1;
    const int played = write(x->fd, &play, sizeof(play)) == (ssize_t)sizeof(play);
    ctl_log(c, "xpad: confirmation pulse %s", played ? "played" : "not played");
    return 0;
}

const ctm_controller_ops_t controller_xpad_ops = {
    .kind = "xpad",
    /* ⛔ NOT GRABBED. SDL reads this pad through the same event node, and
     * rhoquinn8217, 2026-09-13: "Bridging a controller shouldn't stop the
     * controller from using the LB RB view and menu from bringing up the
     * streaming overlay. It should work regardless." A grab hid the pad from
     * SDL, so the combo and overlay navigation both died. ⓘ Nothing reaches the
     * host twice: the handover retires Moonlight's pad for it. */
    .grab_evdev = false,
    .matches = xpad_matches,
    .on_plug_init = xpad_on_plug_init,
    .blank_input = xpad_blank_report,
    .preflight = xpad_preflight,
    .open_input = xpad_open_input,
    .read_input = xpad_read_input,
    .current_report = xpad_current_report,
    .write_output = xpad_write_output,
    /* ⓘ Well inside the listener's 15 s, and a 17-byte report a second costs
     * nothing on a link carrying video. */
    .keepalive_ms = 1000,
};
