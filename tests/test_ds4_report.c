/* Tests for a DualShock 4's report bytes: the unbridge chord, blanking while the
 * TV's overlay is open, and the output report the TV's confirmation writes.
 *
 * ⭐⭐ WHY THIS EXISTS. A wrong offset does not fail. It reads the report's
 * counter as a button, leaves a finger on the touchpad while the overlay is
 * open, or paints the wrong field -- and nothing on the TV would notice. Every
 * expectation below is read off the Linux hid-playstation driver's DS4 structs
 * and a report read off a real cabled pad, and not off the code under test.
 *
 * ➡️ Build and run:  cc tests/test_ds4_report.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

#include "../src/controllers/ds4_report.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* ⭐ READ OFF THE PAD: a cabled DS4 at rest, 64 bytes, report id first. */
static const uint8_t k_live[] = {
    0x01, 0x7c, 0x80, 0x85, 0x81, 0x08, 0x00, 0xe4, 0x00, 0x00, 0x85, 0x95, 0x16, 0xfd, 0xff, 0x02,
    0x00, 0xfd, 0xff, 0xa5, 0xff, 0x7b, 0x1f, 0x79, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1b, 0x00,
    0x00, 0x01, 0x8b, 0xa4, 0x26, 0xf0, 0x22, 0xa2, 0x84, 0x60, 0x16, 0x00, 0x80, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
};

/* The first finger of each touch packet, and the second, in the USB report. */
static const size_t k_usb_fingers[] = { 35, 39, 44, 48, 53, 57 };
/* The same over Bluetooth: two further along, and a fourth packet. */
static const size_t k_bt_fingers[] = { 37, 41, 46, 50, 55, 59, 64, 68 };

/* A Bluetooth 0x11 report carrying the live report's fields: the id, the two
 * header bytes, the USB report's [1..63] at [3..65], the rest of the fourth
 * touch packet at rest, and four CRC bytes nothing should touch. */
static void bt_from_live(uint8_t *bt)
{
    memset(bt, 0, DS4_BT_INPUT_LEN);
    bt[0] = 0x11;
    bt[1] = 0xc0;
    bt[2] = 0x00;
    memcpy(bt + 3, k_live + 1, sizeof(k_live) - 1);
    bt[68] = 0x80;                           /* the fourth packet's second finger */
    bt[74] = 0xde; bt[75] = 0xad; bt[76] = 0xbe; bt[77] = 0xef;
}

static void test_live_report(void)
{
    printf("\nthe report read off a cabled pad\n");
    size_t off = 99;
    int packets = 0;
    ok(sizeof(k_live) == DS4_USB_INPUT_LEN, "it is 64 bytes");
    ok(ds4_pad_report(k_live, sizeof(k_live), &off, &packets) && off == 0 && packets == 3,
       "a whole USB pad report: fields at their own offsets, three touch packets");
    ok(k_live[5] == 0x08, "[5] is 0x08 at rest: hat centred, no face buttons");
    ok((k_live[7] & 0x03) == 0 && (k_live[7] & 0xfc) != 0,
       "[7] has PS and the click clear while its counter bits are not");
    ok(k_live[33] == 0x01, "[33] counts one touch packet");
    ok((k_live[35] & 0x80) && (k_live[39] & 0x80), "[35] and [39] say no finger (0xa4, 0xa2)");
    ok(!ds4_chord_held(k_live, sizeof(k_live)), "the pad at rest is NOT holding the chord");
}

