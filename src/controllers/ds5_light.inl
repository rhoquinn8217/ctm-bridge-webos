/* A DualSense's lightbar: the report the TV writes to set it, on a cable and
 * over Bluetooth, and the test of whether a report from the PC sets it.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-08). A bridged pad breathes green until the PC
 * takes its light, so the TV has to write the light on its own and see the
 * PC's first report that sets it. Both depend only on where the lightbar's
 * claim and colour sit, so they are here, apart from everything that needs a
 * pad, where a test can reach them.
 *
 * WHERE THINGS SIT. Every DualSense output report carries the same 47-byte
 * output block, whose byte 1 is the second panel of claim flags (bit 2: the
 * lightbar's colour) and whose bytes 44-46 are the colour:
 *   cable       0x02, 48 bytes: the block straight after the id.
 *   Bluetooth   0x31, 78 bytes: after the id, a sequence byte and a tag byte
 *               (0x10), and signed in its last four bytes (bt_sign.inl).
 *   Bluetooth   0x36 and 0x32 with audio: a run of [id, length, payload]
 *               blocks, and the output block is the one with id 0x90. These
 *               the TV never writes itself for the light; the PC's are read. */

#ifndef DS5_LIGHT_INL
#define DS5_LIGHT_INL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DS5L_USB_ID          0x02
#define DS5L_USB_LEN         48
#define DS5L_BT_ID           0x31
#define DS5L_BT_LEN          78
#define DS5L_BT_TAG          0x10
#define DS5L_FLAG1_LIGHT     0x04   /* claim panel 2, bit 2: the lightbar's colour */
#define DS5L_BLOCK_FLAG1     1      /* within the output block */
#define DS5L_BLOCK_RED       44
#define DS5L_USB_BLOCK       1      /* where the block starts, on a cable */
#define DS5L_BT_BLOCK        3      /* and over Bluetooth */

/* ⭐⭐ OVER BLUETOOTH THE LIGHT HAS TO BE TAKEN OVER FIRST (rhoquinn8217's C3,
 * 2026-10-09). A DualSense that connects over Bluetooth plays its own
 * connection animation, keeps that colour, and ignores every colour a host
 * writes until a host sends one "reset LED state": bit 3 of the same claim
 * panel (SDL's name for it; hid-playstation calls it releasing the LEDs).
 * SDL sends it once the pad's own clock says the animation is over, and only
 * while it is reading that pad. ⛔ A pad that reconnected mid-stream never had
 * it, and every colour the bridge drew was ignored -- no magenta, no green, no
 * yellow, no red -- while every report was written and none failed.
 * ➡️ So the bridge sends it itself (ctm_bt_signal.inl, controller_ds5.c), and
 * the app does for its own colours (ctm_ds5_bt_paint). Harmless after SDL has
 * sent it. The pad's clock is in its input report; SDL's figure for the end
 * of the animation is kept, in the clock's own units.
 * ⚠️ THAT CLOCK RUNS AT 3 MHz, NOT 1 (the C3, 2026-10-09: 629,356,040 ticks in
 * the 210.0 s between two bridges of one connection; SDL's own sensor code
 * says a third of a microsecond). So the figure is 3.4 s after the pad
 * switched on, and the pad had been on about 1.3 s when the TV first saw it.
 * Read as 10.2 s, it held the app's own colours back 11 s, and a pad bridged
 * as it connected showed no magenta. */
#define DS5L_FLAG1_LED_RESET 0x08   /* claim panel 2, bit 3: take the light over */
#define DS5L_INPUT_CLOCK     28     /* the pad's clock, 4 bytes little-endian, in a cable's 0x01 */
#define DS5L_BT_INPUT_ID     0x31   /* Bluetooth's input report, one byte further along */
#define DS5L_ANIMATION_DONE  10200000u

/* The cable's report: the colour, claimed, and nothing else. Returns its
 * length, or 0 if `out` is too small. */
static inline size_t ds5_light_usb_report(uint8_t *out, size_t cap, uint8_t r, uint8_t g, uint8_t b)
{
    if (!out || cap < DS5L_USB_LEN) return 0;
    memset(out, 0, DS5L_USB_LEN);
    out[0] = DS5L_USB_ID;
    out[DS5L_USB_BLOCK + DS5L_BLOCK_FLAG1] = DS5L_FLAG1_LIGHT;
    out[DS5L_USB_BLOCK + DS5L_BLOCK_RED + 0] = r;
    out[DS5L_USB_BLOCK + DS5L_BLOCK_RED + 1] = g;
    out[DS5L_USB_BLOCK + DS5L_BLOCK_RED + 2] = b;
    return DS5L_USB_LEN;
}

