/* Tests for the wired Xbox pad's report: input events in, the Bluetooth report
 * the listener's Xbox map reads out.
 *
 * ⭐⭐ WHY THIS EXISTS. A wrong byte here does not fail. It presses the wrong
 * button, inverts a stick or leaves the d-pad dead on the PC, and nothing on
 * the TV would notice. Every expectation below is read off
 * CTM-USBIP maps/xbox_gip_usb_over_xbox_bt.map, which is the authority, and not
 * off the code under test.
 *
 * ➡️ Build and run:  cc tests/test_xpad_report.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

#include "../src/controllers/xpad_report.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static uint16_t u16_at(const uint8_t *report, int payload_offset)
{
    const uint8_t *p = report + 1 + payload_offset;
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void frame(xpad_state_t *s, uint8_t *report)
{
    ok(xpad_apply_event(s, EV_SYN, SYN_REPORT, 0) == 1, "SYN_REPORT completes a frame");
    ok(xpad_build_report(s, report, XPAD_BT_REPORT_LEN) == XPAD_BT_REPORT_LEN, "the report is 17 bytes");
}

static void test_at_rest(void)
{
    printf("\na pad at rest\n");
    xpad_state_t s;
    uint8_t r[XPAD_BT_REPORT_LEN];
    xpad_state_init(&s);
    frame(&s, r);
    ok(r[0] == 0x01, "report id 0x01, which the map filters on");
    ok(u16_at(r, 0) == 0x8000 && u16_at(r, 2) == 0x8000 &&
       u16_at(r, 4) == 0x8000 && u16_at(r, 6) == 0x8000,
       "sticks centred at 0x8000, which the map recentres to zero");
    ok(u16_at(r, 8) == 0 && u16_at(r, 10) == 0, "triggers released");
    ok(r[1 + 12] == 0, "hat 0: no direction");
    ok(r[1 + 13] == 0 && r[1 + 14] == 0 && r[1 + 15] == 0, "no buttons");
    ok(xpad_apply_event(&s, EV_ABS, ABS_X, 5) == 0, "an axis alone does not complete a frame");
}

static void test_buttons(void)
{
    printf("\nbuttons land on the map's bits\n");
    const struct { unsigned code; int byte; uint8_t bit; const char *name; } k[] = {
        { BTN_A, 13, 0x01, "A -> payload[13] 0x01" },
        { BTN_B, 13, 0x02, "B -> payload[13] 0x02" },
        { BTN_X, 13, 0x08, "X -> payload[13] 0x08 (bit 2 is a gap)" },
        { BTN_Y, 13, 0x10, "Y -> payload[13] 0x10" },
        { BTN_TL, 13, 0x40, "LB -> payload[13] 0x40 (bit 5 is a gap)" },
        { BTN_TR, 13, 0x80, "RB -> payload[13] 0x80" },
        { BTN_SELECT, 14, 0x04, "View -> payload[14] 0x04" },
        { BTN_START, 14, 0x08, "Menu -> payload[14] 0x08" },
        { BTN_THUMBL, 14, 0x20, "L3 -> payload[14] 0x20" },
        { BTN_THUMBR, 14, 0x40, "R3 -> payload[14] 0x40" },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); ++i) {
        xpad_state_t s;
        uint8_t r[XPAD_BT_REPORT_LEN];
        xpad_state_init(&s);
        xpad_apply_event(&s, EV_KEY, k[i].code, 1);
        frame(&s, r);
        const uint8_t other = (k[i].byte == 13) ? r[1 + 14] : r[1 + 13];
        ok(r[1 + k[i].byte] == k[i].bit && other == 0, k[i].name);
        xpad_apply_event(&s, EV_KEY, k[i].code, 0);
        frame(&s, r);
        ok(r[1 + 13] == 0 && r[1 + 14] == 0, "and released again");
    }
    xpad_state_t s;
    uint8_t r[XPAD_BT_REPORT_LEN];
    xpad_state_init(&s);
    xpad_apply_event(&s, EV_KEY, BTN_MODE, 1);
    frame(&s, r);
    ok(r[1 + 13] == 0 && r[1 + 14] == 0 && r[1 + 15] == 0, "Guide has no byte in the map and sets nothing");
}

static void test_sticks_and_triggers(void)
{
    printf("\nsticks and triggers, scaled from the driver's own ranges\n");
    xpad_state_t s;
    uint8_t r[XPAD_BT_REPORT_LEN];
    xpad_state_init(&s);
    xpad_apply_event(&s, EV_ABS, ABS_X, -32768);
    xpad_apply_event(&s, EV_ABS, ABS_Y, -32768);   /* evdev: up is negative */
    xpad_apply_event(&s, EV_ABS, ABS_RX, 32767);
    xpad_apply_event(&s, EV_ABS, ABS_RY, 32767);
    xpad_apply_event(&s, EV_ABS, ABS_Z, 1023);
    xpad_apply_event(&s, EV_ABS, ABS_RZ, 512);
    frame(&s, r);
    ok(u16_at(r, 0) == 0x0000, "left stick fully left -> LX 0");
    ok(u16_at(r, 2) == 0x0000, "left stick fully up -> LY 0 (the map's Y is down)");
    ok(u16_at(r, 4) == 0xFFFF, "right stick fully right -> RX 0xFFFF");
    ok(u16_at(r, 6) == 0xFFFF, "right stick fully down -> RY 0xFFFF");
    ok(u16_at(r, 8) == 1023, "left trigger fully pulled -> LT 1023");
    ok(u16_at(r, 10) == 512, "right trigger half pulled -> RT 512");

    xpad_state_init(&s);
    s.trigger_range[0].min = 0;
    s.trigger_range[0].max = 255;
    xpad_apply_event(&s, EV_ABS, ABS_Z, 255);
    frame(&s, r);
    ok(u16_at(r, 8) == 1023, "a driver with an 8-bit trigger still reaches 1023");

    xpad_state_init(&s);
    xpad_apply_event(&s, EV_ABS, ABS_X, 40000);
    frame(&s, r);
    ok(u16_at(r, 0) == 0xFFFF, "a value past the range is clamped, not wrapped");
}

