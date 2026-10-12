/* Tests for a bridged mouse going to the TV while its overlay is open.
 *
 * ⭐ WHY THIS EXISTS. A mouse that is not recognised fails quietly: it stays
 * grabbed while the overlay is open, the TV's cursor never moves with it, and
 * its movement goes on to the PC behind the overlay. A wrong report length
 * sends the PC a short or long "nothing pressed" report, and an absolute
 * pointer sent zeros jumps to the corner. The descriptors below are the HID
 * specification's boot mouse (Appendix B.2), a wheel mouse behind a report id,
 * an absolute pointer, a keyboard and a mouse on one interface, and a gamepad.
 *
 * ➡️ Build and run:  cc tests/test_pointer_layout.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/controllers/pointer_layout.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static char *load(const char *rel)
{
    const char *prefixes[] = { "", "../" };
    for (unsigned i = 0; i < 2; ++i) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s", prefixes[i], rel);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        const long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *s = malloc((size_t)len + 1);
        if (!s) { fclose(f); return NULL; }
        s[fread(s, 1, (size_t)len, f)] = '\0';
        fclose(f);
        return s;
    }
    return NULL;
}

/* From `head` to the first `end` after it. NULL if not found. */
static char *between(const char *src, const char *head, const char *end)
{
    const char *a = src ? strstr(src, head) : NULL;
    if (!a) return NULL;
    const char *b = strstr(a + strlen(head), end);
    if (!b) return NULL;
    const size_t len = (size_t)(b - a);
    char *s = malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, a, len);
    s[len] = '\0';
    return s;
}

/* HID 1.11 Appendix B.2, the boot mouse: three buttons, five bits of padding,
 * X and Y relative, one byte each. */
static const uint8_t k_boot_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,             /* three buttons */
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,             /* padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06,             /* X, Y relative */
    0xc0, 0xc0,
};

/* A wheel mouse as report 2, after a consumer-control report 1 that must not
 * change its length: five buttons, 16-bit X and Y, a wheel and a horizontal
 * wheel (AC Pan, on the consumer page), all relative. 7 bytes and the id. */
static const uint8_t k_wheel_mouse[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x01,
    0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xc0,
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x81, 0x02,             /* five buttons */
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,             /* padding */
    0x05, 0x01, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02,
    0x09, 0x30, 0x09, 0x31, 0x81, 0x06,             /* X, Y relative, 16 bits */
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01,
    0x09, 0x38, 0x81, 0x06,                         /* wheel */
    0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,   /* AC Pan */
    0xc0, 0xc0,
};

/* An absolute pointer, as a KVM or a tablet in mouse mode reports: three
 * buttons, and X and Y as absolute 16-bit positions. */
static const uint8_t k_absolute[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,             /* X, Y absolute */
    0xc0, 0xc0,
};

/* A keyboard (report 1) and a mouse (report 2) on one interface, as many
 * wireless sets send them; the mouse's X and Y are given as a usage range. */
static const uint8_t k_combo[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x19, 0x30, 0x29, 0x31, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06,             /* X..Y relative, as a range */
    0xc0, 0xc0,
};

/* HID 1.11 Appendix B.1, the boot keyboard: no mouse at all. */
static const uint8_t k_keyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
};

/* A gamepad: eight buttons and two axes, which are not a mouse's. */
static const uint8_t k_gamepad[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xc0,
};

/* A mouse whose collection's usage carries its page in its top half
 * (Usage 0x00010002), and no Usage Page before it. */
static const uint8_t k_long_usage[] = {
    0x0b, 0x02, 0x00, 0x01, 0x00, 0xa1, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x02, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x06, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0,
};

