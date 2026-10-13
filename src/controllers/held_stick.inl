/* The right stick's Y and its click, read from a pad's own input report while
 * the TV's overlay is open, for the overlay's volume.
 *
 * ⭐⭐ WHY (rhoquinn8217, 2026-10-12): "when the overlay is up i want to rebind
 * the following / right stick up -> volume up / right stick down -> volume
 * down / right stick button -> toggle mute/unmute", and it is the TV's volume,
 * from "any connected controller bridged or released". A pad the TV holds
 * reaches the overlay through SDL. A bridged one never does: its reports go to
 * the PC, blanked while the overlay holds input. So for a bridged pad the core
 * reads the two controls out of the real report, before the blank, and the app
 * asks for them (ctm_controller_get_status, held_stick).
 *
 * ⓘ The offsets are the ones the type's own blank and switch_hold.inl's layouts
 * use. Y comes back the way SDL gives a stick, from -32768 (all the way up) to
 * 32767 (all the way down), whatever the report's own width:
 *
 *   DualSense, Edge   0x01 (cable): Y byte 4, R3 byte 9 bit 7.
 *                     0x31 (Bluetooth): one byte further along.
 *   DS4               0x01 (cable): Y byte 4, R3 byte 6 bit 7.
 *                     0x11 (Bluetooth): two bytes further along.
 *   Xbox              0x01, the report the TV builds for a cabled pad
 *                     (xpad_report.inl) and a Bluetooth pad's own: Y bytes 7
 *                     and 8, low byte first, centre 0x8000; R3 byte 15 bit 6.
 *
 * ⛔ No generic HID gamepad. Its descriptor says where a stick is but not which
 * button is the stick's click, so it is left out rather than guessed. It still
 * works unbridged, through SDL.
 *
 * ⓘ Pure: no controller, no I/O. tests/test_held_stick.c runs it. */

#ifndef HELD_STICK_INL
#define HELD_STICK_INL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A stick byte, 0 up to 255 down, as SDL's range. */
static inline int held_stick_from_u8(uint8_t v)
{
    return ((int)v - 128) * 256;
}

/* Returns false for a report that is not the pad's input report. */
static inline bool held_stick_ds5(const uint8_t *d, size_t n, int *y, bool *click)
{
    if (!d || !y || !click) return false;
    size_t o;
    if (n >= 64 && d[0] == 0x01) o = 0;
    else if (n >= 78 && d[0] == 0x31) o = 1;
    else return false;
    *y = held_stick_from_u8(d[4 + o]);
    *click = (d[9 + o] & 0x80) != 0;
    return true;
}

static inline bool held_stick_ds4(const uint8_t *d, size_t n, int *y, bool *click)
{
    if (!d || !y || !click) return false;
    size_t o;
    if (n >= 64 && d[0] == 0x01) o = 0;
    else if (n >= 78 && d[0] == 0x11) o = 2;
    else return false;
    *y = held_stick_from_u8(d[4 + o]);
    *click = (d[6 + o] & 0x80) != 0;
    return true;
}

static inline bool held_stick_xbox(const uint8_t *d, size_t n, int *y, bool *click)
{
    if (!d || !y || !click || n < 17 || d[0] != 0x01) return false;
    *y = (int)((unsigned)d[7] | ((unsigned)d[8] << 8)) - 32768;
    *click = (d[15] & 0x40) != 0;
    return true;
}

#endif /* HELD_STICK_INL */
