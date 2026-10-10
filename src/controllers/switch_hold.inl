/* Nothing held at a switch reaches the other side.
 *
 * ⭐⭐ THE RULE (rhoquinn8217, 2026-10-09): "The correct behavior is if you
 * bridge while hold anything down should automatically cancel every input. We
 * don't want any lingering inputs being held down." A bridge switches
 * controllers on the PC: the stream's controller goes, the bridged device
 * arrives as a new one. So anything held at that moment -- a button, the
 * d-pad, a stick, a trigger, a finger on the touchpad, a key, a mouse button --
 * reaches the PC as released, and stays released until it is let go. After
 * that it works as normal. Sticks and triggers count, and so do keyboards and
 * mice (their answers to "both directions? sticks and triggers? keyboards and
 * mice?" were yes, yes, yes).
 *
 * ⓘ HOW. Each controller type describes its input report as fields: where a
 * control sits, how wide it is, its value at rest, and how far from rest still
 * counts as rest (a stick's dead zone). The first report after the switch
 * says which fields are held; every report after that has those written at
 * rest until each is seen at rest. A keyboard's array of pressed keys is
 * followed by key code. The real report is never touched: only the copy the
 * PC is sent.
 *
 * ⚠️ THE FIRST REPORT IS THE STATE AT THE SWITCH ONLY FOR A PAD THAT REPORTS
 * ALL THE TIME (a DualSense, a DS4, a cabled Xbox pad, whose report the TV
 * builds from its whole state). A keyboard, a mouse or a Bluetooth Xbox pad
 * reports only when something changes, so its first report may be a new
 * press. The pump asks the device's input nodes what is held at the switch,
 * and arms this only when something is (controller_common.c).
 *
 * ⓘ Bytes only, no I/O, so tests/test_switch_hold.c can hold it. */

#ifndef SWITCH_HOLD_INL
#define SWITCH_HOLD_INL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pad_blank.inl"

#define SWH_MAX_FIELDS 112
#define SWH_MAX_KEYS   16

typedef struct {
    uint8_t report_id;     /* the report's first byte, when ids_used */
    uint16_t min_len;      /* a shorter report does not hold this field */
    uint16_t bit_offset;   /* from the report's first byte, the id counted */
    uint8_t bit_size;      /* 1 to 32 */
    uint32_t rest;         /* its value at rest */
    uint32_t slack;        /* how far from rest still counts as at rest */
} swh_field_t;

typedef struct swh_layout {
    bool ids_used;         /* reports open with an id, and fields name theirs */
    bool truncated;        /* more fields than fit; the rest are not followed */
    uint16_t count;
    swh_field_t field[SWH_MAX_FIELDS];
    /* ⓘ A keyboard's array of pressed keys, followed by key code. */
    bool keys;
    uint8_t keys_report_id;
    uint16_t keys_offset;  /* first byte of the array, the id counted */
    uint8_t keys_count;
} swh_layout_t;

typedef struct {
    bool armed;            /* a switch happened, and its first report decides */
    uint16_t still;        /* fields and keys held at the switch, not yet let go */
    uint8_t seen[(SWH_MAX_FIELDS + 7) / 8];
    uint8_t held[(SWH_MAX_FIELDS + 7) / 8];
    bool keys_seen;
    uint8_t nkeys;
    uint8_t key[SWH_MAX_KEYS];
} swh_state_t;

static inline void swh_add(swh_layout_t *l, uint8_t report_id, uint16_t min_len, uint32_t bit,
                           uint8_t size, uint32_t rest, uint32_t slack)
{
    if (size < 1 || size > 32 || bit > 0xffff) return;
    if (l->count >= SWH_MAX_FIELDS) { l->truncated = true; return; }
    swh_field_t *f = &l->field[l->count++];
    f->report_id = report_id;
    f->min_len = min_len;
    f->bit_offset = (uint16_t)bit;
    f->bit_size = size;
    f->rest = rest;
    f->slack = slack;
}

/* Every bit of one byte, each a button at rest when clear. */
static inline void swh_add_bits(swh_layout_t *l, uint8_t report_id, uint16_t min_len,
                                uint16_t byte, uint8_t mask)
{
    for (int b = 0; b < 8; ++b) {
        if (mask & (1u << b)) swh_add(l, report_id, min_len, (uint32_t)byte * 8u + (uint32_t)b, 1, 0, 0);
    }
}