int main(void)
{
    ptr_layout_t l;
    uint8_t r[64];

    printf("which devices are mice\n");
    ok(ptr_layout_from_descriptor(k_boot_mouse, sizeof(k_boot_mouse), &l) && l.present,
       "the boot mouse is a mouse");
    ok(l.report_id == 0 && l.report_len == 3 && l.relative,
       "  with no report id, three bytes, relative");
    ok(ptr_layout_from_descriptor(k_wheel_mouse, sizeof(k_wheel_mouse), &l) && l.present,
       "a wheel mouse behind a consumer report is a mouse");
    ok(l.report_id == 2 && l.report_len == 8 && l.relative,
       "  report 2, seven bytes and the id, relative: the consumer report does not count");
    ok(ptr_layout_from_descriptor(k_absolute, sizeof(k_absolute), &l) && l.present,
       "an absolute pointer is a mouse");
    ok(l.report_len == 5 && !l.relative, "  five bytes, and absolute");
    ok(ptr_layout_from_descriptor(k_combo, sizeof(k_combo), &l) && l.present,
       "a keyboard and a mouse on one interface has a mouse");
    ok(l.report_id == 2 && l.report_len == 4 && l.relative,
       "  the mouse's report 2, three bytes and the id, X and Y relative as a range");
    ok(ptr_layout_from_descriptor(k_long_usage, sizeof(k_long_usage), &l) && l.present &&
       l.report_len == 3 && l.relative,
       "a collection usage with its page in its top half is a mouse");
    ok(!ptr_layout_from_descriptor(k_keyboard, sizeof(k_keyboard), &l) && !l.present,
       "a keyboard is not a mouse");
    ok(!ptr_layout_from_descriptor(k_gamepad, sizeof(k_gamepad), &l) && !l.present,
       "a gamepad is not a mouse");
    ok(!ptr_layout_from_descriptor(k_boot_mouse, 12, &l) && !l.present,
       "a descriptor cut off before any input is not a mouse");
    ok(!ptr_layout_from_descriptor(NULL, 0, &l) && !l.present, "nothing is not a mouse");

    printf("\nwhich absolute pointers a range makes\n");
    uint8_t combo_abs[sizeof(k_combo)];
    memcpy(combo_abs, k_combo, sizeof(k_combo));
    /* The combo's last input, X..Y: relative (0x06) becomes absolute (0x02). */
    ok(combo_abs[sizeof(combo_abs) - 3] == 0x06, "the range's input item is where the test expects");
    combo_abs[sizeof(combo_abs) - 3] = 0x02;
    ok(ptr_layout_from_descriptor(combo_abs, sizeof(combo_abs), &l) && l.present && !l.relative,
       "X and Y given as an absolute range make an absolute pointer");

    printf("\nthe report the PC is sent as the mouse goes to the TV\n");
    ptr_layout_from_descriptor(k_wheel_mouse, sizeof(k_wheel_mouse), &l);
    memset(r, 0xaa, sizeof(r));
    const size_t n = ptr_released_report(&l, r, sizeof(r));
    int zeros = 1;
    for (size_t i = 1; i < n; ++i) if (r[i] != 0) zeros = 0;
    ok(n == 8 && r[0] == 2 && zeros, "its id, then nothing pressed and no movement");
    ok(r[8] == 0xaa, "  and nothing written past it");
    ptr_layout_from_descriptor(k_boot_mouse, sizeof(k_boot_mouse), &l);
    memset(r, 0xaa, sizeof(r));
    ok(ptr_released_report(&l, r, sizeof(r)) == 3 && r[0] == 0 && r[1] == 0 && r[2] == 0,
       "a mouse with no id: three zeros");
    ok(ptr_released_report(&l, r, 2) == 0, "none when it does not fit");
    ptr_layout_from_descriptor(k_absolute, sizeof(k_absolute), &l);
    ok(ptr_released_report(&l, r, sizeof(r)) == 0,
       "none for an absolute pointer: zeros would put it in the corner");
    ptr_layout_from_descriptor(k_keyboard, sizeof(k_keyboard), &l);
    ok(ptr_released_report(&l, r, sizeof(r)) == 0, "none for a device that is not a mouse");

    printf("\nwhich reports say the mouse is being used (2026-10-11)\n");
    ptr_layout_from_descriptor(k_boot_mouse, sizeof(k_boot_mouse), &l);
    const uint8_t rest[3] = {0, 0, 0}, moved[3] = {0, 1, 0}, pressed[3] = {1, 0, 0};
    ok(!ptr_report_active(&l, rest, sizeof(rest)), "a mouse at rest: no");
    ok(ptr_report_active(&l, moved, sizeof(moved)), "a mouse that moved: yes");
    ok(ptr_report_active(&l, pressed, sizeof(pressed)), "a mouse with a button down: yes");
    ptr_layout_from_descriptor(k_wheel_mouse, sizeof(k_wheel_mouse), &l);
    const uint8_t idle2[8] = {2, 0, 0, 0, 0, 0, 0, 0}, wheel2[8] = {2, 0, 0, 0, 0, 0, 1, 0};
    const uint8_t media1[3] = {1, 0xe9, 0};
    ok(!ptr_report_active(&l, idle2, sizeof(idle2)), "report 2 with nothing in it: no");
    ok(ptr_report_active(&l, wheel2, sizeof(wheel2)), "report 2 with the wheel turned: yes");
    ok(!ptr_report_active(&l, media1, sizeof(media1)), "a media key in report 1 is not the mouse's");
    ptr_layout_from_descriptor(k_combo, sizeof(k_combo), &l);
    const uint8_t typed[9] = {1, 0x07, 0, 0x16, 0, 0, 0, 0, 0}, nudged[4] = {2, 0, 0xff, 0};
    ok(!ptr_report_active(&l, typed, sizeof(typed)), "a keyboard and a mouse on one interface: a key is not the mouse");
    ok(ptr_report_active(&l, nudged, sizeof(nudged)), "  and the mouse's report 2, moved, is");
    ptr_layout_from_descriptor(k_keyboard, sizeof(k_keyboard), &l);
    ok(!ptr_report_active(&l, moved, sizeof(moved)), "never for a device that is not a mouse");

    printf("\nwhere the pump uses it\n");
    char *common = load("src/controllers/controller_common.c");
    ok(common != NULL, "the pump's code can be read");
    if (!common) return 1;
    ok(strstr(common, "#include \"pointer_layout.inl\"") != NULL, "the pump includes it");
    char *opened = between(common, "static int open_hid(ctm_controller_t *c, ctmb_device_caps_t *caps,", "\n}");
    const char *cleared = opened ? strstr(opened, "memset(&c->ptr, 0, sizeof(c->ptr));") : NULL;
    const char *reads = opened ? strstr(opened, "ptr_layout_from_descriptor(report_desc, *report_desc_len, &c->ptr)") : NULL;
    ok(cleared && reads && cleared < reads,
       "each open starts with no mouse and reads its descriptor for one");
    const char *not_pad = reads ? strstr(opened, "if (!c->is_gamepad &&") : NULL;
    const char *not_pad2 = not_pad ? strstr(not_pad + 1, "if (!c->is_gamepad &&") : NULL;
    ok(not_pad2 && not_pad2 < reads, "  and never for a gamepad");
    char *sync = between(common, "static void handback_sync_hold(ctm_controller_t *c)", "\n}");
    ok(sync && strstr(sync, "if (!c->kbd.present && !c->ptr.present) return;") != NULL,
       "the hand-back covers a keyboard and a mouse");
    ok(sync && strstr(sync, "ptr_released_report(&c->ptr, released, sizeof(released))") != NULL,
       "  and sends the PC the mouse with nothing pressed as it goes");
    ok(sync && strstr(sync, "EVIOCGRAB, held ? 0 : 1") != NULL,
       "  and lets go of the grabs while the overlay is open, taking them again after");
    char *thread = between(common, "static void *input_thread_main(void *arg)", "\nstatic ");
    const char *idle = thread ? strstr(thread, "handback_sync_hold(c);") : NULL;
    const char *keep = thread ? strstr(thread, "if (send_keepalive(c) != 0)") : NULL;
    ok(idle && keep && idle < keep, "a mouse at rest follows the overlay too");
    const char *gate = thread ? strstr(thread, "if (c->kbd.present || c->ptr.present) {") : NULL;
    const char *drop = gate ? strstr(gate, "if (c->handed_back) {") : NULL;
    const char *note = thread ? strstr(thread, "swh_note(&c->swh_layout, &c->swh, buf, (size_t)n)") : NULL;
    const char *send = thread ? strstr(thread, "c->primary_in_ep, to_host, (size_t)n)") : NULL;
    ok(gate && drop && note && send && drop < note && drop < send,
       "while the TV has it, a mouse's reports are dropped before anything else reads them");
    const char *used = thread ? strstr(thread, "ptr_report_active(&c->ptr, buf, (size_t)n)") : NULL;
    ok(drop && used && send && drop < used && used < send,
       "a mouse's first use is told from what goes to the host, not what the TV had");
    char *session = between(common, "static void *session_main(void *arg)", "\n}");
    const char *grab = session ? strstr(session, "grab_matching_evdev(c);") : NULL;
    const char *reset = grab ? strstr(grab, "c->handed_back = 0;") : NULL;
    ok(grab && reset, "a fresh grab gives the PC the keyboard or mouse again");
    const char *fresh = grab ? strstr(grab, "c->ptr_used = 0;") : NULL;
    ok(fresh != NULL, "  and each bridge tells a mouse's first use again");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