static void test_chord_usb(void)
{
    printf("\nthe chord over a cable: two fingers and the click\n");
    uint8_t r[DS4_USB_INPUT_LEN];

    memcpy(r, k_live, sizeof(r));
    r[35] &= 0x7f; r[39] &= 0x7f; r[7] |= 0x02;
    ok(ds4_chord_held(r, sizeof(r)), "fingers at [35] and [39], click at [7] 0x02 -> held");

    memcpy(r, k_live, sizeof(r));
    r[35] &= 0x7f; r[7] |= 0x02;
    ok(!ds4_chord_held(r, sizeof(r)), "one finger and the click -> not held");

    memcpy(r, k_live, sizeof(r));
    r[39] &= 0x7f; r[7] |= 0x02;
    ok(!ds4_chord_held(r, sizeof(r)), "the other finger alone and the click -> not held");

    memcpy(r, k_live, sizeof(r));
    r[35] &= 0x7f; r[39] &= 0x7f;
    ok(!ds4_chord_held(r, sizeof(r)), "two fingers without the click -> not held");

    memcpy(r, k_live, sizeof(r));
    r[35] &= 0x7f; r[39] &= 0x7f; r[7] = 0xfd;
    ok(!ds4_chord_held(r, sizeof(r)), "every counter bit and PS set, click clear -> not held");

    memcpy(r, k_live, sizeof(r));
    r[7] |= 0x02;
    ok(!ds4_chord_held(r, sizeof(r)), "the click with no fingers -> not held");

    memcpy(r, k_live, sizeof(r));
    r[44] &= 0x7f; r[48] &= 0x7f; r[7] |= 0x02;
    ok(!ds4_chord_held(r, sizeof(r)), "fingers only in the second packet -> the chord reads the first");

    memcpy(r, k_live, sizeof(r));
    r[33] = 0x00; r[35] &= 0x7f; r[39] &= 0x7f; r[7] |= 0x02;
    ok(ds4_chord_held(r, sizeof(r)), "[33] is a count, not a finger: zero packets still reads the first");

    memcpy(r, k_live, sizeof(r));
    r[35] &= 0x7f; r[39] &= 0x7f; r[7] |= 0x02;
    ok(!ds4_chord_held(r, sizeof(r) - 1), "a report one byte short is refused");
    ok(!ds4_chord_held(r, 10), "a cut-down 10-byte 0x01 is refused");
    ok(!ds4_chord_held(NULL, sizeof(r)), "no report is refused");
    r[0] = 0x05;
    ok(!ds4_chord_held(r, sizeof(r)), "report id 0x05 is refused");
    r[0] = 0x11;
    ok(!ds4_chord_held(r, sizeof(r)), "0x11 at 64 bytes is too short to be the Bluetooth report");
}

static void test_chord_bt(void)
{
    printf("\nthe chord over Bluetooth: the same fields two bytes along\n");
    uint8_t bt[DS4_BT_INPUT_LEN];
    size_t off = 0;
    int packets = 0;

    bt_from_live(bt);
    ok(ds4_pad_report(bt, sizeof(bt), &off, &packets) && off == 2 && packets == 4,
       "a whole 0x11 report: fields two along, four touch packets");
    ok(!ds4_chord_held(bt, sizeof(bt)), "the live fields at rest -> not held");

    bt_from_live(bt);
    bt[37] &= 0x7f; bt[41] &= 0x7f; bt[9] |= 0x02;
    ok(ds4_chord_held(bt, sizeof(bt)), "fingers at [37] and [41], click at [9] -> held");

    bt_from_live(bt);
    bt[35] &= 0x7f; bt[39] &= 0x7f; bt[7] |= 0x02;
    ok(!ds4_chord_held(bt, sizeof(bt)), "the USB positions over Bluetooth -> not held");

    bt_from_live(bt);
    bt[37] &= 0x7f; bt[41] &= 0x7f; bt[9] |= 0x02;
    ok(!ds4_chord_held(bt, sizeof(bt) - 1), "a 77-byte 0x11 is refused");
    bt[0] = 0x12;
    ok(!ds4_chord_held(bt, sizeof(bt)), "report id 0x12 is refused");
    bt[0] = 0x31;
    ok(!ds4_chord_held(bt, sizeof(bt)), "a DualSense's 0x31 is refused");
}

static void test_blank_live(void)
{
    printf("\nthe live report blanked\n");
    uint8_t r[DS4_USB_INPUT_LEN];
    memcpy(r, k_live, sizeof(r));
    ds4_blank_input(r, sizeof(r));
    ok(r[5] == 0x08, "[5] is 0x08");
    ok(r[6] == 0x00, "[6] is 0");
    ok((r[7] & 0xfc) == (k_live[7] & 0xfc), "the counter in [7] is unchanged");
    ok((r[7] & 0x03) == 0, "PS and the click are clear");
    int fingers = 1;
    for (size_t i = 0; i < sizeof(k_usb_fingers) / sizeof(k_usb_fingers[0]); ++i) {
        if (r[k_usb_fingers[i]] != 0x80) fingers = 0;
    }
    ok(fingers, "every finger in the three packets is 0x80");
    ok(r[1] == 0x80 && r[2] == 0x80 && r[3] == 0x80 && r[4] == 0x80, "the sticks are 0x80");
}

