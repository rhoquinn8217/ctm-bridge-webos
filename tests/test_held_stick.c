/* Tests for reading a bridged pad's right stick and its click while the TV's
 * overlay is open (held_stick.inl), for the overlay's volume.
 *
 * ⭐ WHY THIS EXISTS. A wrong offset fails quietly and in the worst way: the
 * volume moves when nobody touched the stick, or the other way from the push,
 * or the mute follows a different button. Each pad type is checked on the
 * cable's report and Bluetooth's, at rest, all the way up and down, with its
 * click alone and with every other button in the same byte, and a report too
 * short or with another id is refused.
 *
 * ➡️ Build and run:  cc tests/test_held_stick.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

#include "../src/controllers/held_stick.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

typedef bool (*reader_t)(const uint8_t *, size_t, int *, bool *);

/* One pad type on one report: Y at `yb` (one byte, or two from `yb` when
 * `wide`), the click at byte `cb` bit `bit`. */
static void check_type(const char *name, reader_t read, uint8_t id, size_t len,
                       size_t yb, int wide, size_t cb, uint8_t bit)
{
    uint8_t r[96];
    int y = 12345;
    bool click = true;
    char what[160];

    memset(r, 0, sizeof(r));
    r[0] = id;
    if (wide) { r[yb] = 0x00; r[yb + 1] = 0x80; } else { r[yb] = 0x80; }
    snprintf(what, sizeof(what), "%s: at rest reads 0 and no click", name);
    ok(read(r, len, &y, &click) && y == 0 && !click, what);

    if (wide) { r[yb] = 0x00; r[yb + 1] = 0x00; } else { r[yb] = 0x00; }
    snprintf(what, sizeof(what), "%s: all the way up reads -32768", name);
    ok(read(r, len, &y, &click) && y == -32768, what);

    if (wide) { r[yb] = 0xff; r[yb + 1] = 0xff; } else { r[yb] = 0xff; }
    snprintf(what, sizeof(what), "%s: all the way down reads more than 32000", name);
    ok(read(r, len, &y, &click) && y > 32000, what);

    if (wide) { r[yb] = 0x00; r[yb + 1] = 0x80; } else { r[yb] = 0x80; }
    r[cb] = bit;
    snprintf(what, sizeof(what), "%s: the click alone", name);
    ok(read(r, len, &y, &click) && click && y == 0, what);

    r[cb] = 0xff;
    snprintf(what, sizeof(what), "%s: the click among every button in its byte", name);
    ok(read(r, len, &y, &click) && click, what);

    r[cb] = (uint8_t)(0xff & ~bit);
    snprintf(what, sizeof(what), "%s: every other button in that byte is no click", name);
    ok(read(r, len, &y, &click) && !click, what);

    snprintf(what, sizeof(what), "%s: a report one byte short is refused", name);
    ok(!read(r, len - 1, &y, &click), what);

    r[0] = (uint8_t)(id + 1);
    snprintf(what, sizeof(what), "%s: another report id is refused", name);
    ok(!read(r, len, &y, &click), what);
}

int main(void)
{
    /* A DualSense or an Edge: the cable's 0x01, Bluetooth's 0x31 one along. */
    check_type("DualSense, cable", held_stick_ds5, 0x01, 64, 4, 0, 9, 0x80);
    check_type("DualSense, Bluetooth", held_stick_ds5, 0x31, 78, 5, 0, 10, 0x80);

    /* A DS4: the cable's 0x01, Bluetooth's 0x11 two along. */
    check_type("DS4, cable", held_stick_ds4, 0x01, 64, 4, 0, 6, 0x80);
    check_type("DS4, Bluetooth", held_stick_ds4, 0x11, 78, 6, 0, 8, 0x80);

    /* An Xbox pad: 16-bit sticks, low byte first; R3 in byte 15. */
    check_type("Xbox", held_stick_xbox, 0x01, 17, 7, 1, 15, 0x40);

    /* ⓘ The left stick's click sits beside it and must not count. */
    {
        uint8_t r[17] = { 0x01 };
        r[8] = 0x80;
        r[15] = 0x20;                /* L3 */
        int y = 1;
        bool click = true;
        ok(held_stick_xbox(r, sizeof(r), &y, &click) && !click && y == 0,
           "Xbox: L3 is not the right stick's click");
    }
    {
        uint8_t r[64] = { 0x01 };
        r[4] = 0x80;
        r[9] = 0x40;                 /* L3 */
        int y = 1;
        bool click = true;
        ok(held_stick_ds5(r, sizeof(r), &y, &click) && !click,
           "DualSense: L3 is not the right stick's click");
        r[3] = 0x00;                 /* the right stick pushed LEFT */
        ok(held_stick_ds5(r, sizeof(r), &y, &click) && y == 0,
           "DualSense: the right stick's X does not move Y");
    }
    ok(!held_stick_ds5(NULL, 64, NULL, NULL), "no report, no reading");

    printf("\n%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
