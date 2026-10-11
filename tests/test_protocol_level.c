/* Tests for whether this TV app and DS5-USBIP on the PC can work together.
 *
 * ⭐ WHY THIS EXISTS (rhoquinn8217, 2026-10-08, "1a and 2a"). Each side has a
 * protocol level that goes up only when the way the two talk changes; when
 * they cannot work together the TV refuses to bridge and says which side to
 * update. A side that names no level is level 1, so every listener and TV
 * from before the levels must still come out as working together. These pin
 * src/shared/protocol_level.inl, and read the shipped source for where it is
 * wired.
 *
 * ➡️ Build and run:  cc tests/test_protocol_level.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/shared/protocol_level.inl"

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
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *s = malloc((size_t)n + 1);
        if (!s) { fclose(f); return NULL; }
        s[fread(s, 1, (size_t)n, f)] = '\0';
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

int main(void)
{
    printf("\nreading a level out of an answer\n");
    ok(protocol_word("OK CTM_AGENT_V1 level=3 min_tv=2\n", " level=", 1) == 3, "the listener's level");
    ok(protocol_word("OK CTM_AGENT_V1 level=3 min_tv=2\n", " min_tv=", 1) == 2, "the oldest TV it works with");
    ok(protocol_word("OK CTM_AGENT_V1\n", " level=", 1) == 1, "none named: the default");
    ok(protocol_word("OK CTM_AGENT_V1 level=\n", " level=", 1) == 1, "no number after it: the default");
    ok(protocol_word("OK CTM_AGENT_V1 xlevel=7\n", " level=", 1) == 1, "only the whole word");
    ok(protocol_word(NULL, " level=", 1) == 1, "nothing from nothing");

    printf("\nwhat a STATUS answer says, for this TV (level %d, needs a listener of %d)\n",
           CTM_PROTOCOL_LEVEL, CTM_PROTOCOL_MIN_LISTENER);
    ok(protocol_compat("OK CTM_AGENT_V1\n") == PROTOCOL_OK,
       "a listener from before the levels works with it");
    ok(protocol_compat("OK CTM_AGENT_V1 level=1 min_tv=1\n") == PROTOCOL_OK, "this listener does");
    ok(protocol_compat("ERR something\n") == PROTOCOL_UNKNOWN && protocol_compat("") == PROTOCOL_UNKNOWN &&
       protocol_compat(NULL) == PROTOCOL_UNKNOWN, "no OK says nothing either way");
    char older_listener[96], newer_listener[96];
    snprintf(older_listener, sizeof(older_listener), "OK CTM_AGENT_V1 level=%d min_tv=1\n",
             CTM_PROTOCOL_MIN_LISTENER - 1);
    snprintf(newer_listener, sizeof(newer_listener), "OK CTM_AGENT_V1 level=%d min_tv=%d\n",
             CTM_PROTOCOL_LEVEL + 1, CTM_PROTOCOL_LEVEL + 1);
    ok(protocol_compat(older_listener) == PROTOCOL_UPDATE_PC, "a listener below what this TV needs: update the PC");
    ok(protocol_compat(newer_listener) == PROTOCOL_UPDATE_TV, "a listener that needs a newer TV: update the TV app");

    printf("\nboth sides' levels\n");
    ok(protocol_compat_for(2, 1, 3, 2) == PROTOCOL_OK, "each at or above what the other needs");
    ok(protocol_compat_for(1, 1, 3, 2) == PROTOCOL_UPDATE_PC, "the listener too old");
    ok(protocol_compat_for(4, 4, 3, 2) == PROTOCOL_UPDATE_TV, "the TV too old");

    printf("\nwhat the panel says\n");
    ok(strstr(protocol_advice(PROTOCOL_UPDATE_PC), "Update DS5-USBIP on the PC") == protocol_advice(PROTOCOL_UPDATE_PC),
       "update the PC, by its name");
    ok(strstr(protocol_advice(PROTOCOL_UPDATE_TV), "Update the TV app") == protocol_advice(PROTOCOL_UPDATE_TV),
       "update the TV app");
    ok(protocol_advice(PROTOCOL_OK)[0] == '\0' && protocol_advice(PROTOCOL_UNKNOWN)[0] == '\0',
       "nothing when nothing needs updating");

    printf("\nthe listener's own refusal\n");
    ok(protocol_refused_tv("ERR update the TV app: it is too old for DS5-USBIP on the PC\n"),
       "read as the TV being too old");
    ok(!protocol_refused_tv("ERR bad bridge args\n") && !protocol_refused_tv(NULL), "and nothing else is");

    printf("\nwhere the app uses it\n");
    char *bridge = load("src/app/ui_bridge.c");
    char *worker = load("src/app/ctm_gesture_worker.inl");
    ok(bridge && worker, "the app's source can be read");
    if (!bridge || !worker) return 1;
    char *start = between(bridge, "static int bridge_start_command(", "\n}");
    ok(start && strstr(start, "if (compat == PROTOCOL_UPDATE_PC || compat == PROTOCOL_UPDATE_TV) {") != NULL,
       "a bridge is refused before anything is sent when the two cannot work together");
    ok(start && strstr(start, "send_agent_command(\"STATUS\", status, sizeof(status))") != NULL &&
       strstr(start, "send_agent_command(\"STATUS\"") < strstr(start, "if (compat == PROTOCOL_UPDATE_PC"),
       "and when nothing is known yet the listener is asked first");
    ok(start && strstr(start, "\"BRIDGE_START %s 0 %s level=%d\"") != NULL &&
       strstr(start, "\"BRIDGE_START %s %d %s level=%d\"") != NULL,
       "both ways of asking for a bridge say this TV's level");
    ok(start && strstr(start, "protocol_refused_tv(response)") != NULL, "the listener's refusal is read");
    ok(strstr(worker, "protocol_compat(probe)") != NULL && strstr(worker, "g_agent_compat = compat;") != NULL,
       "every probe of the listener decides it again");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
