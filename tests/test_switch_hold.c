/* Tests for what a bridge keeps released: src/controllers/switch_hold.inl.
 *
 * ⭐ WHY THIS EXISTS (rhoquinn8217, 2026-10-09). "The correct behavior is if
 * you bridge while hold anything down should automatically cancel every input.
 * We don't want any lingering inputs being held down." Anything held as a
 * bridge starts reaches the PC released, and stays released until it is let
 * go; then it works as normal. These pin the rule and each type's layout, and
 * read controller_common.c for where the pump uses it.
 *
 * ➡️ Build and run:  cc tests/test_switch_hold.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <stdlib.h>

#include "../src/controllers/switch_hold.inl"

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
    const char *b = strstr(a, end);
    if (!b) return NULL;
    char *out = malloc((size_t)(b - a) + 1);
    if (!out) return NULL;
    memcpy(out, a, (size_t)(b - a));
    out[b - a] = '\0';
    return out;
}

/* A DualSense report at rest: sticks centred, the d-pad at 8, no fingers. */
static void ds5_rest(uint8_t *r, size_t len, uint8_t id, size_t off)
{
    memset(r, 0, len);
    r[0] = id;
    for (int s = 0; s < 4; ++s) r[1 + s + off] = 0x80;
    r[8 + off] = 0x08;
    r[33 + off] = 0x80;
    r[37 + off] = 0x80;
}

/* Note a report as the pad sent it, then return the copy the PC is sent. */
static void pass(const swh_layout_t *l, swh_state_t *s, const uint8_t *real, uint8_t *sent, size_t len)
{
    swh_note(l, s, real, len);
    memcpy(sent, real, len);
    swh_apply(l, s, sent, len);
}

