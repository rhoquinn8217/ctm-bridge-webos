/* Tests for the names bridge sessions go by on the listener.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). The names counted from 1 in
 * every run of the app, so a restart sent the listener its last run's names
 * again, and the listener handed the new pads to the old run's sessions. These
 * pin src/shared/session_name.inl: two runs never share a name, the kind stays
 * readable in it, and the longest name fits what the listener takes.
 *
 * ➡️ Build and run:  cc tests/test_session_name.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <string.h>

#include "../src/shared/session_name.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

int main(void)
{
    char tag[SESSION_RUN_TAG_LEN + 1], other[SESSION_RUN_TAG_LEN + 1];
    char name[32], name2[32];

    printf("\nthe run's tag\n");
    session_run_tag_from(0x00abcdefu, tag);
    ok(strcmp(tag, "abcdef") == 0, "six hex digits of the seed");
    session_run_tag_from(0x12345678u, tag);
    ok(strcmp(tag, "345678") == 0 && strlen(tag) == SESSION_RUN_TAG_LEN, "the low six, whatever the seed");

    printf("\nnames across two runs\n");
    session_run_tag_from(0x00c0ffeeu, tag);
    session_run_tag_from(0x00beef01u, other);
    session_name_make(name, sizeof(name), "ds5", 1, tag);
    session_name_make(name2, sizeof(name2), "ds5", 1, other);
    ok(strcmp(name, "ctm-ds5-1-c0ffee") == 0, "a pad's name carries the run's tag");
    ok(strcmp(name, name2) != 0, "the first DualSense of two runs has two names: the restart's pad gets a new session");
    session_name_make(name2, sizeof(name2), "mouse", 1, tag);
    ok(strcmp(name, name2) != 0, "the pointer and a pad never share one");

    printf("\nwhat the name must keep\n");
    session_name_make(name, sizeof(name), "ds4", 3, tag);
    ok(strstr(name, "-ds4-") != NULL, "\"-ds4-\" stays in a Bluetooth DS4's name, which picks its handback tone");
    session_name_make(name, sizeof(name), "ds4_usb", 3, tag);
    ok(strstr(name, "-ds4-") == NULL, "and not in a cabled one's, as before");
    session_name_make(name, sizeof(name), "ds5e_usb", 4294967295u, tag);
    ok(strlen(name) <= 31 && strcmp(name + strlen(name) - SESSION_RUN_TAG_LEN, tag) == 0,
       "the longest kind with the largest count fits the listener's 31, tag whole");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
