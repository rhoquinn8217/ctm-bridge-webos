/* The d-pad, Cross and Circle (A and B on an Xbox pad) and the left stick,
 * read from a pad's own input report while the TV holds input, for a webOS
 * pop-up.
 *
 * ⭐⭐ WHY (rhoquinn8217, 2026-11-03): "I want the controller to be able to
 * answer webOS pop ups", and of a bridged one, asked whether it should: "yes,
 * do it now". A pad the TV holds reaches the app through SDL, and the app sends
 * its presses on to the pop-up as the remote's keys. A bridged one never
 * reaches SDL: its reports go to the PC, blanked while the TV holds input. So
 * for a bridged pad the core reads these out of the real report, before the
 * blank, as it reads the right stick for the overlay's volume (held_stick.inl),
 * and the app asks for them (ctm_controller_get_status, held_nav).
 *
 * ⓘ One word, read with the HELD_NAV_ macros: the d-pad as four bits, the two
 * buttons, and the left stick a byte each way, 0 to 255 with 128 at rest, Y
 * down as SDL has it. The offsets are the ones held_stick.inl and the type's
 * own blank use:
 *
 *   DualSense, Edge   0x01 (cable): LX byte 1, LY byte 2, the hat and the face
 *                     buttons byte 8 (the hat its low four bits, 0 up then
 *                     clockwise, 8 none; Cross 0x20, Circle 0x40).
 *                     0x31 (Bluetooth): one byte further along.
 *   DS4               0x01 (cable): LX 1, LY 2, the hat and buttons byte 5,
 *                     the same bits. 0x11 (Bluetooth): two bytes further along.
 *   Xbox              0x01, the report the TV builds for a cabled pad
 *                     (xpad_report.inl) and a Bluetooth pad's own: LX bytes 1
 *                     and 2, LY 3 and 4, low byte first, centre 0x8000 (the
 *                     high byte is read); the hat byte 13 (0 none, 1 up, then
 *                     clockwise to 8 up-left); A 0x01 and B 0x02 in byte 14.
 *
 * ⛔ No generic HID gamepad, as for the right stick: its descriptor says where a
 * button is but not which one it is. It still answers unbridged, through SDL.
 *
 * ⓘ Pure: no controller, no I/O. tests/test_held_nav.c runs it. */

#ifndef HELD_NAV_INL
#define HELD_NAV_INL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HELD_NAV_UP      0x01u
#define HELD_NAV_DOWN    0x02u
#define HELD_NAV_LEFT    0x04u
#define HELD_NAV_RIGHT   0x08u
#define HELD_NAV_SOUTH   0x10u   /* Cross; A on an Xbox pad */
#define HELD_NAV_EAST    0x20u   /* Circle; B */
#define HELD_NAV_KEYS(w) ((unsigned)(w) & 0x3fu)
#define HELD_NAV_LX(w)   (((unsigned)(w) >> 8) & 0xffu)
#define HELD_NAV_LY(w)   (((unsigned)(w) >> 16) & 0xffu)

/* A hat's eight directions, from up, clockwise; anything else is none. */
static inline uint32_t held_nav_dirs(unsigned from_up)
{
    static const uint8_t k[8] = {
        HELD_NAV_UP, HELD_NAV_UP | HELD_NAV_RIGHT, HELD_NAV_RIGHT, HELD_NAV_DOWN | HELD_NAV_RIGHT,
        HELD_NAV_DOWN, HELD_NAV_DOWN | HELD_NAV_LEFT, HELD_NAV_LEFT, HELD_NAV_UP | HELD_NAV_LEFT,
    };
    return from_up < 8 ? k[from_up] : 0;
}

static inline uint32_t held_nav_word(uint32_t keys, unsigned lx, unsigned ly)
{
    return keys | ((uint32_t)(lx & 0xffu) << 8) | ((uint32_t)(ly & 0xffu) << 16);
}

/* A PlayStation pad's hat and face buttons byte. */
static inline uint32_t held_nav_ps_keys(uint8_t b)
{
    uint32_t keys = held_nav_dirs(b & 0x0fu);
    if (b & 0x20) keys |= HELD_NAV_SOUTH;
    if (b & 0x40) keys |= HELD_NAV_EAST;
    return keys;
}

/* Returns false for a report that is not the pad's input report. */
static inline bool held_nav_ds5(const uint8_t *d, size_t n, uint32_t *word)
{
    if (!d || !word) return false;
    size_t o;
    if (n >= 64 && d[0] == 0x01) o = 0;
    else if (n >= 78 && d[0] == 0x31) o = 1;
    else return false;
    *word = held_nav_word(held_nav_ps_keys(d[8 + o]), d[1 + o], d[2 + o]);
    return true;
}

static inline bool held_nav_ds4(const uint8_t *d, size_t n, uint32_t *word)
{
    if (!d || !word) return false;
    size_t o;
    if (n >= 64 && d[0] == 0x01) o = 0;
    else if (n >= 78 && d[0] == 0x11) o = 2;
    else return false;
    *word = held_nav_word(held_nav_ps_keys(d[5 + o]), d[1 + o], d[2 + o]);
    return true;
}

static inline bool held_nav_xbox(const uint8_t *d, size_t n, uint32_t *word)
{
    if (!d || !word || n < 17 || d[0] != 0x01) return false;
    uint32_t keys = d[13] >= 1 && d[13] <= 8 ? held_nav_dirs((unsigned)d[13] - 1u) : 0;
    if (d[14] & 0x01) keys |= HELD_NAV_SOUTH;
    if (d[14] & 0x02) keys |= HELD_NAV_EAST;
    *word = held_nav_word(keys, d[2], d[4]);
    return true;
}

#endif /* HELD_NAV_INL */
