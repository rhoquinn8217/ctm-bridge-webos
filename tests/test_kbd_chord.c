/* Tests for the overlay's keyboard shortcut in a bridged keyboard's reports.
 *
 * ⭐ WHY THIS EXISTS. A wrong offset does not fail loudly: the shortcut simply
 * never opens the overlay, or a key the user meant for the PC is swallowed.
 * The descriptors below are the HID specification's boot keyboard (Appendix
 * B.1) and the same keyboard behind a report id, as dongles send it.
 *
 * ➡️ Build and run:  cc tests/test_kbd_chord.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

#include "../src/controllers/kbd_chord.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* HID 1.11 Appendix B.1, the boot keyboard. */
static const uint8_t k_boot[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,             /* modifiers */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,             /* reserved byte */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,             /* LED padding */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, /* six keys */
    0xc0,
};

/* The same keyboard as report 2, after a consumer-control report 1 that must not
 * shift its offsets. */
static const uint8_t k_with_ids[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x01,
    0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x00, 0xc0,
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x02,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xc0,
};

/* A three-button mouse: no keyboard collection at all. */
static const uint8_t k_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0,
};

int main(void)
{
    kbd_layout_t l;

    printf("=== the boot keyboard ===\n");
    ok(kbd_layout_from_descriptor(k_boot, sizeof(k_boot), &l), "found");
    ok(l.report_id == 0, "no report id");
    ok(l.mod_offset == 0, "modifiers in byte 0");
    ok(l.keys_offset == 2, "keys from byte 2, after the reserved byte");
    ok(l.keys_count == 6, "six keys");
    ok(l.report_len == 8, "an eight-byte report");

    uint8_t r[8] = {0x01 | 0x04 | 0x02, 0, 0x12, 0, 0, 0, 0, 0};   /* LCtrl LAlt LShift + o */
    ok(kbd_overlay_chord_down(&l, r, sizeof(r)), "left Ctrl, Alt and Shift with O");
    uint8_t right[8] = {0x10 | 0x40 | 0x20, 0, 0, 0, 0x12, 0, 0, 0};
    ok(kbd_overlay_chord_down(&l, right, sizeof(right)), "right Ctrl, Alt and Shift, O in a later slot");
    uint8_t no_alt[8] = {0x01 | 0x02, 0, 0x12, 0, 0, 0, 0, 0};
    ok(!kbd_overlay_chord_down(&l, no_alt, sizeof(no_alt)), "not without Alt");
    uint8_t just_o[8] = {0, 0, 0x12, 0, 0, 0, 0, 0};
    ok(!kbd_overlay_chord_down(&l, just_o, sizeof(just_o)), "not O alone");
    uint8_t other[8] = {0x07, 0, 0x16, 0, 0, 0, 0, 0};   /* s, which Moonlight uses */
    ok(!kbd_overlay_chord_down(&l, other, sizeof(other)), "not another letter");
    ok(!kbd_overlay_chord_down(&l, r, 2), "not a report too short for the keys");

    uint8_t rel[8];
    memcpy(rel, r, sizeof(rel));
    kbd_released_report(&l, rel, sizeof(rel));
    static const uint8_t zero8[8];
    ok(memcmp(rel, zero8, sizeof(rel)) == 0, "released: every byte cleared");

    printf("=== a keyboard behind report id 2 ===\n");
    ok(kbd_layout_from_descriptor(k_with_ids, sizeof(k_with_ids), &l), "found");
    ok(l.report_id == 2, "report id 2");
    ok(l.mod_offset == 1, "modifiers after the id byte");
    ok(l.keys_offset == 3, "keys after the id and the reserved byte");
    ok(l.report_len == 9, "a nine-byte report, id counted");
    uint8_t r2[9] = {0x02, 0x07, 0, 0x12, 0, 0, 0, 0, 0};
    ok(kbd_overlay_chord_down(&l, r2, sizeof(r2)), "the shortcut in report 2");
    uint8_t r1[9] = {0x01, 0x07, 0, 0x12, 0, 0, 0, 0, 0};
    ok(!kbd_overlay_chord_down(&l, r1, sizeof(r1)), "not in the consumer report 1");
    kbd_released_report(&l, r2, sizeof(r2));
    ok(r2[0] == 0x02 && r2[1] == 0 && r2[3] == 0, "released: the id kept, the rest cleared");

    printf("=== a mouse ===\n");
    ok(!kbd_layout_from_descriptor(k_mouse, sizeof(k_mouse), &l), "no keyboard");
    ok(!l.present, "nothing present");
    uint8_t m[3] = {0x07, 0x12, 0x12};
    ok(!kbd_overlay_chord_down(&l, m, sizeof(m)), "never a shortcut");

    printf("=== nothing ===\n");
    ok(!kbd_layout_from_descriptor(NULL, 0, &l), "no descriptor");

    printf("\n%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
