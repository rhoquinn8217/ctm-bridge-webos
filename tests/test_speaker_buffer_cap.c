/* A guard on the speaker's buffer cap: every playback open takes its
 * parameters from the one function that caps the buffer.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). The speaker's buffer is its
 * latency, and with no ceiling the kernel chose a second of it on both TVs
 * measured. The cap went into the ordinary opener. The opener that moves a
 * speaker to its pad's own card was a hand copy made before that, and kept
 * the second. Four copies of the same parameters were how one was missed, so
 * this reads the shipped source: every file that opens a playback node sets
 * its parameters through ds5_playback_hw_params, and that function holds the
 * cap.
 *
 * ➡️ Build and run:  cc tests/test_speaker_buffer_cap.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* Lines of a source file holding `needle` and not `but_not`, or -1 when the
 * file cannot be read. Run from the repo root or from tests/. */
static int count_lines(const char *rel, const char *needle, const char *but_not)
{
    const char *prefixes[] = { "", "../" };
    FILE *fp = NULL;
    char path[256];
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]) && !fp; ++i) {
        snprintf(path, sizeof(path), "%s%s", prefixes[i], rel);
        fp = fopen(path, "r");
    }
    if (!fp) return -1;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, needle) && !(but_not && strstr(line, but_not))) ++n;
    }
    fclose(fp);
    return n;
}

int main(void)
{
    const char *files[] = {
        "src/controllers/controller_common.c",
        "src/controllers/ctm_cardmatch.inl",
    };
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        const int opens = count_lines(files[i], "pcmC%dD0p", NULL);
        const int calls = count_lines(files[i], "ds5_playback_hw_params(",
                                      "static int ds5_playback_hw_params(");
        printf("\n%s: %d playback opens, %d through the capped parameters\n",
               files[i], opens, calls);
        ok(opens > 0, "the guard can see the file's playback open");
        ok(calls == opens, "every playback open sets its parameters through ds5_playback_hw_params");
    }

    printf("\nthe one function\n");
    ok(count_lines(files[0], "static int ds5_playback_hw_params(", NULL) == 1, "is defined once");
    ok(count_lines(files[0], "hw.intervals[9].max = 4800;", NULL) == 1, "and caps the buffer at 4800 frames");
    ok(count_lines(files[1], "intervals[9].max = ", NULL) == 1,
       "the cardmatch file sets a buffer size only for its capture open");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