static inline uint32_t swh_get(const uint8_t *d, size_t len, uint32_t bit, uint8_t size)
{
    uint32_t v = 0;
    for (uint8_t k = 0; k < size; ++k) {
        const uint32_t at = bit + k;
        if ((size_t)(at >> 3) >= len) break;
        if (d[at >> 3] & (1u << (at & 7))) v |= 1u << k;
    }
    return v;
}

static inline void swh_put(uint8_t *d, size_t len, uint32_t bit, uint8_t size, uint32_t value)
{
    for (uint8_t k = 0; k < size; ++k) {
        const uint32_t at = bit + k;
        if ((size_t)(at >> 3) >= len) return;
        const uint8_t m = (uint8_t)(1u << (at & 7));
        if ((value >> k) & 1u) d[at >> 3] |= m;
        else d[at >> 3] &= (uint8_t)~m;
    }
}

static inline bool swh_field_in(const swh_layout_t *l, const swh_field_t *f, const uint8_t *d, size_t len)
{
    if (len < f->min_len) return false;
    if (l->ids_used && (len == 0 || d[0] != f->report_id)) return false;
    return ((size_t)f->bit_offset + f->bit_size + 7u) / 8u <= len;
}

static inline bool swh_at_rest(const swh_field_t *f, uint32_t v)
{
    const uint32_t diff = v > f->rest ? v - f->rest : f->rest - v;
    return diff <= f->slack;
}

