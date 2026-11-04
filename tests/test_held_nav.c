/* Tests for reading a bridged pad's d-pad, Cross and Circle and its left stick
 * while the TV holds input (held_nav.inl), for a webOS pop-up.
 *
 * ⭐ WHY THIS EXISTS. A wrong offset fails quietly and in the worst way: the
 * pop-up's selection moves when nobody pressed anything, or the other way, or
 * OK comes from a button the user never meant as OK. Each pad type is checked
 * on the cable's report and Bluetooth's: at rest, every direction of its d-pad,
 * each of the two buttons alone, the face buttons that must NOT count, the
 * stick at both ends, and a report too short or with another id refused.
 *
 * ➡️ Build and run:  cc tests/test_held_nav.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

#include "../src/controllers/held_nav.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

typedef bool (*reader_t)(const uint8_t *, size_t, uint32_t *);

/* The directions in hat order, from up, clockwise. */
static const uint32_t k_dirs[8] = {
    HELD_NAV_UP, HELD_NAV_UP | HELD_NAV_RIGHT, HELD_NAV_RIGHT, HELD_NAV_DOWN | HELD_NAV_RIGHT,
    HELD_NAV_DOWN, HELD_NAV_DOWN | HELD_NAV_LEFT, HELD_NAV_LEFT, HELD_NAV_UP | HELD_NAV_LEFT,
};

/* A PlayStation pad on one report: the sticks at `lx` and `lx + 1`, the hat and
 * the face buttons at `bb`. */
static void check_ps(const char *name, reader_t read, uint8_t id, size_t len, size_t lx, size_t bb)
{
    uint8_t r[96];
    uint32_t w = 0xffffffffu;
    char what[160];

    memset(r, 0, sizeof(r));
    r[0] = id;
    r[lx] = 0x80;
    r[lx + 1] = 0x80;
    r[bb] = 0x08;   /* the hat: none */
    snprintf(what, sizeof(what), "%s: at rest, nothing pressed and the stick at 128,128", name);
    ok(read(r, len, &w) && HELD_NAV_KEYS(w) == 0 && HELD_NAV_LX(w) == 128 && HELD_NAV_LY(w) == 128, what);

    for (unsigned hat = 0; hat < 8; ++hat) {
        r[bb] = (uint8_t)hat;
        snprintf(what, sizeof(what), "%s: hat %u reads as its direction", name, hat);
        ok(read(r, len, &w) && HELD_NAV_KEYS(w) == k_dirs[hat], what);
    }

    r[bb] = 0x08 | 0x20;
    snprintf(what, sizeof(what), "%s: Cross alone", name);
    ok(read(r, len, &w) && HELD_NAV_KEYS(w) == HELD_NAV_SOUTH, what);

    r[bb] = 0x08 | 0x40;
    snprintf(what, sizeof(what), "%s: Circle alone", name);
    ok(read(r, len, &w) && HELD_NAV_KEYS(w) == HELD_NAV_EAST, what);

    r[bb] = 0x08 | 0x10 | 0x80;
    snprintf(what, sizeof(what), "%s: Square and Triangle count for nothing", name);
    ok(read(r, len, &w) && HELD_NAV_KEYS(w) == 0, what);

    r[bb] = 0x02 | 0x20 | 0x40;
    snprintf(what, sizeof(what), "%s: right with Cross and Circle together", name);
    ok(read(r, len, &w) && HELD_NAV_KEYS(w) == (HELD_NAV_RIGHT | HELD_NAV_SOUTH | HELD_NAV_EAST), what);

    r[bb] = 0x08;
    r[lx] = 0x00;
    r[lx + 1] = 0xff;
    snprintf(what, sizeof(what), "%s: the stick all the way left and down", name);
    ok(read(r, len, &w) && HELD_NAV_LX(w) == 0 && HELD_NAV_LY(w) == 255, what);

    r[lx] = 0xff;
    r[lx + 1] = 0x00;
    snprintf(what, sizeof(what), "%s: the stick all the way right and up", name);
    ok(read(r, len, &w) && HELD_NAV_LX(w) == 255 && HELD_NAV_LY(w) == 0, what);

    snprintf(what, sizeof(what), "%s: a report one byte short is refused", name);
    ok(!read(r, len - 1, &w), what);

    r[0] = (uint8_t)(id + 1);
    snprintf(what, sizeof(what), "%s: another report id is refused", name);
    ok(!read(r, len, &w), what);
}

static void check_xbox(void)
{
    uint8_t r[17];
    uint32_t w = 0xffffffffu;
    char what[160];

    memset(r, 0, sizeof(r));
    r[0] = 0x01;
    r[1] = 0x00; r[2] = 0x80;   /* LX, centre 0x8000 */
    r[3] = 0x00; r[4] = 0x80;   /* LY */
    ok(held_nav_xbox(r, sizeof(r), &w) && HELD_NAV_KEYS(w) == 0 && HELD_NAV_LX(w) == 128 &&
       HELD_NAV_LY(w) == 128, "xbox: at rest, nothing pressed and the stick at 128,128");

    for (unsigned hat = 1; hat <= 8; ++hat) {
        r[13] = (uint8_t)hat;
        snprintf(what, sizeof(what), "xbox: hat ordinal %u reads as its direction", hat);
        ok(held_nav_xbox(r, sizeof(r), &w) && HELD_NAV_KEYS(w) == k_dirs[hat - 1], what);
    }

    r[13] = 0;
    r[14] = 0x01;
    ok(held_nav_xbox(r, sizeof(r), &w) && HELD_NAV_KEYS(w) == HELD_NAV_SOUTH, "xbox: A alone");
    r[14] = 0x02;
    ok(held_nav_xbox(r, sizeof(r), &w) && HELD_NAV_KEYS(w) == HELD_NAV_EAST, "xbox: B alone");
    r[14] = 0x08 | 0x10 | 0x40 | 0x80;
    ok(held_nav_xbox(r, sizeof(r), &w) && HELD_NAV_KEYS(w) == 0, "xbox: X, Y, LB and RB count for nothing");
    r[14] = 0;
    r[15] = 0xff;
    ok(held_nav_xbox(r, sizeof(r), &w) && HELD_NAV_KEYS(w) == 0, "xbox: View, Menu and the clicks count for nothing");
    r[15] = 0;

    r[1] = 0xff; r[2] = 0xff;
    r[3] = 0x00; r[4] = 0x00;
    ok(held_nav_xbox(r, sizeof(r), &w) && HELD_NAV_LX(w) == 255 && HELD_NAV_LY(w) == 0,
       "xbox: the stick all the way right and up");

    ok(!held_nav_xbox(r, sizeof(r) - 1, &w), "xbox: a report one byte short is refused");
    r[0] = 0x04;
    ok(!held_nav_xbox(r, sizeof(r), &w), "xbox: the battery report (0x04) is refused");
}

int main(void)
{
    check_ps("DualSense on a cable", held_nav_ds5, 0x01, 64, 1, 8);
    check_ps("DualSense on Bluetooth", held_nav_ds5, 0x31, 78, 2, 9);
    check_ps("DS4 on a cable", held_nav_ds4, 0x01, 64, 1, 5);
    check_ps("DS4 on Bluetooth", held_nav_ds4, 0x11, 78, 3, 7);
    check_xbox();
    ok(held_nav_dirs(8) == 0 && held_nav_dirs(15) == 0, "a hat of 8 or more is no direction");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
