/* A DualShock 4's reports as bytes: the unbridge chord read out of an input
 * report, that report blanked while the TV's overlay is open, the output
 * report the TV's own confirmation signal writes, and the pad's MAC out of its
 * pairing-info feature report.
 *
 * ⭐⭐ WHERE THE OFFSETS COME FROM. Read three ways and compared before any of
 * this was written: the Linux hid-playstation driver's structs
 * (dualshock4_input_report_usb and _bt, dualshock4_output_report_usb), a live
 * 64-byte report read off a cabled pad, and the DualSense layout beside them.
 * tests/test_ds4_report.c checks every one against that live report.
 *
 *   USB input report 0x01, 64 bytes, counting the report id as byte 0
 *   [1..4]    LX LY RX RY, centred 0x80
 *   [5]       low nibble the d-pad hat, 8 centred; high nibble square 0x10,
 *             cross 0x20, circle 0x40, triangle 0x80
 *   [6]       L1 0x01 R1 0x02 L2 0x04 R2 0x08 share 0x10 options 0x20
 *             L3 0x40 R3 0x80
 *   [7]       PS 0x01, touchpad click 0x02 -- ⛔ the top six bits are a counter
 *             and never a button
 *   [8] [9]   L2 and R2, analog
 *   [10..32]  timestamp, temperature, gyro, accelerometer, battery and status
 *   [33]      how many touch packets follow -- ⛔ a count, not a finger
 *   [34..60]  three touch packets of nine bytes: a timestamp, then two fingers
 *             of four bytes each, led by a contact byte whose 0x80 bit means
 *             NO finger
 *
 *   Bluetooth input report 0x11, 78 bytes: two header bytes after the id, so
 *   every field above sits two further along -- the click at [9], the first
 *   packet's fingers at [37] and [41]. ⚠️ And it carries FOUR touch packets,
 *   not three (the driver's own struct: "BT has 4 compared to 3 for USB"),
 *   then a CRC32 in the last four bytes.
 *
 *   USB output report 0x05, 32 bytes, no checksum over a cable
 *   [1]       valid flags: motors 0x01, lightbar colour 0x02, blink 0x04
 *   [2] [3]   a second flags byte and a reserved one, both left at zero
 *   [4] [5]   weak (right) and strong (left) motor, 0..255
 *   [6..8]    red, green, blue     [9] [10] blink on and off
 *
 * ⚠️ A MOTOR RUNS UNTIL A REPORT STOPS IT. Nothing here times anything out, so
 * whatever starts a pulse owns sending the stop.
 *
 * ⓘ Pure: no file descriptors, no threads. controller_ds4.c does the I/O. */

#ifndef DS4_REPORT_INL
#define DS4_REPORT_INL

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DS4_USB_INPUT_ID       0x01
#define DS4_USB_INPUT_LEN      64
#define DS4_BT_INPUT_ID        0x11
#define DS4_BT_INPUT_LEN       78
/* The Bluetooth report's two header bytes: every field sits this much further
 * along than it does over a cable. */
#define DS4_BT_OFFSET          2

/* Positions in the USB input report. Add DS4_BT_OFFSET for Bluetooth. */
#define DS4_IN_LX              1      /* then LY, RX, RY */
#define DS4_IN_HAT             5      /* the hat, with the face buttons above it */
#define DS4_IN_BUTTONS         6
#define DS4_IN_PS              7      /* PS, the touchpad click, then a counter */
#define DS4_IN_L2              8
#define DS4_IN_R2              9
#define DS4_IN_TOUCH           34     /* the first touch packet */

#define DS4_TOUCH_PACKET_LEN   9      /* a timestamp, then two fingers of four */
#define DS4_TOUCH_FINGER_1     1      /* contact bytes, within a packet */
#define DS4_TOUCH_FINGER_2     5
#define DS4_USB_TOUCH_PACKETS  3
#define DS4_BT_TOUCH_PACKETS   4

