/* A blank for any gamepad the TV relays without knowing its report layout,
 * built from the pad's own HID report descriptor, and one for a Switch Pro
 * Controller's full report, which its descriptor does not describe.
 *
 * ⭐⭐ WHY (rhoquinn8217, 2026-09-16: "why aren't we doing blank reports for all
 * controllers in the streaming overlay, usb or BT"). While the TV's overlay is
 * open the host must see nothing pressed. A type that knows its layout blanks
 * its own reports (ds5_blank_input, the DS4's, the cabled Xbox pad's); every
 * other gamepad was relayed unchanged, so a GameSir in its Android mode or a
 * Pro Controller kept driving the PC behind the overlay.
 *
 * ⭐ WHAT A DESCRIPTOR GIVES: where each field sits in its report, what it is
 * (its usage) and its range, so a resting value can be worked out per field:
 *   a button (Button page; a 1-bit Consumer, Keyboard or   0
 *     Generic Desktop control such as Start or a d-pad bit)
 *   a stick (Generic Desktop X, Y, Z, Rx, Ry, Rz)          the centre of its range
 *   anything relative                                      0, no movement
 *   a trigger (Simulation Accelerator or Brake; a Slider,  its minimum
 *     Dial or Wheel)
 *   the d-pad (Hat switch)                                 outside its range: 0 when
 *                                                          the range starts at 1,
 *                                                          else one past the maximum
 *   a button array (no Variable flag)                      0, nothing held
 * ⛔ AND NOTHING ELSE. Vendor pages, motion, battery and counters stay as the pad
 * sent them, as ds5_blank_input leaves the gyro: the host sees a live controller
 * doing nothing, not a stalled one. ⛔ Only fields inside a Joystick, Game Pad or
 * Multi-axis application collection: a keyboard or a vendor collection in the
 * same device is not touched.
 *
 * ⭐⭐ THE PRO CONTROLLER'S FULL REPORT IS NOT DESCRIBED. Its USB descriptor lays
 * report 0x30 out as buttons and 16-bit axes, which is not what its bytes are
 * (Windows' Game Controllers showed wild presses from exactly that, 2026-09-16),
 * and over Bluetooth 0x30 is vendor data. A blank by the descriptor would write
 * centred 16-bit axes over its buttons and packed sticks. ➡️ So for 057e:2009 the
 * reports 0x21 and 0x30 to 0x33 are blanked by the Switch layout instead:
 *   byte 3 right-side buttons, byte 4 shared (its bit 7 is the charging grip,
 *   kept), byte 5 left-side buttons, bytes 6-8 and 9-11 the two sticks as 12-bit
 *   pairs, centred at 0x800. The timer, the battery, the vibrator byte, a
 *   subcommand reply and the motion data stay.
 * ⚠️ A pad's true centre is a little off 0x800 by its calibration; a blank at
 * 0x800 still sits inside a game's dead zone.
 *
 * ⓘ Pure: no controller, no I/O. tests/test_pad_blank.c runs it. */

#ifndef PAD_BLANK_INL
#define PAD_BLANK_INL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PAD_BLANK_MAX_FIELDS 96

typedef struct {
    uint8_t report_id;     /* 0 when the device's reports carry no id */
    uint8_t bit_size;      /* 1 to 32 */
    uint16_t bit_offset;   /* counted from the first byte after the id, if any */
    uint32_t value;        /* the resting value, already cut to bit_size */
} pad_blank_field_t;

typedef struct {
    bool present;          /* something to blank: fields, or the Switch layout */
    bool ids_used;         /* the reports start with an id byte */
    bool switch_layout;    /* a Pro Controller: 0x21 and 0x30 to 0x33 by its own layout */
    bool truncated;        /* more fields than fit; the rest are left alone */
    uint16_t count;
    pad_blank_field_t field[PAD_BLANK_MAX_FIELDS];
} pad_blank_plan_t;

/* The report ids a Pro Controller's own layout covers. */
static inline bool pad_blank_switch_report(uint8_t id)
{
    return id == 0x21 || (id >= 0x30 && id <= 0x33);
}

/* An item's data as the signed number HID ranges are, by the item's size. */
static inline int64_t pad_blank_signed(uint32_t v, size_t size)
{
    if (size == 1) return (int8_t)v;
    if (size == 2) return (int16_t)v;
    if (size == 4) return (int32_t)v;
    return 0;
}

