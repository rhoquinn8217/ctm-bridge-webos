/* Tests for a DualSense's lightbar reports: the ones the TV writes for the
 * bridged-and-waiting breath, and the test of whether the PC has set it.
 *
 * ⭐ WHY THIS EXISTS (rhoquinn8217, 2026-10-08). A bridged pad breathes green
 * until the PC takes its light. If the TV's report put the colour or its claim
 * a byte out, the pad would ignore it and never breathe; if the test of the
 * PC's reports missed the claim, the pad would breathe over Steam's colour
 * forever. These pin src/controllers/ds5_light.inl.
 *
 * ➡️ Build and run:  cc tests/test_ds5_light.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <stdlib.h>

#include "../src/controllers/bt_sign.inl"
#include "../src/controllers/ds5_light.inl"

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

/* Does the report end in the checksum the pad checks? */
static int signed_right(const uint8_t *r, size_t n)
{
    const uint8_t seed = 0xa2;
    uint32_t crc = bt_crc32_step(0xffffffffu, &seed, 1);
    crc = ~bt_crc32_step(crc, r, n - 4);
    return r[n - 4] == (uint8_t)crc && r[n - 3] == (uint8_t)(crc >> 8) &&
           r[n - 2] == (uint8_t)(crc >> 16) && r[n - 1] == (uint8_t)(crc >> 24);
}