static inline bool swh_flag(const uint8_t *set, uint16_t i) { return (set[i >> 3] >> (i & 7)) & 1u; }
static inline void swh_set(uint8_t *set, uint16_t i, bool on)
{
    if (on) set[i >> 3] |= (uint8_t)(1u << (i & 7));
    else set[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

/* A switch: the next report says what is held. */
static inline void swh_arm(swh_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->armed = true;
}

/* Something is still being kept released. */
static inline bool swh_active(const swh_state_t *s)
{
    return s->armed && s->still > 0;
}

static inline bool swh_key_in(const uint8_t *d, size_t len, const swh_layout_t *l, uint8_t code)
{
    for (uint8_t k = 0; k < l->keys_count; ++k) {
        const size_t at = (size_t)l->keys_offset + k;
        if (at < len && d[at] == code) return true;
    }
    return false;
}

static inline bool swh_keys_in(const swh_layout_t *l, const uint8_t *d, size_t len)
{
    if (!l->keys) return false;
    if (l->keys_report_id != 0 && (len == 0 || d[0] != l->keys_report_id)) return false;
    return (size_t)l->keys_offset + l->keys_count <= len;
}

/* Read one report as the device sent it: the first that holds a field says
 * whether it is held; one held is let go when it is seen at rest. Returns how
 * many fields and keys were newly found held, for the log. */
static inline int swh_note(const swh_layout_t *l, swh_state_t *s, const uint8_t *d, size_t len)
{
    if (!l || !s || !s->armed || !d || len == 0) return 0;
    int found = 0;
    for (uint16_t i = 0; i < l->count; ++i) {
        const swh_field_t *f = &l->field[i];
        if (!swh_field_in(l, f, d, len)) continue;
        const uint32_t v = swh_get(d, len, f->bit_offset, f->bit_size);
        if (!swh_flag(s->seen, i)) {
            swh_set(s->seen, i, true);
            if (!swh_at_rest(f, v)) {
                swh_set(s->held, i, true);
                ++s->still;
                ++found;
            }
        } else if (swh_flag(s->held, i) && swh_at_rest(f, v)) {
            swh_set(s->held, i, false);
            --s->still;
        }
    }
    if (swh_keys_in(l, d, len)) {
        if (!s->keys_seen) {
            s->keys_seen = true;
            for (uint8_t k = 0; k < l->keys_count && s->nkeys < SWH_MAX_KEYS; ++k) {
                const uint8_t code = d[l->keys_offset + k];
                if (code < 4) continue;   /* none, or the keyboard's error codes */
                s->key[s->nkeys++] = code;
                ++s->still;
                ++found;
            }
        } else {
            for (uint8_t k = 0; k < s->nkeys;) {
                if (!swh_key_in(d, len, l, s->key[k])) {
                    s->key[k] = s->key[--s->nkeys];
                    --s->still;
                } else {
                    ++k;
                }
            }
        }
    }
    return found;
}

/* The copy the PC is sent: every field still held from the switch at rest,
 * and every key still held taken out of the array. */
static inline void swh_apply(const swh_layout_t *l, const swh_state_t *s, uint8_t *d, size_t len)
{
    if (!l || !s || !swh_active(s) || !d || len == 0) return;
    for (uint16_t i = 0; i < l->count; ++i) {
        if (!swh_flag(s->held, i)) continue;
        const swh_field_t *f = &l->field[i];
        if (!swh_field_in(l, f, d, len)) continue;
        swh_put(d, len, f->bit_offset, f->bit_size, f->rest);
    }
    if (s->nkeys > 0 && swh_keys_in(l, d, len)) {
        uint8_t kept[256];
        uint8_t n = 0;
        for (uint8_t k = 0; k < l->keys_count; ++k) {
            const uint8_t code = d[l->keys_offset + k];
            bool drop = false;
            for (uint8_t h = 0; h < s->nkeys; ++h) {
                if (s->key[h] == code) { drop = true; break; }
            }
            if (!drop) kept[n++] = code;
        }
        for (uint8_t k = 0; k < l->keys_count; ++k) d[l->keys_offset + k] = k < n ? kept[k] : 0;
    }
}

/* ------------------------------------------------------------------------
 * The layouts. Offsets are the ones the type's own blank uses.
 * ------------------------------------------------------------------------ */

/* A DualSense or an Edge: the cable's 0x01 and Bluetooth's 0x31, one byte
 * further along (ds5_blank_input). Sticks with a dead zone, the triggers, the
 * d-pad (8 is its rest), every button (the Edge's Fn and paddles too), and the
 * two touch points (bit 7 set: no finger). */
static inline void swh_layout_ds5(swh_layout_t *l)
{
    memset(l, 0, sizeof(*l));
    l->ids_used = true;
    const struct { uint8_t id; uint8_t off; uint16_t min_len; } kinds[2] = {
        { 0x01, 0, 64 }, { 0x31, 1, 78 },
    };
    for (int k = 0; k < 2; ++k) {
        const uint8_t id = kinds[k].id, o = kinds[k].off;
        const uint16_t m = kinds[k].min_len;
        for (int s = 0; s < 4; ++s) swh_add(l, id, m, (uint32_t)(1 + s + o) * 8u, 8, 0x80, 24);
        swh_add(l, id, m, (uint32_t)(5 + o) * 8u, 8, 0, 8);          /* L2 */
        swh_add(l, id, m, (uint32_t)(6 + o) * 8u, 8, 0, 8);          /* R2 */
        swh_add(l, id, m, (uint32_t)(8 + o) * 8u, 4, 8, 0);          /* the d-pad */
        swh_add_bits(l, id, m, (uint16_t)(8 + o), 0xf0);             /* the face buttons */
        swh_add_bits(l, id, m, (uint16_t)(9 + o), 0xff);             /* shoulders, Create, Options, sticks */
        swh_add_bits(l, id, m, (uint16_t)(10 + o), 0xff);            /* PS, the pad, mute, the Edge's */
        swh_add(l, id, m, (uint32_t)(33 + o) * 8u + 7u, 1, 1, 0);    /* first touch point */
        swh_add(l, id, m, (uint32_t)(37 + o) * 8u + 7u, 1, 1, 0);    /* second touch point */
    }
}

/* A DS4: the cable's 0x01 and Bluetooth's 0x11, two bytes further along
 * (ds4_blank_input). Byte 7 is PS and the pad's click and then a counter. */
static inline void swh_layout_ds4(swh_layout_t *l)
{
    memset(l, 0, sizeof(*l));
    l->ids_used = true;
    const struct { uint8_t id; uint8_t off; uint16_t min_len; int packets; } kinds[2] = {
        { 0x01, 0, 64, 3 }, { 0x11, 2, 78, 4 },
    };
    for (int k = 0; k < 2; ++k) {
        const uint8_t id = kinds[k].id, o = kinds[k].off;
        const uint16_t m = kinds[k].min_len;
        for (int s = 0; s < 4; ++s) swh_add(l, id, m, (uint32_t)(1 + s + o) * 8u, 8, 0x80, 24);
        swh_add(l, id, m, (uint32_t)(5 + o) * 8u, 4, 8, 0);          /* the d-pad */
        swh_add_bits(l, id, m, (uint16_t)(5 + o), 0xf0);             /* the face buttons */
        swh_add_bits(l, id, m, (uint16_t)(6 + o), 0xff);
        swh_add_bits(l, id, m, (uint16_t)(7 + o), 0x03);             /* PS, the pad's click */
        swh_add(l, id, m, (uint32_t)(8 + o) * 8u, 8, 0, 8);          /* L2 */
        swh_add(l, id, m, (uint32_t)(9 + o) * 8u, 8, 0, 8);          /* R2 */
        for (int p = 0; p < kinds[k].packets; ++p) {
            const uint32_t at = 34u + o + (uint32_t)p * 9u;
            swh_add(l, id, m, (at + 1u) * 8u + 7u, 1, 1, 0);         /* finger 1 */
            swh_add(l, id, m, (at + 5u) * 8u + 7u, 1, 1, 0);         /* finger 2 */
        }
    }
}

/* An Xbox pad, cabled (the report the TV builds, xpad_report.inl) or over
 * Bluetooth (its own 0x01, read at the same offsets): four 16-bit sticks
 * centred at 0x8000, two 10-bit triggers, the d-pad as an ordinal (0 at rest)
 * and three bytes of buttons. */
static inline void swh_layout_xbox(swh_layout_t *l)
{
    memset(l, 0, sizeof(*l));
    l->ids_used = true;
    for (int s = 0; s < 4; ++s) swh_add(l, 0x01, 17, (uint32_t)(1 + 2 * s) * 8u, 16, 0x8000, 0x1800);
    swh_add(l, 0x01, 17, 9u * 8u, 16, 0, 32);                       /* LT */
    swh_add(l, 0x01, 17, 11u * 8u, 16, 0, 32);                      /* RT */
    swh_add(l, 0x01, 17, 13u * 8u, 8, 0, 0);                        /* the d-pad */
    swh_add_bits(l, 0x01, 17, 14, 0xff);
    swh_add_bits(l, 0x01, 17, 15, 0xff);
    swh_add_bits(l, 0x01, 17, 16, 0xff);
}

/* Any other gamepad: the fields its descriptor places (pad_blank.inl), at the
 * rest it gives them and with its dead zones. Movement is never held, so a
 * relative field is left out. A Pro Controller's full report and its replies
 * by their own layout: three bytes of buttons (the charging grip, byte 4's top
 * bit, is not one) and two sticks of 12-bit pairs centred at 0x800. */
static inline void swh_layout_from_pad_blank(swh_layout_t *l, const pad_blank_plan_t *p)
{
    memset(l, 0, sizeof(*l));
    if (!p || !p->present) return;
    l->ids_used = p->ids_used || p->switch_layout;
    if (p->switch_layout) {
        static const uint8_t ids[2] = { 0x21, 0x30 };
        for (int k = 0; k < 2; ++k) {
            swh_add_bits(l, ids[k], 12, 3, 0xff);
            swh_add_bits(l, ids[k], 12, 4, 0x7f);
            swh_add_bits(l, ids[k], 12, 5, 0xff);
            swh_add(l, ids[k], 12, 6u * 8u, 12, 0x800, 0x200);
            swh_add(l, ids[k], 12, 6u * 8u + 12u, 12, 0x800, 0x200);
            swh_add(l, ids[k], 12, 9u * 8u, 12, 0x800, 0x200);
            swh_add(l, ids[k], 12, 9u * 8u + 12u, 12, 0x800, 0x200);
        }
    }
    const uint32_t base = p->ids_used ? 8u : 0u;
    for (uint16_t i = 0; i < p->count; ++i) {
        const pad_blank_field_t *f = &p->field[i];
        if (f->relative) continue;
        swh_add(l, f->report_id, 0, base + f->bit_offset, f->bit_size, f->value, f->slack);
    }
}

/* A keyboard: its eight modifier bits, and its array of pressed keys
 * (kbd_chord.inl finds both). Offsets count the id byte. */
static inline void swh_layout_keyboard(swh_layout_t *l, uint8_t report_id, uint16_t mod_offset,
                                       uint16_t keys_offset, uint8_t keys_count)
{
    memset(l, 0, sizeof(*l));
    l->ids_used = report_id != 0;
    swh_add_bits(l, report_id, 0, mod_offset, 0xff);
    l->keys = keys_count > 0;
    l->keys_report_id = report_id;
    l->keys_offset = keys_offset;
    l->keys_count = keys_count;
}

/* A mouse, or any device that is neither a gamepad nor a keyboard: its
 * buttons, every one-bit Button-page input in a Mouse or Pointer collection.
 * Its movement and its wheel are never held. */
static inline void swh_layout_buttons_from_descriptor(swh_layout_t *l, const uint8_t *d, size_t n)
{
    memset(l, 0, sizeof(*l));
    if (!d || n == 0) return;
    uint32_t bits[256];
    memset(bits, 0, sizeof(bits));
    uint16_t page = 0;
    uint32_t size = 0, count = 0;
    uint8_t id = 0;
    struct { uint16_t page; uint32_t size, count; uint8_t id; } stack[4];
    int sp = 0;
    uint32_t usage = 0;
    bool have_usage = false;
    int depth = 0, mouse_depth = 0;
    size_t i = 0;
    while (i < n) {
        const uint8_t b = d[i];
        if (b == 0xfe) {   /* a long item: its data size is in the next byte */
            if (i + 1 >= n) break;
            i += 3u + d[i + 1];
            continue;
        }
        const size_t dsize = (b & 0x03) == 3 ? 4 : (b & 0x03);
        const uint8_t type = (b >> 2) & 0x03, tag = (b >> 4) & 0x0f;
        if (i + 1 + dsize > n) break;
        uint32_t v = 0;
        for (size_t k = 0; k < dsize; ++k) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        i += 1 + dsize;
        if (type == 1) {
            if (tag == 0x0) page = (uint16_t)v;
            else if (tag == 0x7) size = v;
            else if (tag == 0x8) { id = (uint8_t)v; l->ids_used = true; }
            else if (tag == 0x9) count = v;
            else if (tag == 0xa && sp < 4) { stack[sp].page = page; stack[sp].size = size; stack[sp].count = count; stack[sp].id = id; ++sp; }
            else if (tag == 0xb && sp > 0) { --sp; page = stack[sp].page; size = stack[sp].size; count = stack[sp].count; id = stack[sp].id; }
            continue;
        }
        if (type == 2) {
            if (tag == 0x0 && !have_usage) { usage = v; have_usage = true; }
            continue;
        }
        if (type != 0) continue;
        if (tag == 0xa) {   /* collection */
            ++depth;
            const uint32_t upage = usage > 0xffff ? (usage >> 16) : page;
            if (mouse_depth == 0 && v == 0x01 && have_usage && upage == 0x01 &&
                ((usage & 0xffff) == 0x02 || (usage & 0xffff) == 0x01)) {
                mouse_depth = depth;
            }
        } else if (tag == 0xc) {   /* end collection */
            if (mouse_depth == depth) mouse_depth = 0;
            if (depth > 0) --depth;
        } else if (tag == 0x8) {   /* input */
            const uint32_t at = bits[id];
            bits[id] = at + size * count;
            const bool constant = (v & 0x01) != 0, variable = (v & 0x02) != 0;
            if (mouse_depth != 0 && !constant && variable && page == 0x09 && size == 1 && count <= 32) {
                for (uint32_t k = 0; k < count; ++k) swh_add(l, id, 0, at + k, 1, 0, 0);
            }
        }
        have_usage = false;
    }
    /* ⓘ Offsets were counted after the id; an id anywhere means every report
     * opens with one. */
    if (l->ids_used) {
        for (uint16_t k = 0; k < l->count; ++k) l->field[k].bit_offset = (uint16_t)(l->field[k].bit_offset + 8u);
    }
}

#endif /* SWITCH_HOLD_INL */