static void test_blank_usb(void)
{
    printf("\nblanked over a cable, with everything pressed\n");
    uint8_t r[DS4_USB_INPUT_LEN];
    memcpy(r, k_live, sizeof(r));
    r[1] = 0x00; r[2] = 0xff; r[3] = 0x10; r[4] = 0xf0;
    r[5] = 0xf3;                             /* every face button, hat 3 */
    r[6] = 0xff;
    r[7] = 0xe7;                             /* PS and the click, counter 0x39 */
    r[8] = 0xff; r[9] = 0x80;
    for (size_t i = 0; i < sizeof(k_usb_fingers) / sizeof(k_usb_fingers[0]); ++i) {
        r[k_usb_fingers[i]] = 0x05;          /* a finger down, tracking number 5 */
    }
    uint8_t before[DS4_USB_INPUT_LEN];
    memcpy(before, r, sizeof(r));

    ds4_blank_input(r, sizeof(r));
    ok(r[1] == 0x80 && r[2] == 0x80 && r[3] == 0x80 && r[4] == 0x80, "sticks [1..4] centred at 0x80");
    ok(r[5] == 0x08, "[5] 0x08: hat centred, faces clear");
    ok(r[6] == 0x00, "[6] shoulders, share, options, L3, R3 clear");
    ok(r[7] == 0xe4, "[7] PS and click clear, counter bits kept (0xe7 -> 0xe4)");
    ok(r[8] == 0x00 && r[9] == 0x00, "both analog triggers released");
    int fingers = 1;
    for (size_t i = 0; i < sizeof(k_usb_fingers) / sizeof(k_usb_fingers[0]); ++i) {
        if (r[k_usb_fingers[i]] != 0x80) fingers = 0;
    }
    ok(fingers, "fingers at [35] [39] [44] [48] [53] [57] all read no finger");
    ok(memcmp(r + 10, before + 10, 33 - 10) == 0, "timestamp, temperature, gyro, accel, status untouched");
    ok(r[33] == before[33], "the packet count untouched");
    ok(r[34] == before[34] && r[43] == before[43] && r[52] == before[52], "the packets' timestamps untouched");
    ok(memcmp(r + 36, before + 36, 3) == 0 && memcmp(r + 40, before + 40, 3) == 0,
       "the finger coordinates untouched");
    ok(memcmp(r + 58, before + 58, 64 - 58) == 0, "the tail after the last finger untouched");

    memcpy(r, before, sizeof(r));
    ds4_blank_input(r, sizeof(r) - 1);
    ok(memcmp(r, before, sizeof(r)) == 0, "a report one byte short is left alone");
    r[0] = 0x05;
    ds4_blank_input(r, sizeof(r));
    ok(r[1] == before[1] && r[5] == before[5] && r[7] == before[7], "another report id is left alone");
    ds4_blank_input(NULL, sizeof(r));
    ok(1, "no report does not crash");
}

static void test_blank_bt(void)
{
    printf("\nblanked over Bluetooth\n");
    uint8_t bt[DS4_BT_INPUT_LEN];
    bt_from_live(bt);
    bt[3] = 0x00; bt[4] = 0xff; bt[5] = 0x00; bt[6] = 0xff;
    bt[7] = 0x62;                            /* cross and circle, hat 2 */
    bt[8] = 0x30;
    bt[9] = 0xe7;
    bt[10] = 0xff; bt[11] = 0xff;
    for (size_t i = 0; i < sizeof(k_bt_fingers) / sizeof(k_bt_fingers[0]); ++i) {
        bt[k_bt_fingers[i]] = 0x07;
    }
    uint8_t before[DS4_BT_INPUT_LEN];
    memcpy(before, bt, sizeof(bt));

    ds4_blank_input(bt, sizeof(bt));
    ok(bt[1] == 0xc0 && bt[2] == 0x00, "the two header bytes untouched");
    ok(bt[3] == 0x80 && bt[4] == 0x80 && bt[5] == 0x80 && bt[6] == 0x80, "sticks [3..6] centred");
    ok(bt[7] == 0x08 && bt[8] == 0x00, "[7] hat centred and faces clear, [8] clear");
    ok(bt[9] == 0xe4, "[9] PS and click clear, counter kept");
    ok(bt[10] == 0x00 && bt[11] == 0x00, "triggers [10] [11] released");
    int fingers = 1;
    for (size_t i = 0; i < sizeof(k_bt_fingers) / sizeof(k_bt_fingers[0]); ++i) {
        if (bt[k_bt_fingers[i]] != 0x80) fingers = 0;
    }
    ok(fingers, "fingers in all four packets read no finger");
    ok(memcmp(bt + 12, before + 12, 35 - 12) == 0, "motion, battery and status untouched");
    ok(bt[35] == before[35] && bt[36] == before[36], "the count and the first timestamp untouched");
    ok(memcmp(bt + 74, before + 74, 4) == 0, "the CRC bytes untouched");
}

