/* A wired Xbox pad, read as input events, turned into the report the listener
 * already reads from an Xbox pad over Bluetooth.
 *
 * ⭐⭐ WHY THIS SHAPE. The kernel's xpad driver gives a wired Xbox pad no hidraw
 * node, only /dev/input/jsN and eventN, so there are no HID reports to relay.
 * The listener already has a real Xbox Series device (a captured 045e:0b12 USB
 * profile) fed by `xbox_gip_usb_over_xbox_bt.map`, which reads the Bluetooth
 * input report. ➡️ So the TV builds that report from the events and the pad
 * reaches the PC as an Xbox pad, with no new listener code. rhoquinn8217,
 * 2026-09-12: "the Xbox pad should appear as itself".
 *
 * ⛔ THE MAP IS THE AUTHORITY on every byte here, read 2026-09-13:
 *
 *   report id 0x01, then the 16-byte payload
 *   [0..1] LX  [2..3] LY  [4..5] RX  [6..7] RY   u16, centre 0x8000, Y down
 *   [8..9] LT  [10..11] RT                       u16, 0..1023
 *   [12]   hat ordinal: 0 none, 1 up, then clockwise to 8 up-left
 *   [13]   A 0x01  B 0x02  X 0x08  Y 0x10  LB 0x40  RB 0x80
 *   [14]   View 0x04  Menu 0x08  L3 0x20  R3 0x40
 *
 * and the host's rumble comes back as report 0x03: enable mask, left trigger,
 * right trigger, left motor, right motor, on time, off time, repeat.
 *
 * ⚠️ WHAT CANNOT CROSS: the Guide button (the map has no byte for it), the
 * Series pad's Share button, and the impulse-trigger motors, which have no
 * force-feedback equivalent on the TV.
 *
 * ⓘ Pure: no file descriptors, no threads. controller_xpad.c does the I/O, and
 * tests/test_xpad_report.c checks every field against the map. */

#ifndef XPAD_REPORT_INL
#define XPAD_REPORT_INL

#include <stdint.h>
#include <string.h>

#ifdef __linux__
#include <linux/input.h>
#endif

#define XPAD_BT_REPORT_ID      0x01
#define XPAD_BT_REPORT_LEN     17
#define XPAD_RUMBLE_REPORT_ID  0x03
#define XPAD_RUMBLE_REPORT_LEN 9

/* The GIP enable-mask bits for the two big motors (Linux xpad.c). */
#define XPAD_MOTOR_RIGHT 0x01
#define XPAD_MOTOR_LEFT  0x02

/* d-pad directions, gathered from a hat or from four buttons */
#define XPAD_DPAD_UP    0x01
#define XPAD_DPAD_RIGHT 0x02
#define XPAD_DPAD_DOWN  0x04
#define XPAD_DPAD_LEFT  0x08

typedef struct {
    int min;
    int max;
} xpad_range_t;

typedef struct {
    /* ⭐ Read from the device at open, never assumed: the ranges are the
     * driver's to choose. */
    xpad_range_t stick_range[4];   /* ABS_X, ABS_Y, ABS_RX, ABS_RY */
    xpad_range_t trigger_range[2]; /* ABS_Z, ABS_RZ */
    int stick[4];
    int trigger[2];
    uint8_t dpad;
    uint8_t buttons13;
    uint8_t buttons14;
} xpad_state_t;

/* A state at rest, for a pad whose ranges are not known yet. */
static void xpad_state_init(xpad_state_t *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 4; ++i) {
        s->stick_range[i].min = -32768;
        s->stick_range[i].max = 32767;
    }
    for (int i = 0; i < 2; ++i) {
        s->trigger_range[i].min = 0;
        s->trigger_range[i].max = 1023;
    }
}

static uint16_t xpad_scale(int v, xpad_range_t r, uint32_t out_max)
{
    if (r.max <= r.min) return 0;
    if (v < r.min) v = r.min;
    if (v > r.max) v = r.max;
    return (uint16_t)(((int64_t)(v - r.min) * (int64_t)out_max) / (int64_t)(r.max - r.min));
}

static void xpad_key(uint8_t *byte, uint8_t bit, int pressed)
{
    if (pressed) *byte |= bit;
    else *byte &= (uint8_t)~bit;
}

static void xpad_dpad_axis(uint8_t *dpad, uint8_t negative, uint8_t positive, int value)
{
    *dpad &= (uint8_t)~(negative | positive);
    if (value < 0) *dpad |= negative;
    else if (value > 0) *dpad |= positive;
}

/* Feed one input event. Returns 1 when a frame is complete and a report should
 * go to the host, 0 otherwise. */
