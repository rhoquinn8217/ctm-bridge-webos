/* The streaming overlay's keyboard shortcut, Ctrl+Alt+Shift+O, found in a
 * bridged keyboard's own reports.
 *
 * ⭐ WHY IT IS HERE AND NOT IN THE APP (rhoquinn8217, 2026-09-13): the shortcut
 * works bridged or not. A keyboard the TV reads reaches Aurora's own shortcut
 * handling; a BRIDGED keyboard is grabbed, so its keys never reach Aurora at
 * all, and only the bridge sees them.
 *
 * ⓘ Pure: no controller, no I/O. It reads a report descriptor for where the
 * keyboard's modifier byte and key array sit, and a report for whether the
 * shortcut is down. tests/test_kbd_chord.c runs it on known descriptors.
 *
 * ⚠️ The layout it understands is the standard one -- eight modifier bits and a
 * byte array of pressed keys, with or without a report id -- which is what boot
 * keyboards and nearly every dongle use. A keyboard that reports its keys as a
 * bitmap (some "n-key rollover" modes) is not recognised and simply has no
 * shortcut while bridged. */

#ifndef KBD_CHORD_INL
#define KBD_CHORD_INL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    bool present;          /* a keyboard collection with both fields was found */
    uint8_t report_id;     /* 0 when its reports carry no id */
    uint16_t mod_offset;   /* byte of the eight modifier bits, the id byte counted */
    uint16_t keys_offset;  /* first byte of the pressed-key array, the id counted */
    uint8_t keys_count;    /* how many key bytes */
    uint16_t report_len;   /* the whole input report, the id byte counted */
} kbd_layout_t;

#define KBD_USAGE_O 0x12   /* keyboard usage page: "o" and "O" */

/* Where the keyboard's modifier bits and key array are, from its report
 * descriptor. Returns true and fills `out` when both were found in one report.
 * ⓘ Only Input items count; the offsets are counted per report id, as the
 * descriptor lays them out. */
static inline bool kbd_layout_from_descriptor(const uint8_t *d, size_t n, kbd_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    if (d == NULL || n == 0) return false;

    uint32_t bits[256];
    memset(bits, 0, sizeof(bits));
    uint16_t usage_page = 0;
    uint32_t report_size = 0, report_count = 0;
    uint8_t report_id = 0;
    bool ids_used = false;
    uint32_t usage = 0, usage_min = 0, usage_max = 0;
    bool have_usage = false, have_min = false, have_max = false;
    /* Collection nesting, and the depth at which a keyboard application
     * collection began (0 = not inside one). */
    int depth = 0, keyboard_depth = 0;
    bool mods_found = false, keys_found = false;
    uint8_t mods_id = 0, keys_id = 0;
    uint16_t mod_byte = 0, keys_byte = 0;
    uint8_t keys_n = 0;

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

        if (type == 1) {   /* global */
            if (tag == 0x0) usage_page = (uint16_t)v;
            else if (tag == 0x7) report_size = v;
            else if (tag == 0x8) { report_id = (uint8_t)v; ids_used = true; }
            else if (tag == 0x9) report_count = v;
        } else if (type == 2) {   /* local */
            if (tag == 0x0) { usage = v; have_usage = true; }
            else if (tag == 0x1) { usage_min = v; have_min = true; }
            else if (tag == 0x2) { usage_max = v; have_max = true; }
        } else if (type == 0) {   /* main */
            if (tag == 0xa) {   /* collection */
                ++depth;
                /* An application collection whose usage is Generic Desktop /
                 * Keyboard. ⓘ A usage given with its page (four bytes) carries
                 * the page in its top half. */
                const uint32_t page = (have_usage && usage > 0xffff) ? (usage >> 16) : usage_page;
                const uint32_t id = usage & 0xffff;
                if (keyboard_depth == 0 && v == 0x01 && have_usage && page == 0x01 && id == 0x06) {
                    keyboard_depth = depth;
                }
            } else if (tag == 0xc) {   /* end collection */
                if (keyboard_depth == depth) keyboard_depth = 0;
                if (depth > 0) --depth;
            } else if (tag == 0x8) {   /* input */
                const uint32_t at = bits[report_id];
                const uint32_t width = report_size * report_count;
                const bool variable = (v & 0x02) != 0;
                const bool constant = (v & 0x01) != 0;
                if (keyboard_depth != 0 && !constant && usage_page == 0x07 && (at % 8) == 0) {
                    if (!mods_found && variable && report_size == 1 && report_count == 8 &&
                        have_min && have_max && usage_min == 0xe0 && usage_max == 0xe7) {
                        mods_found = true;
                        mods_id = report_id;
                        mod_byte = (uint16_t)(at / 8);
                    } else if (!keys_found && !variable && report_size == 8 &&
                               report_count >= 1 && report_count <= 32) {
                        keys_found = true;
                        keys_id = report_id;
                        keys_byte = (uint16_t)(at / 8);
                        keys_n = (uint8_t)report_count;
                    }
                }
                bits[report_id] = at + width;
            }
            have_usage = have_min = have_max = false;
            usage = usage_min = usage_max = 0;
        }
        i += 1 + size;
    }

    if (!mods_found || !keys_found || mods_id != keys_id) return false;
    const uint16_t lead = ids_used ? 1 : 0;
    out->present = true;
    out->report_id = ids_used ? mods_id : 0;
    out->mod_offset = (uint16_t)(mod_byte + lead);
    out->keys_offset = (uint16_t)(keys_byte + lead);
    out->keys_count = keys_n;
    out->report_len = (uint16_t)((bits[mods_id] + 7) / 8 + lead);
    return true;
}

/* Is Ctrl+Alt+Shift+O down in this report? Either Ctrl, either Alt, either
 * Shift. ⓘ False for a report of another id or too short to hold the fields. */
static inline bool kbd_overlay_chord_down(const kbd_layout_t *l, const uint8_t *r, size_t n)
{
    if (l == NULL || !l->present || r == NULL) return false;
    if (l->report_id != 0 && (n < 1 || r[0] != l->report_id)) return false;
    if (n < (size_t)l->keys_offset + l->keys_count || n <= l->mod_offset) return false;
    const uint8_t mods = r[l->mod_offset];
    const bool ctrl = (mods & 0x11) != 0;
    const bool shift = (mods & 0x22) != 0;
    const bool alt = (mods & 0x44) != 0;
    if (!(ctrl && shift && alt)) return false;
    for (uint8_t k = 0; k < l->keys_count; ++k) {
        if (r[l->keys_offset + k] == KBD_USAGE_O) return true;
    }
    return false;
}

/* The same report with nothing pressed: every byte after the id cleared. Sent
 * to the host in place of the one that completed the shortcut, so it is never
 * left holding Ctrl, Alt or Shift. */
static inline void kbd_released_report(const kbd_layout_t *l, uint8_t *r, size_t n)
{
    if (l == NULL || r == NULL || n == 0) return;
    const size_t lead = l->report_id != 0 ? 1 : 0;
    if (n > lead) memset(r + lead, 0, n - lead);
}

#endif /* KBD_CHORD_INL */