static void test_output_report(void)
{
    printf("\nthe output report 0x05 the TV writes\n");
    uint8_t out[DS4_OUT_LEN + 4];
    memset(out, 0xaa, sizeof(out));
    ok(ds4_build_output(out, sizeof(out), DS4_OUT_MOTORS | DS4_OUT_LIGHT, 0x11, 0x22, 0x33, 0x44, 0x55)
       == 32, "the report is 32 bytes");
    ok(out[0] == 0x05, "[0] report id 0x05");
    ok(out[1] == 0x03, "[1] valid flags: motors 0x01 and colour 0x02");
    ok(out[2] == 0x00 && out[3] == 0x00, "[2] second flags and [3] reserved stay zero");
    ok(out[4] == 0x11, "[4] the weak (right) motor");
    ok(out[5] == 0x22, "[5] the strong (left) motor");
    ok(out[6] == 0x33 && out[7] == 0x44 && out[8] == 0x55, "[6..8] red, green, blue");
    int zero = 1;
    for (size_t i = 9; i < DS4_OUT_LEN; ++i) {
        if (out[i] != 0) zero = 0;
    }
    ok(zero, "[9..31] blink timings and the tail are zero");
    ok(out[DS4_OUT_LEN] == 0xaa, "nothing written past byte 31");

    ds4_build_output(out, sizeof(out), DS4_OUT_LIGHT, 0x7f, 0x7f, 0xff, 0xff, 0x00);
    ok(out[1] == 0x02 && out[4] == 0 && out[5] == 0 && out[6] == 0xff && out[7] == 0xff && out[8] == 0,
       "colour only: the motor bytes stay zero whatever was passed");
    ds4_build_output(out, sizeof(out), DS4_OUT_MOTORS, 0x7f, 0x7f, 0xff, 0xff, 0x00);
    ok(out[1] == 0x01 && out[4] == 0x7f && out[5] == 0x7f && out[6] == 0 && out[7] == 0 && out[8] == 0,
       "motors only: the colour bytes stay zero");
    ds4_build_output(out, sizeof(out), DS4_OUT_MOTORS, 0, 0, 0, 0, 0);
    ok(out[1] == 0x01 && out[4] == 0 && out[5] == 0, "a stop claims the motors, at zero");
    ok(ds4_build_output(out, DS4_OUT_LEN - 1, DS4_OUT_LIGHT, 0, 0, 1, 2, 3) == 0,
       "a 31-byte buffer is refused");
}

static void test_host_output(void)
{
    printf("\nthe host's 0x05, while the TV's signal plays\n");
    uint8_t h[DS4_OUT_LEN];
    uint32_t rgb = 0;

    memset(h, 0, sizeof(h));
    h[0] = 0x05; h[1] = 0xf7; h[4] = 0x40; h[5] = 0x60; h[6] = 0x12; h[7] = 0x34; h[8] = 0x56;
    h[9] = 0x10; h[10] = 0x20; h[19] = 0x50;
    ok(ds4_host_output(h, sizeof(h)), "a whole 0x05 is the host's output report");
    ok(ds4_output_colour(h, sizeof(h), &rgb) && rgb == 0x123456, "its colour reads 0x123456");
    ok(ds4_withhold_output(h, sizeof(h), DS4_OUT_MOTORS | DS4_OUT_LIGHT) == 0x03,
       "withholding motors and colour takes 0x03");
    ok(h[1] == 0xf4, "blink 0x04 and the audio flags 0xf0 stay the host's");
    ok(h[4] == 0x40 && h[5] == 0x60 && h[6] == 0x12 && h[9] == 0x10 && h[19] == 0x50,
       "the bytes behind the flags are left as sent");
    ok(!ds4_output_colour(h, sizeof(h), &rgb), "with its colour flag gone it sets no colour");

    memset(h, 0, sizeof(h));
    h[0] = 0x05; h[1] = 0x01; h[4] = 0x80;
    ok(!ds4_output_colour(h, sizeof(h), &rgb), "a rumble-only report sets no colour");
    ok(ds4_withhold_output(h, sizeof(h), DS4_OUT_LIGHT) == 0 && h[1] == 0x01,
       "withholding the colour from it takes nothing");
    ok(ds4_withhold_output(h, sizeof(h), DS4_OUT_MOTORS) == 0x01 && h[1] == 0x00,
       "withholding the motors takes 0x01");

    memset(h, 0, sizeof(h));
    h[0] = 0x05; h[1] = 0x03;
    ok(!ds4_host_output(h, sizeof(h) - 1) && ds4_withhold_output(h, sizeof(h) - 1, 0x03) == 0 &&
       h[1] == 0x03, "a 31-byte report is left alone");
    h[0] = 0x11;
    ok(!ds4_output_colour(h, sizeof(h), &rgb) && ds4_withhold_output(h, sizeof(h), 0x03) == 0 &&
       h[1] == 0x03, "another report id is left alone");
}