#define DS4_STICK_CENTRED      0x80
#define DS4_HAT_CENTRED        0x08   /* 0 would read as "up" */
#define DS4_BTN_PS             0x01
#define DS4_BTN_PAD_CLICK      0x02
#define DS4_NO_FINGER          0x80

#define DS4_OUT_ID             0x05
#define DS4_OUT_LEN            32
#define DS4_OUT_FLAGS          1
#define DS4_OUT_WEAK           4
#define DS4_OUT_STRONG         5
#define DS4_OUT_RED            6
#define DS4_OUT_GREEN          7
#define DS4_OUT_BLUE           8
/* ⭐⭐ THE BLUETOOTH OUTPUT REPORT. Everything the cabled 0x05 carries, two
 * bytes further along, behind a 0x11 header and with a Sony CRC on the end.
 *
 * ⓘ These offsets are not derived: ds4_bt_send_audio has been writing the
 * volume bytes of this exact report since T-229, and the pad obeys it --
 * heard 2026-09-22, silent at 20 and audible at 100.
 * ⛔ Byte 1 is 0xC0: HID bit ON, because this IS an effects report. The pure
 * audio reports (0x12/0x14/0x17) have it OFF. */
#define DS4_BT_OUT_ID          0x11
#define DS4_BT_OUT_LEN         78
#define DS4_BT_OUT_FLAGS       3
#define DS4_BT_OUT_WEAK        6
#define DS4_BT_OUT_STRONG      7
#define DS4_BT_OUT_RED         8
#define DS4_BT_OUT_GREEN       9
#define DS4_BT_OUT_BLUE        10

#define DS4_OUT_MOTORS         0x01
#define DS4_OUT_LIGHT          0x02
#define DS4_OUT_BLINK          0x04

/* Feature report 0x12, the pad's pairing info: its own MAC in [1..6],
 * little-endian. ⓘ The Linux driver's DS4_FEATURE_REPORT_PAIRING_INFO, 16
 * bytes counting the id, and SDL's "serial number" report. */
#define DS4_FEATURE_PAIRING_INFO      0x12
#define DS4_FEATURE_PAIRING_INFO_LEN  16

/* Is this a whole pad report, and where do its fields sit? `off` is 0 for the
 * USB report and DS4_BT_OFFSET for Bluetooth; `packets` is how many touch
 * packets that format carries. Either may be NULL.
 *
 * ⛔ False for a report too short to be the whole thing and for every other id
 * -- Bluetooth's cut-down 10-byte 0x01 before the pad is asked for its full
 * report, say -- and then nothing below reads or writes it. ⓘ The lengths are
 * the driver's own: it takes a report of exactly these sizes and no other. */
static bool ds4_pad_report(const uint8_t *data, size_t len, size_t *off, int *packets)
{
    if (!data || len == 0) return false;
    if (data[0] == DS4_USB_INPUT_ID && len >= DS4_USB_INPUT_LEN) {
        if (off) *off = 0;
        if (packets) *packets = DS4_USB_TOUCH_PACKETS;
        return true;
    }
    if (data[0] == DS4_BT_INPUT_ID && len >= DS4_BT_INPUT_LEN) {
        if (off) *off = DS4_BT_OFFSET;
        if (packets) *packets = DS4_BT_TOUCH_PACKETS;
        return true;
    }
    return false;
}

/* The unbridge chord: TWO fingers on the touchpad AND the touchpad pressed
 * down, the same chord a DualSense uses.
 *
 * ⓘ Only the contact byte's top bit is meaningful. A finger clears it and the
 * low bits become a tracking number, so a resting pad reads 0x80 or, as on the
 * live report, 0xa4 -- no finger either way.
 *
 * ⚠️ THE FIRST TOUCH PACKET, WHATEVER [33] SAYS. The count is how many packets
 * the report carries; the driver reads only that many. ⓘ Whether a pad ever
 * reports zero while fingers rest on it has NOT been measured, and gating on
 * the count would turn such a report into "fingers lifted" and restart the
 * hold -- the fault the DualSense's audio reports once caused. Reading the
 * first packet regardless risks at worst a stale finger, and a chord must
 * still stay clicked for the whole hold to count. */
