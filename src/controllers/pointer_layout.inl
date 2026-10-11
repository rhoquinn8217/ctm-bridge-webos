/* A bridged mouse, handed to the TV while its streaming overlay is open, as a
 * bridged keyboard is (controller_common.c, handback_sync_hold).
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-11): "When the keyboard, controller or remote
 * are bridge, their inputs still reach the streaming overlay. The mouse does
 * not." A bridged mouse stayed grabbed while the overlay was open, so the TV's
 * cursor never moved with it, and its reports went on to the PC behind the
 * overlay.
 *
 * ⓘ Pure: no controller, no I/O. It reads a report descriptor for whether the
 * device is a mouse, which report carries it, how long that report is, and
 * whether its X and Y are relative. tests/test_pointer_layout.c runs it on
 * known descriptors.
 *
 * ⚠️ Only a relative mouse is sent a report of zeros as it goes to the TV: for
 * it that says "nothing pressed, no movement", so the PC is not left holding
 * a button. An absolute pointer's zeros would put its pointer in the corner,
 * so it is handed over without one. */

#ifndef POINTER_LAYOUT_INL
#define POINTER_LAYOUT_INL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    bool present;          /* a Mouse or Pointer collection with an input in it */
    bool relative;         /* every X and Y in that report is relative, or it has none */
    uint8_t report_id;     /* 0 when its reports carry no id */
    uint16_t report_len;   /* the whole input report, the id byte counted */
} ptr_layout_t;

/* Whether the device is a mouse, from its report descriptor: an application
 * collection of Generic Desktop / Mouse or Pointer with an input in it. The
 * report is the one carrying that collection's first input. Returns true and
 * fills `out` when one was found. ⓘ Only Input items count toward the
 * report's length, per report id, as the descriptor lays them out. */
static inline bool ptr_layout_from_descriptor(const uint8_t *d, size_t n, ptr_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    if (d == NULL || n == 0) return false;

    uint32_t bits[256];
    memset(bits, 0, sizeof(bits));
    uint16_t page = 0;
    uint32_t size = 0, count = 0;
    uint8_t id = 0;
    bool ids_used = false;
    struct { uint16_t page; uint32_t size, count; uint8_t id; } stack[4];
    int sp = 0;
    /* The local usages since the last main item: the first one, a range, and
     * whether X or Y is among them. */
    uint32_t usage = 0, usage_min = 0, usage_max = 0;
    bool have_usage = false, have_min = false, have_max = false, xy = false;
    /* Collection nesting, and the depth at which a mouse collection began
     * (0 = not inside one). */
    int depth = 0, mouse_depth = 0;
    bool found = false, absolute = false;
    uint8_t found_id = 0;

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

        if (type == 1) {   /* global */
            if (tag == 0x0) page = (uint16_t)v;
            else if (tag == 0x7) size = v;
            else if (tag == 0x8) { id = (uint8_t)v; ids_used = true; }
            else if (tag == 0x9) count = v;
            else if (tag == 0xa && sp < 4) {   /* push */
                stack[sp].page = page; stack[sp].size = size;
                stack[sp].count = count; stack[sp].id = id; ++sp;
            } else if (tag == 0xb && sp > 0) {   /* pop */
                --sp; page = stack[sp].page; size = stack[sp].size;
                count = stack[sp].count; id = stack[sp].id;
            }
            continue;
        }
        if (type == 2) {   /* local. ⓘ A usage given with its page (four bytes)
                            * carries the page in its top half. */
            const uint32_t upage = v > 0xffff ? (v >> 16) : page;
            if (tag == 0x0) {
                if (!have_usage) { usage = v; have_usage = true; }
                if (upage == 0x01 && ((v & 0xffff) == 0x30 || (v & 0xffff) == 0x31)) xy = true;
            } else if (tag == 0x1) {
                usage_min = v; have_min = true;
            } else if (tag == 0x2) {
                usage_max = v; have_max = true;
            }
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
            bits[id] += size * count;
            const bool constant = (v & 0x01) != 0;
            if (mouse_depth != 0 && !constant) {
                if (!found) { found = true; found_id = id; }
                if (id == found_id) {
                    if (have_min && have_max) {
                        const uint32_t mpage = usage_min > 0xffff ? (usage_min >> 16) : page;
                        if (mpage == 0x01 && (usage_min & 0xffff) <= 0x31 && (usage_max & 0xffff) >= 0x30) {
                            xy = true;
                        }
                    }
                    if (xy && (v & 0x04) == 0) absolute = true;
                }
            }
        }
        /* Every main item ends the local usages. */
        have_usage = have_min = have_max = xy = false;
        usage = usage_min = usage_max = 0;
    }

    if (!found) return false;
    const uint16_t lead = ids_used ? 1 : 0;
    out->present = true;
    out->relative = !absolute;
    out->report_id = ids_used ? found_id : 0;
    out->report_len = (uint16_t)((bits[found_id] + 7) / 8 + lead);
    return true;
}

/* The mouse's report with nothing pressed and no movement: its id, then
 * zeros. Its length, or 0 when there is none to send: no mouse, an absolute
 * pointer, or no room. */
static inline size_t ptr_released_report(const ptr_layout_t *l, uint8_t *r, size_t cap)
{
    if (l == NULL || r == NULL || !l->present || !l->relative ||
        l->report_len == 0 || l->report_len > cap) {
        return 0;
    }
    memset(r, 0, l->report_len);
    if (l->report_id != 0) r[0] = l->report_id;
    return l->report_len;
}

#endif /* POINTER_LAYOUT_INL */