static int xpad_apply_event(xpad_state_t *s, unsigned int type, unsigned int code, int value)
{
    if (type == EV_SYN) return code == SYN_REPORT ? 1 : 0;

    if (type == EV_KEY) {
        const int down = value != 0;   /* 2 is autorepeat: still down */
        switch (code) {
        case BTN_A:      xpad_key(&s->buttons13, 0x01, down); break;
        case BTN_B:      xpad_key(&s->buttons13, 0x02, down); break;
        case BTN_X:      xpad_key(&s->buttons13, 0x08, down); break;
        case BTN_Y:      xpad_key(&s->buttons13, 0x10, down); break;
        case BTN_TL:     xpad_key(&s->buttons13, 0x40, down); break;
        case BTN_TR:     xpad_key(&s->buttons13, 0x80, down); break;
        case BTN_SELECT: xpad_key(&s->buttons14, 0x04, down); break;
        case BTN_START:  xpad_key(&s->buttons14, 0x08, down); break;
        case BTN_THUMBL: xpad_key(&s->buttons14, 0x20, down); break;
        case BTN_THUMBR: xpad_key(&s->buttons14, 0x40, down); break;
        /* ⓘ xpad reports the d-pad as four buttons when loaded with
         * dpad_to_buttons, in this order. */
        case BTN_TRIGGER_HAPPY1: xpad_key(&s->dpad, XPAD_DPAD_LEFT, down); break;
        case BTN_TRIGGER_HAPPY2: xpad_key(&s->dpad, XPAD_DPAD_RIGHT, down); break;
        case BTN_TRIGGER_HAPPY3: xpad_key(&s->dpad, XPAD_DPAD_UP, down); break;
        case BTN_TRIGGER_HAPPY4: xpad_key(&s->dpad, XPAD_DPAD_DOWN, down); break;
        default: break;
        }
        return 0;
    }

    if (type == EV_ABS) {
        switch (code) {
        case ABS_X:      s->stick[0] = value; break;
        case ABS_Y:      s->stick[1] = value; break;
        case ABS_RX:     s->stick[2] = value; break;
        case ABS_RY:     s->stick[3] = value; break;
        case ABS_Z:      s->trigger[0] = value; break;
        case ABS_RZ:     s->trigger[1] = value; break;
        case ABS_HAT0X:  xpad_dpad_axis(&s->dpad, XPAD_DPAD_LEFT, XPAD_DPAD_RIGHT, value); break;
        case ABS_HAT0Y:  xpad_dpad_axis(&s->dpad, XPAD_DPAD_UP, XPAD_DPAD_DOWN, value); break;
        default: break;
        }
    }
    return 0;
}

/* The hat ordinal the map's hat8.to_dpad_bits reads: 1 up, clockwise to 8. A
 * direction and its opposite together cancel, as on a real pad. */
static uint8_t xpad_hat_ordinal(uint8_t dpad)
{
    int up = (dpad & XPAD_DPAD_UP) && !(dpad & XPAD_DPAD_DOWN);
    int down = (dpad & XPAD_DPAD_DOWN) && !(dpad & XPAD_DPAD_UP);
    int right = (dpad & XPAD_DPAD_RIGHT) && !(dpad & XPAD_DPAD_LEFT);
    int left = (dpad & XPAD_DPAD_LEFT) && !(dpad & XPAD_DPAD_RIGHT);
    if (up && right) return 2;
    if (down && right) return 4;
    if (down && left) return 6;
    if (up && left) return 8;
    if (up) return 1;
    if (right) return 3;
    if (down) return 5;
    if (left) return 7;
    return 0;
}

static void xpad_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

/* Build the 17-byte report. Returns its length, or 0 if `cap` is too small. */
static size_t xpad_build_report(const xpad_state_t *s, uint8_t *out, size_t cap)
{
    if (!s || !out || cap < XPAD_BT_REPORT_LEN) return 0;
    memset(out, 0, XPAD_BT_REPORT_LEN);
    out[0] = XPAD_BT_REPORT_ID;
    uint8_t *p = out + 1;
    for (int i = 0; i < 4; ++i) {
        xpad_put_u16(p + 2 * i, xpad_scale(s->stick[i], s->stick_range[i], 65535u));
    }
    xpad_put_u16(p + 8, xpad_scale(s->trigger[0], s->trigger_range[0], 1023u));
    xpad_put_u16(p + 10, xpad_scale(s->trigger[1], s->trigger_range[1], 1023u));
    p[12] = xpad_hat_ordinal(s->dpad);
    p[13] = s->buttons13;
    p[14] = s->buttons14;
    return XPAD_BT_REPORT_LEN;
}

/* Blank a report in place while the TV's overlay is open: sticks centred,
 * triggers released, nothing pressed. ⓘ See blank_input in ctm_controller.h
 * for why a blank report is sent rather than none. */
static void xpad_blank_report(uint8_t *data, size_t len)
{
    if (!data || len < XPAD_BT_REPORT_LEN || data[0] != XPAD_BT_REPORT_ID) return;
    uint8_t *p = data + 1;
    for (int i = 0; i < 4; ++i) xpad_put_u16(p + 2 * i, 0x8000);
    memset(p + 8, 0, XPAD_BT_REPORT_LEN - 1 - 8);
}

typedef struct {
    uint16_t strong;   /* the left, low-frequency motor */
    uint16_t weak;     /* the right, high-frequency motor */
} xpad_rumble_t;

/* Read the host's rumble report. Returns 0, or -1 if it is not one. Motor
 * levels arrive as a percentage; anything above 100 is taken as full. */
static int xpad_parse_rumble(const uint8_t *r, size_t len, xpad_rumble_t *out)
{
    if (!r || !out || len < XPAD_RUMBLE_REPORT_LEN || r[0] != XPAD_RUMBLE_REPORT_ID) return -1;
    const uint8_t enable = r[1];
    unsigned left = (enable & XPAD_MOTOR_LEFT) ? r[4] : 0u;
    unsigned right = (enable & XPAD_MOTOR_RIGHT) ? r[5] : 0u;
    if (left > 100u) left = 100u;
    if (right > 100u) right = 100u;
    out->strong = (uint16_t)((left * 65535u) / 100u);
    out->weak = (uint16_t)((right * 65535u) / 100u);
    return 0;
}

#endif /* XPAD_REPORT_INL */