static bool ds4_chord_held(const uint8_t *data, size_t len)
{
    size_t off = 0;
    if (!ds4_pad_report(data, len, &off, NULL)) return false;
    const bool pressed = (data[DS4_IN_PS + off] & DS4_BTN_PAD_CLICK) != 0;
    const bool finger1 = (data[DS4_IN_TOUCH + DS4_TOUCH_FINGER_1 + off] & DS4_NO_FINGER) == 0;
    const bool finger2 = (data[DS4_IN_TOUCH + DS4_TOUCH_FINGER_2 + off] & DS4_NO_FINGER) == 0;
    return pressed && finger1 && finger2;
}

/* ⭐⭐ BLANK A DS4 INPUT REPORT: nothing pressed, sticks centred, triggers
 * released, no fingers on the touchpad. Used while the TV's overlay is open --
 * see blank_input in ctm_controller.h for why a blank report is sent rather
 * than none, and ds5_blank_input for the DualSense's.
 *
 * ⛔ [7] IS HALF BUTTONS, HALF COUNTER. Only PS and the touchpad click are
 * cleared; zeroing the byte would stop the counter the host may be watching.
 *
 * ⓘ Every finger in every touch packet reads "no finger", so a game reading
 * the touchpad sees the fingers lift rather than freeze where they were. The
 * chord is read from the real report before this runs.
 *
 * ⓘ Everything not named is left alone: the gyro and accelerometer, the
 * timestamps, the battery and the packet count all keep moving, so the host
 * sees a live controller doing nothing rather than a stalled one. ⚠️ Over
 * Bluetooth the report's CRC is not recomputed, the same as the DualSense's
 * Bluetooth blanking; the host's map copies the fields and never checks it. */
static void ds4_blank_input(uint8_t *data, size_t len)
{
    size_t off = 0;
    int packets = 0;
    if (!ds4_pad_report(data, len, &off, &packets)) return;

    for (size_t i = 0; i < 4; ++i) data[DS4_IN_LX + i + off] = DS4_STICK_CENTRED;
    data[DS4_IN_HAT + off] = DS4_HAT_CENTRED;          /* hat centred, faces up */
    data[DS4_IN_BUTTONS + off] = 0x00;
    data[DS4_IN_PS + off] &= (uint8_t)~(DS4_BTN_PS | DS4_BTN_PAD_CLICK);
    data[DS4_IN_L2 + off] = 0x00;
    data[DS4_IN_R2 + off] = 0x00;

    for (int p = 0; p < packets; ++p) {
        const size_t at = DS4_IN_TOUCH + off + (size_t)p * DS4_TOUCH_PACKET_LEN;
        data[at + DS4_TOUCH_FINGER_1] = DS4_NO_FINGER;
        data[at + DS4_TOUCH_FINGER_2] = DS4_NO_FINGER;
    }
}

/* Build the USB output report 0x05: the valid flags, both motors and the
 * lightbar colour. Returns its length, or 0 if `cap` is too small.
 *
 * ⚠️ A FIELD WHOSE FLAG IS CLEAR STAYS ZERO, whatever was passed for it, so a
 * report never carries a value nothing claimed. Blink is never set: the flag
 * byte is the caller's, and a blink claim with zero timings is a real change. */