/* The resting value for one variable field, or false to leave it alone. */
static inline bool pad_blank_value_for(uint32_t page, uint32_t id, bool relative,
                                       int64_t min, int64_t max, uint8_t size,
                                       int64_t *out)
{
    if (page == 0x09) { *out = 0; return true; }                     /* button */
    if (page == 0x01) {                                              /* generic desktop */
        if (id >= 0x30 && id <= 0x35) {                              /* X Y Z Rx Ry Rz */
            if (relative) { *out = 0; return true; }
            if (max < min) return false;
            *out = min + (max - min + 1) / 2;
            return true;
        }
        if (id >= 0x36 && id <= 0x38) {                              /* slider, dial, wheel */
            *out = relative ? 0 : min;
            return true;
        }
        if (id == 0x39) {                                            /* hat switch */
            if (min >= 1) { *out = 0; return true; }
            if (size < 32 && max + 1 <= (((int64_t)1 << size) - 1)) { *out = max + 1; return true; }
            return false;
        }
        if (size == 1 && ((id >= 0x3d && id <= 0x3e) ||             /* start, select */
                          (id >= 0x81 && id <= 0x8d) ||             /* system controls */
                          (id >= 0x90 && id <= 0x93))) {            /* d-pad as buttons */
            *out = 0;
            return true;
        }
        return false;
    }
    if (page == 0x02 && (id == 0xc4 || id == 0xc5)) {                /* accelerator, brake */
        *out = relative ? 0 : min;
        return true;
    }
    if ((page == 0x0c || page == 0x07) && size == 1) { *out = 0; return true; }
    return false;
}

static inline void pad_blank_add(pad_blank_plan_t *p, uint8_t report_id, uint32_t bit,
                                 uint8_t size, int64_t value)
{
    if (p->switch_layout && pad_blank_switch_report(report_id)) return;
    if (bit > 0xffff) return;
    if (p->count >= PAD_BLANK_MAX_FIELDS) { p->truncated = true; return; }
    const uint32_t mask = size >= 32 ? 0xffffffffu : ((1u << size) - 1u);
    pad_blank_field_t *f = &p->field[p->count++];
    f->report_id = report_id;
    f->bit_size = size;
    f->bit_offset = (uint16_t)bit;
    f->value = (uint32_t)value & mask;
}

typedef struct {
    uint16_t usage_page;
    uint32_t report_size, report_count;
    uint8_t report_id;
    int64_t logical_min;
    uint32_t logical_max_raw;
    size_t logical_max_size;
} pad_blank_globals_t;

/* Build the plan for a gamepad from its report descriptor. `vid` and `pid` pick
 * out a Pro Controller. Returns out->present. */
