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

/* The cable's report: the colour, claimed, and nothing else. Returns its
 * length, or 0 if `out` is too small. */
static size_t ds5_light_usb_report(uint8_t *out, size_t cap, uint8_t r, uint8_t g, uint8_t b)
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
static size_t ds5_light_bt_report(uint8_t *out, size_t cap, unsigned seq,
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

/* Does this report from the PC set the lightbar? ⓘ Only the claim counts: a
 * report that carries a colour without claiming it leaves the light alone. */
static bool ds5_report_sets_light(const uint8_t *d, size_t len)
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