static size_t ds4_build_output(uint8_t *out, size_t cap, uint8_t flags,
                               uint8_t weak, uint8_t strong,
                               uint8_t r, uint8_t g, uint8_t b)
{
    if (!out || cap < DS4_OUT_LEN) return 0;
    memset(out, 0, DS4_OUT_LEN);
    out[0] = DS4_OUT_ID;
    out[DS4_OUT_FLAGS] = flags;
    if (flags & DS4_OUT_MOTORS) {
        out[DS4_OUT_WEAK] = weak;
        out[DS4_OUT_STRONG] = strong;
    }
    if (flags & DS4_OUT_LIGHT) {
        out[DS4_OUT_RED] = r;
        out[DS4_OUT_GREEN] = g;
        out[DS4_OUT_BLUE] = b;
    }
    return DS4_OUT_LEN;
}

/* The same report for a Bluetooth pad.
 * ⚠⚠ IT IS NOT SIGNED HERE, AND IT MUST BE SIGNED. The pad drops any output
 * report whose CRC does not match, silently -- a missing one looks exactly
 * like a light that does not work. ctm_bt_sign_output lives in
 * controller_common.c, which this file's unit test does not link, so the one
 * caller signs instead. 🔗 ds4_signal_write. */
static size_t ds4_bt_build_output(uint8_t *out, size_t cap, uint8_t flags,
                                  uint8_t weak, uint8_t strong,
                                  uint8_t r, uint8_t g, uint8_t b,
                                  uint8_t headphone_vol, uint8_t speaker_vol)
{
    if (!out || cap < DS4_BT_OUT_LEN) return 0;
    memset(out, 0, DS4_BT_OUT_LEN);
    out[0] = DS4_BT_OUT_ID;
    out[1] = 0xc0;                 /* HID bit on: an effects report */
    out[2] = 0xa0;
    /* ⛔⛔ THE VOLUME BITS ARE CARRIED, NOT LEFT CLEAR.
     *
     * Byte 3's HIGH bits are the volume-valid flags -- 0x10/0x20 headphone L/R,
     * 0x80 speaker -- and ds4_patch_output says what they mean: "without them
     * the pad ignores bytes 21/22/24". ⚠️ This report was sending them CLEAR
     * with the volume bytes zeroed, which is not "leave the volumes alone" so
     * much as an effects report that mentions no audio at all.
     *
     * ⭐ MEASURED 2026-09-23 on the C3: with the light and the pulse OFF the
     * tone played in 5 of 5 runs; with them ON, in 5 of 10, and the RELEASE
     * tone -- the one that follows two of these reports -- was the worst of
     * all. The bridge tone, which plays BEFORE any of them, was the better one.
     * ➡️ So the light carries the pad's audio settings with it and leaves
     * them as it found them. */
    out[DS4_BT_OUT_FLAGS] = (uint8_t)(flags | 0xb0u);
    out[21] = headphone_vol;
    out[22] = headphone_vol;
    out[24] = speaker_vol;
    if (flags & DS4_OUT_MOTORS) {
        out[DS4_BT_OUT_WEAK] = weak;
        out[DS4_BT_OUT_STRONG] = strong;
    }
    if (flags & DS4_OUT_LIGHT) {
        out[DS4_BT_OUT_RED] = r;
        out[DS4_BT_OUT_GREEN] = g;
        out[DS4_BT_OUT_BLUE] = b;
    }
    return DS4_BT_OUT_LEN;
}

/* Is this the host's USB output report 0x05, whole? ⓘ Anything else the host
 * sends passes through the patcher untouched. */
static bool ds4_host_output(const uint8_t *data, size_t len)
{
    return data && len >= DS4_OUT_LEN && data[0] == DS4_OUT_ID;
}

/* The lightbar colour a host report sets, as 0xRRGGBB. False when the report
 * does not claim the lightbar: its colour bytes then mean nothing. */
static bool ds4_output_colour(const uint8_t *data, size_t len, uint32_t *rgb)
{
    if (!ds4_host_output(data, len) || !(data[DS4_OUT_FLAGS] & DS4_OUT_LIGHT)) return false;
    if (rgb) {
        *rgb = ((uint32_t)data[DS4_OUT_RED] << 16) |
               ((uint32_t)data[DS4_OUT_GREEN] << 8) |
               (uint32_t)data[DS4_OUT_BLUE];
    }
    return true;
}