static inline bool pad_blank_from_descriptor(const uint8_t *d, size_t n, uint16_t vid,
                                             uint16_t pid, pad_blank_plan_t *out)
{
    memset(out, 0, sizeof(*out));
    out->switch_layout = vid == 0x057e && pid == 0x2009;
    if (d == NULL || n == 0) {
        out->present = out->switch_layout;
        return out->present;
    }

    uint32_t bits[256];
    memset(bits, 0, sizeof(bits));
    pad_blank_globals_t g, stack[4];
    memset(&g, 0, sizeof(g));
    int sp = 0;
    uint32_t usages[32];
    size_t usage_count = 0;
    uint32_t usage_min = 0, usage_max = 0;
    bool have_min = false, have_max = false;
    int depth = 0, pad_depth = 0;

    size_t i = 0;
    while (i < n) {
        const uint8_t b = d[i];
        if (b == 0xfe) {   /* a long item: its data size is in the next byte */
            if (i + 1 >= n) break;
            i += 3u + d[i + 1];
            continue;
        }
        const uint8_t size_code = b & 0x03;
        const size_t size = size_code == 3 ? 4 : size_code;
        const uint8_t type = (b >> 2) & 0x03;
        const uint8_t tag = (b >> 4) & 0x0f;
        if (i + 1 + size > n) break;
        uint32_t v = 0;
        for (size_t k = 0; k < size; ++k) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        i += 1 + size;

        if (type == 1) {   /* global */
            if (tag == 0x0) g.usage_page = (uint16_t)v;
            else if (tag == 0x1) g.logical_min = pad_blank_signed(v, size);
            else if (tag == 0x2) { g.logical_max_raw = v; g.logical_max_size = size; }
            else if (tag == 0x7) g.report_size = v;
            else if (tag == 0x8) { g.report_id = (uint8_t)v; out->ids_used = true; }
            else if (tag == 0x9) g.report_count = v;
            else if (tag == 0xa) { if (sp < 4) stack[sp++] = g; }
            else if (tag == 0xb) { if (sp > 0) g = stack[--sp]; }
            continue;
        }
        if (type == 2) {   /* local */
            if (tag == 0x0) { if (usage_count < 32) usages[usage_count++] = v; }
            else if (tag == 0x1) { usage_min = v; have_min = true; }
            else if (tag == 0x2) { usage_max = v; have_max = true; }
            continue;
        }
        if (type != 0) continue;   /* reserved */

        /* A main item. ⓘ As Linux reads it: the maximum is signed only when the
         * minimum is negative, so a one-byte 0xff maximum over 0 means 255. */
        const int64_t min = g.logical_min;
        const int64_t max = min < 0 ? pad_blank_signed(g.logical_max_raw, g.logical_max_size)
                                    : (int64_t)g.logical_max_raw;
        if (tag == 0xa) {   /* collection */
            ++depth;
            const uint32_t u = usage_count ? usages[0] : 0;
            const uint32_t page = u > 0xffff ? (u >> 16) : g.usage_page;
            const uint32_t id = u & 0xffff;
            if (pad_depth == 0 && v == 0x01 && usage_count && page == 0x01 &&
                (id == 0x04 || id == 0x05 || id == 0x08)) {
                pad_depth = depth;
            }
        } else if (tag == 0xc) {   /* end collection */
            if (pad_depth == depth) pad_depth = 0;
            if (depth > 0) --depth;
        } else if (tag == 0x8) {   /* input */
            const uint32_t at = bits[g.report_id];
            const uint32_t count = g.report_count;
            const uint32_t fsize = g.report_size;
            bits[g.report_id] = at + fsize * count;
            const bool constant = (v & 0x01) != 0;
            const bool variable = (v & 0x02) != 0;
            const bool relative = (v & 0x04) != 0;
            if (pad_depth != 0 && !constant && fsize >= 1 && fsize <= 32 && count <= 1024) {
                for (uint32_t k = 0; k < count; ++k) {
                    uint32_t u;
                    if (variable) {
                        if (usage_count) u = usages[k < usage_count ? k : usage_count - 1];
                        else if (have_min) u = (have_max && usage_min + k > usage_max) ? usage_max : usage_min + k;
                        else continue;
                        const uint32_t page = u > 0xffff ? (u >> 16) : g.usage_page;
                        int64_t rest;
                        if (pad_blank_value_for(page, u & 0xffff, relative, min, max,
                                                (uint8_t)fsize, &rest)) {
                            pad_blank_add(out, g.report_id, at + k * fsize, (uint8_t)fsize, rest);
                        }
                    } else if (g.usage_page == 0x09 || g.usage_page == 0x07 || g.usage_page == 0x0c) {
                        pad_blank_add(out, g.report_id, at + k * fsize, (uint8_t)fsize, 0);
                    }
                }
            }
        }
        /* Every main item ends the local items that went before it. */
        usage_count = 0;
        have_min = have_max = false;
    }
    out->present = out->count > 0 || out->switch_layout;
    return out->present;
}

/* Write `size` bits of `value`, least significant first, at bit `bit`. */
static inline void pad_blank_put(uint8_t *d, size_t len, uint32_t bit, uint8_t size, uint32_t value)
{
    for (uint8_t k = 0; k < size; ++k) {
        const uint32_t at = bit + k;
        if ((size_t)(at >> 3) >= len) return;
        const uint8_t mask = (uint8_t)(1u << (at & 7));
        if ((value >> k) & 1u) d[at >> 3] |= mask;
        else d[at >> 3] &= (uint8_t)~mask;
    }
}

/* A Pro Controller's full report or subcommand reply, blanked in place. */
static inline void pad_blank_switch(uint8_t *d, size_t len)
{
    if (len < 12) return;
    d[3] = 0x00;               /* Y X B A, SR SL, R ZR */
    d[4] &= 0x80;              /* minus plus, stick clicks, home capture; bit 7 is the charging grip */
    d[5] = 0x00;               /* down up right left, SR SL, L ZL */
    d[6] = 0x00;               /* left stick X and Y, 0x800 each */
    d[7] = 0x08;
    d[8] = 0x80;
    d[9] = 0x00;               /* right stick X and Y */
    d[10] = 0x08;
    d[11] = 0x80;
}

/* Blank one input report in place by the plan. Anything the plan does not
 * place is left as it came. */
static inline void pad_blank_apply(const pad_blank_plan_t *p, uint8_t *data, size_t len)
{
    if (!p || !p->present || !data || len == 0) return;
    /* ⓘ A Pro Controller's reports always open with their id, whatever its
     * descriptor says about ids. */
    if (p->switch_layout && pad_blank_switch_report(data[0])) {
        pad_blank_switch(data, len);
        return;
    }
    const uint8_t id = p->ids_used ? data[0] : 0;
    const uint32_t base = p->ids_used ? 8u : 0u;
    for (uint16_t i = 0; i < p->count; ++i) {
        const pad_blank_field_t *f = &p->field[i];
        if (f->report_id != id) continue;
        pad_blank_put(data, len, base + f->bit_offset, f->bit_size, f->value);
    }
}

#endif /* PAD_BLANK_INL */
