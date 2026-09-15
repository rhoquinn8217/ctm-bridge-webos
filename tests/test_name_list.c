/* Tests for the name lists a device keeps: "input3, input4".
 *
 * ⭐⭐ WHY THIS EXISTS. On 2026-09-14 the U5s restarted and numbered a GameSir's
 * pad input3 and an Xbox One S pad input30. The GameSir's pad vanished from the
 * device list, because "input3" was searched for with strstr and found inside
 * "input30". The names below are those two devices'.
 *
 * ➡️ Build and run:  cc tests/test_name_list.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

#include "../src/shared/name_list.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static void test_whole_names_only(void)
{
    printf("\na name is found only whole\n");
    ok(!name_list_has("input30", "input3"), "input3 is not in a list holding only input30");
    ok(name_list_has("input30, input3", "input3"), "input3 is found beside input30");
    ok(name_list_has("input3", "input3"), "a list of one name holds that name");
    ok(!name_list_has("js10, event31", "js1"), "js1 is not in js10");
    ok(name_list_has("js10, event31", "event31"), "the last name is found");
    ok(name_list_has(" input4 ,input5", "input5"), "spaces around a name do not hide it");
    ok(!name_list_has("", "input3"), "an empty list holds nothing");
    ok(!name_list_has("input3", ""), "an empty name is never held");
}

static void test_adding(void)
{
    printf("\nadding keeps one of each, in order\n");
    char inputs[64] = "input30";
    name_list_add(inputs, sizeof(inputs), "input3");
    ok(strcmp(inputs, "input30, input3") == 0, "input3 is added although input30 is there");
    name_list_add(inputs, sizeof(inputs), "input3");
    ok(strcmp(inputs, "input30, input3") == 0, "adding it again changes nothing");

    char events[64] = "js8, event30";
    name_list_add(events, sizeof(events), "js0, event3");
    ok(strcmp(events, "js8, event30, js0, event3") == 0, "a list of names adds each one, event3 included");

    char empty[64] = "";
    name_list_add(empty, sizeof(empty), "event3");
    ok(strcmp(empty, "event3") == 0, "the first name has no separator before it");

    char tiny[8] = "event30";
    name_list_add(tiny, sizeof(tiny), "event3");
    ok(strcmp(tiny, "event30") == 0, "a full list is left as it was");

    char nearly[16] = "event30";
    name_list_add(nearly, sizeof(nearly), "event31");
    ok(strcmp(nearly, "event30") == 0, "a name that would be cut off is not added at all");
}

int main(void)
{
    test_whole_names_only();
    test_adding();

    printf("\n%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
