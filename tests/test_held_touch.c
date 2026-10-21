/* Tests for reading a bridged pad's touchpad while the TV's overlay is open
 * (held_touch.inl), for the TV's pointer.
 *
 * ⭐ WHY THIS EXISTS. A wrong offset fails quietly: the pointer runs the wrong
 * way, jumps, or clicks when nobody pressed. The fixture is a real report,
 * recorded from a DualSense Edge through its DS5Dongle on the K5S on
 * 2026-10-20, with one finger down at 1199, 840; the other cases change only
 * the bytes they are about.
 *
 * ➡️ Build and run:  cc tests/test_held_touch.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

#include "../src/controllers/held_touch.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static const uint8_t k_recorded[64] = {
        0x01, 0x7e, 0x7e, 0x7e, 0x80, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x00, 0xdc, 0x53, 0x1b, 0x1e,
        0xff, 0xff, 0x00, 0x00, 0x02, 0x00, 0x8c, 0xff, 0xaf, 0x1f, 0x7c, 0x05, 0x29, 0x0c, 0x1e, 0x4d,
        0x1b, 0x0d, 0xaf, 0x84, 0x34, 0x82, 0xe4, 0x60, 0x34, 0xc1, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x1c, 0x00, 0x08, 0x03, 0x0a, 0x20, 0x00, 0x8d, 0x24, 0xfc, 0xa1, 0x0d, 0x7d, 0x2f, 0xdc,
};

int main(void)
{
    uint8_t r[80];
    uint32_t w = 0;

    printf("the recorded report: one finger at 1199, 840\n");
    memcpy(r, k_recorded, sizeof(k_recorded));
    ok(held_touch_ds5(r, 64, &w), "read");
    ok(HELD_TOUCH_FINGERS(w) == 1, "one finger");
    ok(HELD_TOUCH_X(w) == 1199, "X 1199");
    ok(HELD_TOUCH_Y(w) == 840, "Y 840");
    ok(HELD_TOUCH_ID(w) == 13, "its contact count, 13");
    ok(!HELD_TOUCH_CLICK(w), "not pressed in");

    printf("the touchpad pressed in (byte 10 bit 1), with the other buttons in its byte\n");
    memcpy(r, k_recorded, 64);
    r[10] = 0x02;
    ok(held_touch_ds5(r, 64, &w) && HELD_TOUCH_CLICK(w), "the click alone");
    r[10] = 0xFF;
    ok(held_touch_ds5(r, 64, &w) && HELD_TOUCH_CLICK(w), "the click among the others");
    r[10] = 0xFD;
    ok(held_touch_ds5(r, 64, &w) && !HELD_TOUCH_CLICK(w), "every other button, no click");

    printf("no finger down: both contact bytes with bit 7 set\n");
    memcpy(r, k_recorded, 64);
    r[33] |= 0x80;
    ok(held_touch_ds5(r, 64, &w), "read");
    ok(HELD_TOUCH_FINGERS(w) == 0, "no finger");

    printf("the second touch alone, at the far corner\n");
    memcpy(r, k_recorded, 64);
    r[33] |= 0x80;
    r[37] = 0x22;                       /* down, count 34 */
    r[38] = 0x7F;                       /* X 1919 = 0x77F */
    r[39] = (uint8_t)(0x7 | (0x7 << 4)); /* X's high 7, Y's low 7 */
    r[40] = 0x43;                       /* Y 1079 = 0x437 */
    ok(held_touch_ds5(r, 64, &w), "read");
    ok(HELD_TOUCH_FINGERS(w) == 1, "one finger");
    ok(HELD_TOUCH_X(w) == 1919 && HELD_TOUCH_Y(w) == 1079, "1919, 1079");
    ok(HELD_TOUCH_ID(w) == 34, "its count, 34");

    printf("two fingers: the first touch is the one given\n");
    r[33] = 0x0d;
    ok(held_touch_ds5(r, 64, &w), "read");
    ok(HELD_TOUCH_FINGERS(w) == 2, "two fingers");
    ok(HELD_TOUCH_X(w) == 1199 && HELD_TOUCH_Y(w) == 840, "the first touch's place");

    printf("refused: not measured, or not the input report\n");
    memcpy(r, k_recorded, 64);
    ok(!held_touch_ds5(r, 63, &w), "a report one byte short");
    r[0] = 0x31;
    ok(!held_touch_ds5(r, 78, &w), "Bluetooth's 0x31: not measured yet");
    r[0] = 0x05;
    ok(!held_touch_ds5(r, 64, &w), "another report id");
    ok(!held_touch_ds5(NULL, 64, &w), "no report");

    printf("the packing holds every field at its widest\n");
    w = held_touch_pack(2047, 2047, 127, 3, true);
    ok(HELD_TOUCH_X(w) == 2047 && HELD_TOUCH_Y(w) == 2047 && HELD_TOUCH_ID(w) == 127 &&
       HELD_TOUCH_FINGERS(w) == 3 && HELD_TOUCH_CLICK(w), "all ones back");
    w = held_touch_pack(0, 1079, 0, 0, false);
    ok(HELD_TOUCH_X(w) == 0 && HELD_TOUCH_Y(w) == 1079 && !HELD_TOUCH_CLICK(w), "Y alone");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