/* Take `bits` out of a host report's valid flags, so the pad ignores those
 * fields of it and keeps what the TV last wrote. Returns the bits actually
 * taken. ⓘ Everything else stays exactly as the host sent it -- the other
 * flags, the audio volumes they govern, and the bytes behind the taken flags,
 * which the pad no longer reads. */
static uint8_t ds4_withhold_output(uint8_t *data, size_t len, uint8_t bits)
{
    if (!ds4_host_output(data, len)) return 0;
    const uint8_t taken = (uint8_t)(data[DS4_OUT_FLAGS] & bits);
    data[DS4_OUT_FLAGS] = (uint8_t)(data[DS4_OUT_FLAGS] & ~bits);
    return taken;
}

/* The pad's own MAC, out of its pairing-info reply, as "aa:bb:cc:dd:ee:ff".
 * False, with `out` left empty, for a reply too short, one echoing another id,
 * or one whose six MAC bytes are all zero.
 *
 * ⭐⭐ WHY A CABLED DS4 NEEDS IT. The kernel fills `uniq` with the MAC only
 * where its driver asks for this report, and the C1's does not: a cabled DS4
 * there has `uniq=-`, so the host was told nothing and a config could not link
 * to the pad. The U5s's kernel reads it, and gave `30:0e:d5:a9:69:51`.
 *
 * ⓘ WHERE THE LAYOUT COMES FROM, three ways that agree: the Linux driver copies
 * [1..6] into the MAC and prints it last byte first; SDL formats [6] down to
 * [1]; and on the C1 SDL read `30-0e-d5-a9-69-51` for the same pad the U5s's
 * kernel named `30:0e:d5:a9:69:51`.
 * ✅ AND MEASURED on the C1, 2026-09-15, with a one-off probe on that pad:
 *
 *     12 51 69 a9 d5 0e 30  08 25 00  7e 8f d1 7e 61 e8
 *
 * The id, the MAC last byte first, then `08 25 00` -- the same constant a
 * DualSense's report 0x09 carries -- and six bytes that are most likely the
 * paired host's address, as they are on a DualSense. Only [1..6] is parsed.
 *
 * ⛔ A ZERO MAC IS NOT AN ANSWER: every pad that gave one would share it, the
 * collision the host's config store refuses. */
static bool ds4_mac_from_pairing_info(const uint8_t *reply, size_t len, char *out, size_t out_len)
{
    if (out && out_len) out[0] = '\0';
    if (!reply || !out || out_len < 18 || len < 7 || reply[0] != DS4_FEATURE_PAIRING_INFO) {
        return false;
    }
    bool any = false;
    for (size_t i = 1; i <= 6; ++i) {
        if (reply[i]) any = true;
    }
    if (!any) return false;
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             reply[6], reply[5], reply[4], reply[3], reply[2], reply[1]);
    return true;
}

/* The lightbar's brightness `at_ms` into a signal of `breaths` rises and falls
 * spread evenly across `total_ms`: 0 at the start and end of each breath, 255
 * at its middle, a straight ramp between, like the DualSense's wired breath.
 * ⓘ Asked of the clock, so a late step lands at the brightness its moment
 * deserves rather than shifting the whole shape. */
static uint8_t ds4_breath_level(long at_ms, long total_ms, int breaths)
{
    if (at_ms < 0 || total_ms <= 0 || breaths <= 0 || at_ms >= total_ms) return 0;
    const long span = total_ms / breaths;
    const long half = span / 2;
    if (half <= 0) return 0;
    const long within = at_ms % span;
    long level = (within < half) ? (within * 255) / half : ((span - within) * 255) / half;
    if (level > 255) level = 255;
    return (uint8_t)level;
}

#endif /* DS4_REPORT_INL */