static void test_dpad(void)
{
    printf("\nthe d-pad, as the hat ordinal hat8.to_dpad_bits reads\n");
    const struct { int x, y; uint8_t hat; const char *name; } h[] = {
        { 0, -1, 1, "up -> 1" },       { 1, -1, 2, "up-right -> 2" },
        { 1, 0, 3, "right -> 3" },     { 1, 1, 4, "down-right -> 4" },
        { 0, 1, 5, "down -> 5" },      { -1, 1, 6, "down-left -> 6" },
        { -1, 0, 7, "left -> 7" },     { -1, -1, 8, "up-left -> 8" },
        { 0, 0, 0, "centre -> 0" },
    };
    for (size_t i = 0; i < sizeof(h) / sizeof(h[0]); ++i) {
        xpad_state_t s;
        uint8_t r[XPAD_BT_REPORT_LEN];
        xpad_state_init(&s);
        xpad_apply_event(&s, EV_ABS, ABS_HAT0X, h[i].x);
        xpad_apply_event(&s, EV_ABS, ABS_HAT0Y, h[i].y);
        frame(&s, r);
        ok(r[1 + 12] == h[i].hat, h[i].name);
    }
    xpad_state_t s;
    uint8_t r[XPAD_BT_REPORT_LEN];
    xpad_state_init(&s);
    xpad_apply_event(&s, EV_KEY, BTN_TRIGGER_HAPPY2, 1);   /* right */
    xpad_apply_event(&s, EV_KEY, BTN_TRIGGER_HAPPY4, 1);   /* down */
    frame(&s, r);
    ok(r[1 + 12] == 4, "d-pad as four buttons (dpad_to_buttons): right+down -> 4");
    xpad_apply_event(&s, EV_KEY, BTN_TRIGGER_HAPPY1, 1);   /* left as well */
    frame(&s, r);
    ok(r[1 + 12] == 5, "left and right together cancel, leaving down -> 5");
}

