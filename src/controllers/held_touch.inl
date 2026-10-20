/* A pad's touchpad, read from its own input report while the TV's overlay
 * holds input, for the TV's pointer.
 *
 * ⭐⭐ WHY (rhoquinn8217, 2026-10-20): "All touchpads on ds5/ds5e/ds4 control
 * the mouse in the dashboard and when the streaming overlay is up. The
 * controllers can be bridge or released." A pad the TV holds reaches the
 * pointer through SDL. A bridged one never does: its reports go to the PC,
 * with the touch blanked while the overlay holds input. So for a bridged pad
 * the core reads where the finger is out of the real report, before the
 * blank, and the app asks for it (ctm_controller_get_status, held_touch).
 *
 * ⭐ MEASURED, 2026-10-20, on the K5S: a DualSense Edge through its DS5Dongle,
 * report 0x01 (the cable layout), 4,570 reports with a finger down. The first
 * touch is four bytes from 33, the second four from 37:
 *   - the contact byte: bit 7 CLEAR while a finger is down (the offsets the
 *     chord and the blank already use), its low 7 bits a count that changes
 *     with each new touch;
 *   - X: byte 34, and the low four bits of byte 35 above it: 52 to 1919 seen;
 *   - Y: the high four bits of byte 35, and byte 36 above them: 0 to 1079 seen.
 * No jump inside any one touch; the other way of packing the same bytes gives
 * 0 to 4095, which no pad of 1920 across has. The touchpad's click is byte 10
 * bit 1, as the chord reads it.
 *
 * ⛔ NOT YET the DualSense over Bluetooth (report 0x31) or the DS4: their touch
 * bytes have not been captured, and the protocol here comes only from this
 * project's own observation (README, "Clean-room"). Their reports are refused,
 * so a bridged one does not move the pointer, rather than moving it wrongly.
 *
 * ⓘ Packed in one 32-bit word, so a reader on another thread always has the
 * parts of one report together: X bits 0-10, Y bits 11-21, the contact count
 * bits 22-28, how many fingers bits 29-30, the click bit 31.
 *
 * ⓘ Pure: no controller, no I/O. tests/test_held_touch.c runs it. */

#ifndef HELD_TOUCH_INL
#define HELD_TOUCH_INL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HELD_TOUCH_X(w)       ((int)((w) & 0x7FFu))
#define HELD_TOUCH_Y(w)       ((int)(((w) >> 11) & 0x7FFu))
#define HELD_TOUCH_ID(w)      ((int)(((w) >> 22) & 0x7Fu))
#define HELD_TOUCH_FINGERS(w) ((int)(((w) >> 29) & 0x3u))
#define HELD_TOUCH_CLICK(w)   (((w) >> 31) != 0)

static inline uint32_t held_touch_pack(int x, int y, int id, int fingers, bool click)
{
    return ((uint32_t)x & 0x7FFu) | (((uint32_t)y & 0x7FFu) << 11) | (((uint32_t)id & 0x7Fu) << 22) |
           (((uint32_t)fingers & 0x3u) << 29) | (click ? 0x80000000u : 0u);
}

/* One touch's four bytes from `at`: true while its finger is down. */
static inline bool held_touch_point(const uint8_t *d, size_t at, int *x, int *y, int *id)
{
    if (d[at] & 0x80) return false;
    *id = d[at] & 0x7F;
    *x = d[at + 1] | ((d[at + 2] & 0x0F) << 8);
    *y = (d[at + 2] >> 4) | (d[at + 3] << 4);
    return true;
}

/* A DualSense's or an Edge's report, the cable layout only (above). Returns
 * false for a report it does not read. With no finger down the word says so
 * (fingers 0) and still carries the click. */
static inline bool held_touch_ds5(const uint8_t *d, size_t n, uint32_t *word)
{
    if (!d || !word || n < 64 || d[0] != 0x01) return false;
    int x1 = 0, y1 = 0, id1 = 0, x2 = 0, y2 = 0, id2 = 0;
    const bool f1 = held_touch_point(d, 33, &x1, &y1, &id1);
    const bool f2 = held_touch_point(d, 37, &x2, &y2, &id2);
    const bool click = (d[10] & 0x02) != 0;
    const int fingers = (f1 ? 1 : 0) + (f2 ? 1 : 0);
    if (f1) {
        *word = held_touch_pack(x1, y1, id1, fingers, click);
    } else if (f2) {
        *word = held_touch_pack(x2, y2, id2, fingers, click);
    } else {
        *word = held_touch_pack(0, 0, 0, 0, click);
    }
    return true;
}

#endif /* HELD_TOUCH_INL */