int main(void)
{
    uint8_t r[400];

    printf("\non a cable\n");
    size_t n = ds5_light_usb_report(r, sizeof(r), 1, 2, 3);
    ok(n == 48 && r[0] == 0x02, "report 0x02, 48 bytes");
    ok(r[2] == 0x04 && r[1] == 0 && r[39] == 0, "claims the lightbar's colour and nothing else");
    ok(r[45] == 1 && r[46] == 2 && r[47] == 3, "the colour in bytes 45-47, where the pad reads it");
    ok(ds5_report_sets_light(r, n), "it counts as setting the light");
    r[2] = 0x01;   /* the mute light alone, as the listener's own settings report claims it */
    ok(!ds5_report_sets_light(r, n), "a report claiming only the mute light does not");
    ok(ds5_light_usb_report(r, 47, 0, 0, 0) == 0, "no report into too small a buffer");

    printf("\nover Bluetooth\n");
    n = ds5_light_bt_report(r, sizeof(r), 0x13, 9, 8, 7);
    ok(n == 78 && r[0] == 0x31, "report 0x31, 78 bytes");
    ok(r[1] == 0x30 && r[2] == 0x10, "its count in the high half of byte 1, wrapped at 16, and the tag 0x10");
    ok(r[4] == 0x04 && r[3] == 0, "claims the lightbar's colour only");
    ok(r[47] == 9 && r[48] == 8 && r[49] == 7, "the colour in bytes 47-49");
    bt_sign_output(r, n);
    ok(signed_right(r, n), "signed as the pad checks it, once the caller signs it");
    ok(ds5_report_sets_light(r, n), "it counts as setting the light");

    printf("\nthe PC's reports with audio (0x36, 0x32)\n");
    memset(r, 0, sizeof(r));
    r[0] = 0x36;
    r[2] = 0x90; r[3] = 63;           /* the output block first */
    r[5] = 0x04;                      /* its claim panel 2: the lightbar */
    ok(ds5_report_sets_light(r, 398), "a 0x90 block claiming the light sets it");
    r[5] = 0x01;
    ok(!ds5_report_sets_light(r, 398), "one claiming only the mute light does not");
    memset(r, 0, sizeof(r));
    r[0] = 0x32;
    r[2] = 0x91; r[3] = 7;            /* another block in front */
    r[11] = 0x90; r[12] = 63;
    r[14] = 0x04;
    ok(ds5_report_sets_light(r, 141), "found after another block, as the walk goes");
    r[3] = 250;                       /* a block that runs off the end */
    ok(!ds5_report_sets_light(r, 141), "a block running past the end is not believed");

    printf("\nnot a lightbar report\n");
    ok(!ds5_report_sets_light(NULL, 0) && !ds5_report_sets_light(r, 2), "nothing, or too short");
    r[0] = 0x05; r[1] = 0x02;
    ok(!ds5_report_sets_light(r, 32), "a DS4's report, whatever it claims");

    printf("\nthe light's takeover over Bluetooth (rhoquinn8217's C3, 2026-10-09)\n");
    n = ds5_light_bt_takeover_report(r, sizeof(r), 0x15);
    ok(n == 78 && r[0] == 0x31 && r[1] == 0x50 && r[2] == 0x10, "report 0x31, its count and tag as the colour's");
    ok(r[4] == 0x08 && r[3] == 0, "claims the reset of the light's state and nothing else");
    ok(r[47] == 0 && r[48] == 0 && r[49] == 0, "and carries no colour");
    ok(!ds5_report_sets_light(r, n), "so it does not count as setting the light");
    bt_sign_output(r, n);
    ok(signed_right(r, n), "signed as the pad checks it");
    ok(ds5_light_bt_takeover_report(r, 77, 0) == 0, "no report into too small a buffer");

    printf("\nthe pad's clock, which says when its connection animation is over\n");
    uint32_t clock = 0;
    memset(r, 0, sizeof(r));
    r[0] = 0x31;
    r[29] = 0x40; r[30] = 0x9f; r[31] = 0x9b; r[32] = 0x00;   /* 10198848: not yet */
    ok(ds5_input_clock(r, 78, &clock) && clock == 10198848u, "read from Bluetooth's report, one byte along");
    ok(clock < DS5L_ANIMATION_DONE, "and just short of the end of the animation");
    r[31] = 0x9c;                                               /* 10264384 */
    ok(ds5_input_clock(r, 78, &clock) && clock >= DS5L_ANIMATION_DONE, "a little later it is over");
    memset(r, 0, sizeof(r));
    r[0] = 0x01;
    r[28] = 1; r[29] = 2; r[30] = 3; r[31] = 4;
    ok(ds5_input_clock(r, 64, &clock) && clock == 0x04030201u, "a cable's report holds it a byte earlier");
    ok(!ds5_input_clock(r, 63, &clock), "not from a short cable report");
    r[0] = 0x31;
    ok(!ds5_input_clock(r, 77, &clock), "nor a short Bluetooth one");
    r[0] = 0x36;
    ok(!ds5_input_clock(r, 398, &clock) && !ds5_input_clock(NULL, 78, &clock), "nor any other report, or none");

    printf("\nwhere the takeover is sent\n");
    char *sig = load("src/controllers/ctm_bt_signal.inl");
    char *ds5 = load("src/controllers/controller_ds5.c");
    ok(sig && ds5, "the signal's and the DualSense's code can be read");
    if (!sig || !ds5) return 1;
    char *play = between(sig, "static int btsig_play_fd(", "\n}");
    const char *take = play ? strstr(play, "ds5_light_bt_takeover_report(take, sizeof take") : NULL;
    const char *prime = play ? strstr(play, "for (int i = 0; i < prime_frames; ++i) {") : NULL;
    ok(take && prime && take < prime, "every Bluetooth signal sends it before its first report");
    char *paint = between(sig, "int ctm_ds5_bt_paint(", "\n}");
    const char *paint_take = paint ? strstr(paint, "ds5_light_bt_takeover_report(") : NULL;
    const char *paint_colour = paint ? strstr(paint, "ds5_light_bt_report(") : NULL;
    ok(paint_take && paint_colour && paint_take < paint_colour, "and the app's own colours, before the colour");
    char *ds5_ops = between(ds5, "const ctm_controller_ops_t ctm_controller_ds5_ops = {", "\n};");
    char *ds5e_ops = between(ds5, "const ctm_controller_ops_t ctm_controller_ds5e_ops = {", "\n};");
    ok(ds5_ops && strstr(ds5_ops, ".on_input_report = ds5_input_hook") != NULL &&
       ds5e_ops && strstr(ds5e_ops, ".on_input_report = ds5_input_hook") != NULL,
       "a bridged DualSense and an Edge watch their clock for it");
    char *when = between(ds5, "static void ds5_take_light_when_ready(", "\n}");
    ok(when && strstr(when, "clock < DS5L_ANIMATION_DONE") != NULL &&
       strstr(when, "ctm_controller_set_type_state(c, DS5_SLOT_LIGHT, 1)") != NULL,
       "and send it once a bridge, when the animation is over");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