static void test_rumble(void)
{
    printf("\nthe host's rumble report\n");
    xpad_rumble_t m;
    const uint8_t full[9] = { 0x03, 0x0F, 0x00, 0x00, 100, 50, 0xFF, 0x00, 0xFF };
    ok(xpad_parse_rumble(full, sizeof(full), &m) == 0, "report 0x03 is accepted");
    ok(m.strong / 512 == 100, "left motor 100 -> xpad sends the pad 100");
    ok(m.weak / 512 == 50, "right motor 50 -> xpad sends the pad 50");

    const uint8_t measured[9] = { 0x03, 0x0F, 0, 0, 61, 61, 0xFF, 0, 0xFF };
    ok(xpad_parse_rumble(measured, sizeof(measured), &m) == 0 && m.strong / 512 == 61 && m.weak / 512 == 61,
       "Windows' 61 (measured 2026-09-13) reaches the pad as 61, not 78");

    const uint8_t off[9] = { 0x03, 0x0F, 0, 0, 0, 0, 0, 0, 0 };
    ok(xpad_parse_rumble(off, sizeof(off), &m) == 0 && m.strong == 0 && m.weak == 0, "zero stops both");

    const uint8_t right_only[9] = { 0x03, 0x01, 0, 0, 80, 80, 0xFF, 0, 0xFF };
    ok(xpad_parse_rumble(right_only, sizeof(right_only), &m) == 0 && m.strong == 0 && m.weak / 512 == 80,
       "a motor the enable mask leaves out stays still");

    const uint8_t hot[9] = { 0x03, 0x0F, 0, 0, 127, 255, 0xFF, 0, 0xFF };
    ok(xpad_parse_rumble(hot, sizeof(hot), &m) == 0 && m.strong / 512 == 127 && m.weak / 512 == 127,
       "a level past what xpad can send is capped at 127, not wrapped");

    const uint8_t wrong[9] = { 0x02, 0x0F, 0, 0, 100, 100, 0, 0, 0 };
    ok(xpad_parse_rumble(wrong, sizeof(wrong), &m) != 0, "any other report id is refused");
    ok(xpad_parse_rumble(full, 8, &m) != 0, "a short report is refused");
}

static void test_blank(void)
{
    printf("\nblanked while the TV's overlay is open\n");
    xpad_state_t s;
    uint8_t r[XPAD_BT_REPORT_LEN];
    xpad_state_init(&s);
    xpad_apply_event(&s, EV_KEY, BTN_A, 1);
    xpad_apply_event(&s, EV_KEY, BTN_START, 1);
    xpad_apply_event(&s, EV_ABS, ABS_X, 32767);
    xpad_apply_event(&s, EV_ABS, ABS_RZ, 1023);
    xpad_apply_event(&s, EV_ABS, ABS_HAT0Y, -1);
    frame(&s, r);
    xpad_blank_report(r, sizeof(r));
    ok(r[0] == 0x01, "still report 0x01, so the host keeps receiving it");
    ok(u16_at(r, 0) == 0x8000 && u16_at(r, 2) == 0x8000 &&
       u16_at(r, 4) == 0x8000 && u16_at(r, 6) == 0x8000, "sticks centred");
    ok(u16_at(r, 8) == 0 && u16_at(r, 10) == 0, "triggers released");
    ok(r[1 + 12] == 0 && r[1 + 13] == 0 && r[1 + 14] == 0, "hat and buttons clear");
}

int main(void)
{
    test_blank();
    test_at_rest();
    test_buttons();
    test_sticks_and_triggers();
    test_dpad();
    test_rumble();

    printf("\n%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