static void test_breath(void)
{
    printf("\nthe breath's brightness\n");
    ok(ds4_breath_level(0, 700, 1) == 0, "one breath starts dark");
    ok(ds4_breath_level(350, 700, 1) == 255, "and is brightest at its middle");
    const uint8_t quarter = ds4_breath_level(175, 700, 1);
    ok(quarter >= 126 && quarter <= 128, "half bright a quarter of the way in");
    ok(ds4_breath_level(680, 700, 1) <= 15, "nearly dark by its last step");
    ok(ds4_breath_level(700, 700, 1) == 0, "dark once it is over");
    ok(ds4_breath_level(200, 800, 2) == 255 && ds4_breath_level(600, 800, 2) == 255,
       "two breaths peak at 200 and 600 of 800");
    ok(ds4_breath_level(400, 800, 2) == 0, "and are dark between them");
    ok(ds4_breath_level(-5, 700, 1) == 0 && ds4_breath_level(10, 0, 1) == 0 &&
       ds4_breath_level(10, 700, 0) == 0, "nonsense asks for dark");
}

/* ⭐ READ OFF THE PAD: its feature report 0x12, whole, on the C1 (2026-09-15).
 * SDL on the C1 names this pad `30-0e-d5-a9-69-51`, and the U5s's kernel
 * `30:0e:d5:a9:69:51`. */
static const uint8_t k_live_pairing[DS4_FEATURE_PAIRING_INFO_LEN] = {
    0x12, 0x51, 0x69, 0xa9, 0xd5, 0x0e, 0x30, 0x08, 0x25, 0x00, 0x7e, 0x8f, 0xd1, 0x7e, 0x61, 0xe8,
};

static void test_pairing_info(void)
{
    printf("\nthe MAC out of the pairing-info report\n");
    uint8_t r[DS4_FEATURE_PAIRING_INFO_LEN];
    char mac[24];
    memcpy(r, k_live_pairing, sizeof(r));
    ok(ds4_mac_from_pairing_info(r, sizeof(r), mac, sizeof(mac)) &&
       strcmp(mac, "30:0e:d5:a9:69:51") == 0, "the live reply gives the pad's own MAC, as SDL and the kernel name it");

    ok(!ds4_mac_from_pairing_info(r, 6, mac, sizeof(mac)) && mac[0] == '\0',
       "a reply too short for six bytes gives nothing");
    ok(!ds4_mac_from_pairing_info(r, sizeof(r), mac, 17) && mac[0] == '\0',
       "a buffer too small for the text gives nothing");

    r[0] = 0x09;
    ok(!ds4_mac_from_pairing_info(r, sizeof(r), mac, sizeof(mac)), "another report's reply is refused");

    memset(r, 0, sizeof(r));
    r[0] = 0x12;
    r[9] = 0x42;
    ok(!ds4_mac_from_pairing_info(r, sizeof(r), mac, sizeof(mac)) && mac[0] == '\0',
       "six zero bytes are no MAC, whatever follows");

    r[6] = 0x01;
    ok(ds4_mac_from_pairing_info(r, sizeof(r), mac, sizeof(mac)) &&
       strcmp(mac, "01:00:00:00:00:00") == 0, "one non-zero byte is enough");
}

int main(void)
{
    test_live_report();
    test_chord_usb();
    test_chord_bt();
    test_blank_live();
    test_blank_usb();
    test_blank_bt();
    test_output_report();
    test_host_output();
    test_breath();
    test_pairing_info();

    printf("\n%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
