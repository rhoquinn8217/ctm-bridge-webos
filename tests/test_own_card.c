/* Tests for finding a pad's own sound card by its USB id (own_card.inl).
 *
 * ⭐ WHY THIS EXISTS (2026-11-03). A cabled second-model DS4 is a USB audio
 * device, and its card is found by the id snd-usb-audio writes for it in
 * /proc/asound/cardN/usbid. A card that is not its own must never be taken:
 * the pad's speaker sound would play through another pad, or the TV's own
 * card.
 *
 * ⛔ Nothing here needs a television, a controller or a stream. The finder
 * reads one small file per card; these tests point it at a scratch directory
 * and write those files themselves.
 *
 * ➡️ Build and run:  cc tests/test_own_card.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sound/asound.h>

/* ⓘ The finder is included from the real file rather than copied, so a change
 * there is seen here. Only its two paths are pointed elsewhere. */
static char g_usbid_fmt[160];
static char g_control_fmt[160];
#define OWN_CARD_USBID_FMT   g_usbid_fmt     /* "<dir>/card%d/usbid" */
#define OWN_CARD_CONTROL_FMT g_control_fmt   /* "<dir>/controlC%d" */

#include "../src/controllers/own_card.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static char g_dir[128];

/* Write card `card`'s usbid as `text`, the way the kernel would. */
static void set_usbid(int card, const char *text)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/card%d", g_dir, card);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/card%d/usbid", g_dir, card);
    FILE *f = fopen(path, "w");
    if (!f) { printf("  (could not write %s: errno=%d)\n", path, errno); return; }
    fputs(text, f);
    fclose(f);
}

static void remove_card(int card)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/card%d/usbid", g_dir, card);
    unlink(path);
    snprintf(path, sizeof(path), "%s/card%d", g_dir, card);
    rmdir(path);
}

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/ctm-own-card-%d", (int)getpid());
    mkdir(g_dir, 0700);
    snprintf(g_usbid_fmt, sizeof(g_usbid_fmt), "%s/card%%d/usbid", g_dir);
    snprintf(g_control_fmt, sizeof(g_control_fmt), "%s/controlC%%d", g_dir);

    /* As the C1 had them on 2026-11-03: its own two cards (no usbid at all),
     * then a DS4Dongle's. Then a DualSense's, written in upper case, and one
     * that is not an id. */
    set_usbid(2, "054c:09cc\n");
    set_usbid(3, "054C:0CE6\n");
    set_usbid(4, "not an id\n");

    printf("\nfinding a card by the pad's USB id\n");
    ok(!own_card_usbid_is(0, 0x054c, 0x09cc), "the TV's own card (no usbid) is not a pad's");
    ok(own_card_usbid_is(2, 0x054c, 0x09cc), "a second-model DS4's card is found by its id");
    ok(!own_card_usbid_is(2, 0x054c, 0x05c4), "a first-model DS4's id does not match it");
    ok(!own_card_usbid_is(3, 0x054c, 0x09cc), "a DualSense's card is not the DS4's");
    ok(own_card_usbid_is(3, 0x054c, 0x0ce6), "and upper case reads the same as lower");
    ok(!own_card_usbid_is(4, 0x054c, 0x09cc), "a usbid that is not an id matches nothing");
    ok(!own_card_usbid_is(7, 0x054c, 0x09cc), "a card that is not there matches nothing");

    printf("\nthe volume, with no mixer to set it on\n");
    char said[200];
    own_card_volume_up(2, said, sizeof(said));
    ok(strstr(said, "mixer not opened") != NULL, "says the mixer did not open");
    ok(strstr(said, "volume left as it was") != NULL, "and that nothing was changed");

    remove_card(2);
    remove_card(3);
    remove_card(4);
    rmdir(g_dir);

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