int main(void)
{
    static swh_layout_t l;
    swh_state_t s;
    uint8_t r[128], out[128];

    printf("\nnothing happens without a switch\n");
    swh_layout_ds5(&l);
    memset(&s, 0, sizeof(s));
    ds5_rest(r, 64, 0x01, 0);
    r[8] |= 0x20;                                       /* Cross held */
    pass(&l, &s, r, out, 64);
    ok(out[8] == 0x28 && !swh_active(&s), "a report goes as it came when no bridge has started");

    printf("\na DualSense on a cable: what is held at the switch goes as released\n");
    swh_arm(&s);
    ds5_rest(r, 64, 0x01, 0);
    r[8] |= 0x20;                                       /* Cross */
    r[5] = 200;                                         /* L2 squeezed */
    r[33] = 0x05;                                       /* a finger on the pad (the bridge gesture's) */
    r[1] = 0x90;                                        /* the left stick inside its dead zone */
    const int found = swh_note(&l, &s, r, 64);
    ok(found == 3, "three things held: Cross, L2 and the finger (a stick in its dead zone is at rest)");
    memcpy(out, r, 64);
    swh_apply(&l, &s, out, 64);
    ok(out[8] == 0x08 && out[5] == 0 && (out[33] & 0x80) != 0, "the PC sees Cross, L2 and the finger released");
    ok(out[1] == 0x90, "and the stick at rest as it is");
    r[8] = 0x08 | 0x20 | 0x10;                          /* Square pressed now, Cross still held */
    pass(&l, &s, r, out, 64);
    ok((out[8] & 0x10) != 0 && (out[8] & 0x20) == 0, "a button pressed after the switch works; the held one stays released");
    r[8] = 0x08 | 0x10;                                 /* Cross let go */
    pass(&l, &s, r, out, 64);
    r[8] = 0x08 | 0x20;                                 /* and pressed again */
    pass(&l, &s, r, out, 64);
    ok((out[8] & 0x20) != 0, "once let go, it works as normal");
    r[5] = 0; r[33] = 0x85;                             /* L2 and the finger let go */
    pass(&l, &s, r, out, 64);
    ok(!swh_active(&s), "everything let go: nothing is kept any more");

    printf("\nover Bluetooth: the same, one byte along\n");
    swh_arm(&s);
    ds5_rest(r, 78, 0x31, 1);
    r[9] |= 0x80;                                       /* Triangle, at 8 + 1 */
    r[11] = 0x02;                                       /* the touchpad's click, at 10 + 1 */
    pass(&l, &s, r, out, 78);
    ok((out[9] & 0x80) == 0 && out[11] == 0, "Triangle and the pad's click go as released");
    ok(out[2] == 0x80 && out[9] == 0x08, "the sticks and the d-pad as they were");
    ds5_rest(r, 10, 0x01, 0);
    r[5] = 0xff;
    memcpy(out, r, 10);
    swh_apply(&l, &s, out, 10);
    ok(out[5] == 0xff, "Bluetooth's short report is not a cable's report, and is left alone");

    printf("\na DS4: the d-pad is a direction, 8 at rest; the counter is not touched\n");
    swh_layout_ds4(&l);
    swh_arm(&s);
    memset(r, 0, 64);
    r[0] = 0x01;
    for (int i = 1; i <= 4; ++i) r[i] = 0x80;
    r[5] = 0x00;                                        /* d-pad up */
    r[7] = 0xa4;                                        /* the counter, and nothing pressed */
    r[35] = 0x80; r[39] = 0x80; r[44] = 0x80; r[48] = 0x80; r[53] = 0x80; r[57] = 0x80;
    pass(&l, &s, r, out, 64);
    ok((out[5] & 0x0f) == 0x08, "up held at the switch goes as the d-pad at rest");
    ok(out[7] == 0xa4, "and the counter keeps counting");

    printf("\nan Xbox pad: 16-bit sticks with a dead zone, triggers, the d-pad, buttons\n");
    swh_layout_xbox(&l);
    swh_arm(&s);
    memset(r, 0, 17);
    r[0] = 0x01;
    for (int i = 0; i < 4; ++i) { r[1 + 2 * i] = 0x00; r[2 + 2 * i] = 0x80; }
    r[2] = 0x85;                                        /* left X 0x8500: inside the dead zone */
    r[4] = 0xf0;                                        /* left Y 0xf000: pushed */
    r[9] = 0xf4; r[10] = 0x01;                          /* LT 500 */
    r[13] = 1;                                          /* d-pad up */
    r[14] = 0x01;                                       /* A */
    pass(&l, &s, r, out, 17);
    ok(out[2] == 0x85 && out[3] == 0x00 && out[4] == 0x80, "the pushed stick goes centred, the resting one as it is");
    ok(out[9] == 0 && out[10] == 0 && out[13] == 0 && out[14] == 0, "LT, the d-pad and A go as released");

    printf("\na keyboard: modifiers by bit, keys by code\n");
    swh_layout_keyboard(&l, 0, 0, 2, 6);
    swh_arm(&s);
    uint8_t k[8] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };    /* Shift and "a" held */
    pass(&l, &s, k, out, 8);
    ok(out[0] == 0 && out[2] == 0, "Shift and a go as released");
    uint8_t k2[8] = { 0x02, 0, 0x04, 0x05, 0, 0, 0, 0 }; /* "b" pressed now */
    pass(&l, &s, k2, out, 8);
    ok(out[0] == 0 && out[2] == 0x05 && out[3] == 0, "b, pressed after the switch, goes through, first in the array");
    uint8_t k3[8] = { 0x02, 0, 0x05, 0, 0, 0, 0, 0 };   /* "a" let go */
    pass(&l, &s, k3, out, 8);
    uint8_t k4[8] = { 0x02, 0, 0x05, 0x04, 0, 0, 0, 0 }; /* and pressed again */
    pass(&l, &s, k4, out, 8);
    ok(out[2] == 0x05 && out[3] == 0x04 && out[0] == 0, "a, let go and pressed again, works; Shift is still held over");

    printf("\na mouse: its buttons, never its movement\n");
    static const uint8_t mouse[] = {
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
        0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
        0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
        0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0,
    };
    swh_layout_buttons_from_descriptor(&l, mouse, sizeof(mouse));
    ok(l.count == 3 && !l.ids_used && l.field[0].bit_offset == 0 && l.field[2].bit_offset == 2,
       "three buttons, at bits 0 to 2 of a report with no id");
    swh_arm(&s);
    uint8_t m[3] = { 0x01, 5, 0xfb };                   /* left held, moving */
    pass(&l, &s, m, out, 3);
    ok(out[0] == 0 && out[1] == 5 && out[2] == 0xfb, "the left button goes as released, the movement as it came");

    printf("\nany other gamepad, by its descriptor\n");
    static const uint8_t pad[] = {
        0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
        0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
        0xc0,
    };
    static pad_blank_plan_t plan;
    pad_blank_from_descriptor(pad, sizeof(pad), 0x1234, 0x5678, &plan);
    swh_layout_from_pad_blank(&l, &plan);
    ok(l.count == 10 && l.field[0].rest == 128 && l.field[0].slack == 32 && l.field[2].slack == 0,
       "two sticks centred with an eighth of dead zone, eight buttons with none");
    swh_arm(&s);
    uint8_t g[3] = { 0x20, 0x80, 0x01 };                /* X pushed, button 1 held */
    pass(&l, &s, g, out, 3);
    ok(out[0] == 128 && out[1] == 0x80 && out[2] == 0, "the pushed stick goes centred and the button released");

    printf("\nwhere the pump uses it\n");
    char *common = load("src/controllers/controller_common.c");
    char *ds5 = load("src/controllers/controller_ds5.c");
    char *ds4 = load("src/controllers/controller_ds4.c");
    char *xpad = load("src/controllers/controller_xpad.c");
    char *xbox = load("src/controllers/controller_xbox.c");
    ok(common && ds5 && ds4 && xpad && xbox, "the pump's and the types' code can be read");
    if (!common || !ds5 || !ds4 || !xpad || !xbox) return 1;
    char *session = between(common, "static void *session_main(void *arg)", "\n}");
    const char *opened = session ? strstr(session, "if (c->hid_fd < 0) {") : NULL;
    const char *begin = session ? strstr(session, "switch_hold_begin(c, report_desc, report_desc_len);") : NULL;
    const char *loop = session ? strstr(session, "while (!c->stop)") : NULL;
    ok(opened && begin && loop && opened < begin && begin < loop,
       "a bridge is a switch once, after the device is open and before its first connection");
    char *thread = between(common, "static void *input_thread_main(void *arg)", "\nstatic ");
    const char *note = thread ? strstr(thread, "swh_note(&c->swh_layout, &c->swh, buf, (size_t)n)") : NULL;
    const char *send = thread ? strstr(thread, "c->primary_in_ep, to_host, (size_t)n)") : NULL;
    const char *type = send ? strstr(send, "c->ops->on_input_report(c, buf, (size_t)n)") : NULL;
    ok(note && send && note < send, "every report is read for what is let go, and the PC gets the copy");
    ok(type != NULL, "the type still reads the real report after it is sent: the unbridge chord lives there");
    char *keep = between(common, "static int send_keepalive(ctm_controller_t *c)", "\n}");
    ok(keep && strstr(keep, "swh_apply(&c->swh_layout, &c->swh, buf, (size_t)n)") != NULL,
       "a still pad's report sent again is kept at rest too");
    char *begin_fn = between(common, "static void switch_hold_begin(", "\n}");
    ok(begin_fn && strstr(begin_fn, "if (c->ops->reports_steadily) {") != NULL &&
       strstr(begin_fn, "switch_held_now(c, what, sizeof(what))") != NULL,
       "a pad that reports only on a change is asked what is held first");
    char *ds5_ops = between(ds5, "const ctm_controller_ops_t ctm_controller_ds5_ops = {", "\n};");
    char *ds5e_ops = between(ds5, "const ctm_controller_ops_t ctm_controller_ds5e_ops = {", "\n};");
    char *ds4_ops = between(ds4, "const ctm_controller_ops_t ctm_controller_ds4_ops = {", "\n};");
    char *ds4u_ops = between(ds4, "const ctm_controller_ops_t controller_ds4_usb_ops = {", "\n};");
    char *xpad_ops = between(xpad, "const ctm_controller_ops_t controller_xpad_ops = {", "\n};");
    char *xbox_ops = between(xbox, "const ctm_controller_ops_t ctm_controller_xbox_ops = {", "\n};");
    ok(ds5_ops && strstr(ds5_ops, ".switch_layout = swh_layout_ds5,") && strstr(ds5_ops, ".reports_steadily = true,") &&
       ds5e_ops && strstr(ds5e_ops, ".switch_layout = swh_layout_ds5,") && strstr(ds5e_ops, ".reports_steadily = true,"),
       "a DualSense and an Edge by their layout, steadily");
    ok(ds4_ops && strstr(ds4_ops, ".switch_layout = swh_layout_ds4,") && strstr(ds4_ops, ".reports_steadily = true,") &&
       ds4u_ops && strstr(ds4u_ops, ".switch_layout = swh_layout_ds4,") && strstr(ds4u_ops, ".reports_steadily = true,"),
       "a DS4 on either bus");
    ok(xpad_ops && strstr(xpad_ops, ".switch_layout = swh_layout_xbox,") && strstr(xpad_ops, ".reports_steadily = true,") &&
       xbox_ops && strstr(xbox_ops, ".switch_layout = swh_layout_xbox,") && strstr(xbox_ops, ".reports_steadily = false,"),
       "an Xbox pad: cabled steadily, over Bluetooth only on a change");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
