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

#include "../src/controllers/bt_sign.inl"
#include "../src/controllers/ds5_light.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
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

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