/* The Bluetooth report, UNSIGNED: the caller signs it (bt_sign_output, or
 * ctm_bt_sign_output beside the pad). `seq` is its own count, 0-15, kept by
 * the caller. */
static inline size_t ds5_light_bt_report(uint8_t *out, size_t cap, unsigned seq,
                                  uint8_t r, uint8_t g, uint8_t b)
{
    if (!out || cap < DS5L_BT_LEN) return 0;
    memset(out, 0, DS5L_BT_LEN);
    out[0] = DS5L_BT_ID;
    out[1] = (uint8_t)((seq & 0x0fu) << 4);
    out[2] = DS5L_BT_TAG;
    out[DS5L_BT_BLOCK + DS5L_BLOCK_FLAG1] = DS5L_FLAG1_LIGHT;
    out[DS5L_BT_BLOCK + DS5L_BLOCK_RED + 0] = r;
    out[DS5L_BT_BLOCK + DS5L_BLOCK_RED + 1] = g;
    out[DS5L_BT_BLOCK + DS5L_BLOCK_RED + 2] = b;
    return DS5L_BT_LEN;
}

/* The takeover: the reset bit and nothing else, UNSIGNED like the report
 * above. */
static inline size_t ds5_light_bt_takeover_report(uint8_t *out, size_t cap, unsigned seq)
{
    if (!out || cap < DS5L_BT_LEN) return 0;
    memset(out, 0, DS5L_BT_LEN);
    out[0] = DS5L_BT_ID;
    out[1] = (uint8_t)((seq & 0x0fu) << 4);
    out[2] = DS5L_BT_TAG;
    out[DS5L_BT_BLOCK + DS5L_BLOCK_FLAG1] = DS5L_FLAG1_LED_RESET;
    return DS5L_BT_LEN;
}

/* The pad's own clock, from one of its input reports: a cable's 0x01 at full
 * size, or Bluetooth's 0x31. False for any other report. */
static inline bool ds5_input_clock(const uint8_t *d, size_t len, uint32_t *clock)
{
    size_t at;
    if (!d || !clock) return false;
    if (d[0] == 0x01 && len >= 64) {
        at = DS5L_INPUT_CLOCK;
    } else if (d[0] == DS5L_BT_INPUT_ID && len >= DS5L_BT_LEN) {
        at = DS5L_INPUT_CLOCK + 1;
    } else {
        return false;
    }
    *clock = (uint32_t)d[at] | ((uint32_t)d[at + 1] << 8) |
             ((uint32_t)d[at + 2] << 16) | ((uint32_t)d[at + 3] << 24);
    return true;
}

/* What one input report says about the pad's connection animation: 1 over,
 * 0 not yet, -1 when the report does not carry the clock -- Bluetooth's
 * reduced report, before a host asks for the full one, does not. */
static inline int ds5_report_animation_over(const uint8_t *d, size_t len)
{
    uint32_t clock;
    if (!ds5_input_clock(d, len, &clock)) return -1;
    return clock >= DS5L_ANIMATION_DONE ? 1 : 0;
}

/* Does this report from the PC set the lightbar? ⓘ Only the claim counts: a
 * report that carries a colour without claiming it leaves the light alone. */
static inline bool ds5_report_sets_light(const uint8_t *d, size_t len)
{
    if (!d || len < 3) return false;
    switch (d[0]) {
    case DS5L_USB_ID:
        return len > DS5L_USB_BLOCK + DS5L_BLOCK_FLAG1 &&
               (d[DS5L_USB_BLOCK + DS5L_BLOCK_FLAG1] & DS5L_FLAG1_LIGHT) != 0;
    case DS5L_BT_ID:
        return len > DS5L_BT_BLOCK + DS5L_BLOCK_FLAG1 &&
               (d[DS5L_BT_BLOCK + DS5L_BLOCK_FLAG1] & DS5L_FLAG1_LIGHT) != 0;
    case 0x36:
    case 0x32: {
        /* The same walk ds5_patch_output makes: blocks from byte 2, the
         * signature's four bytes excluded. */
        if (len < 12) return false;
        const size_t limit = len - 4;
        size_t pos = 2;
        while (pos + 2 <= limit) {
            const uint8_t id = d[pos];
            const size_t payload = d[pos + 1];
            if (id == 0 && payload == 0) break;
            if (payload + 2 > limit - pos) break;
            if (id == 0x90 && payload > DS5L_BLOCK_FLAG1) {
                return (d[pos + 2 + DS5L_BLOCK_FLAG1] & DS5L_FLAG1_LIGHT) != 0;
            }
            pos += payload + 2;
        }
        return false;
    }
    default:
        return false;
    }
}

#endif
