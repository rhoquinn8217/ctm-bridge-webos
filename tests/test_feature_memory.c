/* Tests for the feature reports a pad leaves unanswered:
 * src/controllers/feature_memory.inl.
 *
 * ⭐ WHY THIS EXISTS (2026-10-29). On webOS a Bluetooth pad's feature reports
 * are answered by the system's Bluetooth service, and the rooted monitor's DS4
 * never answers 0x05: each ask held every hidraw node on the TV for 5 to 10 s,
 * three times at every bridge, and the app's loop waited behind them. A report
 * left unanswered is asked once, then fails at once: for the rest of the
 * connection, and on the same pad's later connections until the app stops.
 * These pin that, and read controller_common.c for where the session uses it.
 *
 * ➡️ Build and run:  cc tests/test_feature_memory.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <stdlib.h>

#include "../src/controllers/feature_memory.inl"

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

int main(void)
{
    printf("== one bit per report id\n");
    feature_set_t s;
    memset(&s, 0, sizeof(s));
    feature_set_add(&s, 0x05);
    feature_set_add(&s, 0xff);
    ok(feature_set_has(&s, 0x05) && feature_set_has(&s, 0xff), "0x05 and 0xff kept");
    ok(!feature_set_has(&s, 0x04) && !feature_set_has(&s, 0x06) && !feature_set_has(&s, 0x00) &&
       !feature_set_has(&s, 0x85), "their neighbours, 0x00 and 0x85 not");

    printf("== a pad's key\n");
    char key[FEATURE_KEY_LEN];
    feature_pad_key("054c", "05c4", "30:0e:d5:a9:69:51", "30:0e:d5:a9:69:51", key, sizeof(key));
    ok(strcmp(key, "054c:05c4 30:0e:d5:a9:69:51") == 0, "the ids and the serial");
    feature_pad_key("054c", "05c4", "", "30:0e:d5:a9:69:51", key, sizeof(key));
    ok(strcmp(key, "054c:05c4 30:0e:d5:a9:69:51") == 0, "the MAC when there is no serial");
    feature_pad_key("054c", "0ce6", "a0-fa-9c-ef-9b-30", "", key, sizeof(key));
    ok(strcmp(key, "054c:0ce6 a0-fa-9c-ef-9b-30") == 0, "the serial when there is no MAC");
    feature_pad_key("054c", "05c4", "", "", key, sizeof(key));
    ok(key[0] == '\0', "nothing when the pad has neither");

    printf("== remembered per pad, until the app stops\n");
    char ds4[FEATURE_KEY_LEN], other_ds4[FEATURE_KEY_LEN], ds5[FEATURE_KEY_LEN];
    feature_pad_key("054c", "05c4", "30:0e:d5:a9:69:51", "", ds4, sizeof(ds4));
    feature_pad_key("054c", "05c4", "aa:bb:cc:dd:ee:ff", "", other_ds4, sizeof(other_ds4));
    feature_pad_key("054c", "0ce6", "30:0e:d5:a9:69:51", "", ds5, sizeof(ds5));
    ok(!feature_pad_remembers(ds4, 0x05), "nothing known before an ask fails");
    feature_pad_remember(ds4, 0x05);
    ok(feature_pad_remembers(ds4, 0x05), "the pad's next connection does not ask 0x05");
    ok(!feature_pad_remembers(ds4, 0x02), "but still asks its other reports");
    ok(!feature_pad_remembers(other_ds4, 0x05), "another DS4 still asks 0x05");
    ok(!feature_pad_remembers(ds5, 0x05), "another kind of pad with the same address still asks it");
    feature_pad_remember("", 0x05);
    ok(!feature_pad_remembers("", 0x05) && !feature_pad_remembers(NULL, 0x05),
       "a pad with no key is remembered per connection only");
    feature_pad_remember(ds4, 0x02);
    ok(feature_pad_remembers(ds4, 0x05) && feature_pad_remembers(ds4, 0x02),
       "a second report adds to the same pad's");

    printf("== the table full\n");
    char many[FEATURE_PADS_MAX + 1][FEATURE_KEY_LEN];
    for (int i = 0; i <= FEATURE_PADS_MAX; ++i) {
        char mac[32];
        snprintf(mac, sizeof(mac), "00:00:00:00:00:%02x", i);
        feature_pad_key("054c", "05c4", mac, "", many[i], sizeof(many[i]));
        feature_pad_remember(many[i], 0x05);
    }
    /* The DS4 above took one slot, so of these nine the first seven fit. */
    ok(feature_pad_remembers(many[0], 0x05) && feature_pad_remembers(many[FEATURE_PADS_MAX - 2], 0x05),
       "pads up to the table's size are kept");
    ok(!feature_pad_remembers(many[FEATURE_PADS_MAX - 1], 0x05) &&
       !feature_pad_remembers(many[FEATURE_PADS_MAX], 0x05),
       "past it, a pad is kept for its connection only");
    ok(feature_pad_remembers(ds4, 0x05), "and the pads already kept stay kept");

    printf("== where the session uses it (controller_common.c)\n");
    char *common = load("src/controllers/controller_common.c");
    ok(common != NULL, "controller_common.c read");
    char *get = between(common, "} else if (h->type == CTMB_MSG_FEATURE_GET) {", "} else if (h->type == CTMB_MSG_FEATURE_SET) {");
    ok(get && strstr(get, "!feature_unanswered(c, payload[0])") != NULL &&
       strstr(get, "!feature_unanswered(c, payload[0])") < strstr(get, "HIDIOCGFEATURE"),
       "a request is checked before the pad is asked");
    ok(get && strstr(get, "if (!ok) (void)c_send(c, CTMB_MSG_FEATURE_REPORT, 0,") != NULL,
       "and a report not asked is answered as failed, at once");
    char *unanswered = between(common, "static bool feature_unanswered(", "\n}");
    ok(unanswered && strstr(unanswered, "feature_set_has(&c->feature_unanswered, id)") != NULL &&
       strstr(unanswered, "feature_pad_remembers(key, id)") != NULL,
       "this connection's set first, then the pad's earlier connections");
    char *note = between(common, "static void feature_wait_note(", "\n}");
    ok(note && strstr(note, "took >= FEATURE_NO_ANSWER_US") != NULL &&
       strstr(note, "feature_set_add(&c->feature_unanswered, id)") != NULL &&
       strstr(note, "feature_pad_remember(key, id)") != NULL,
       "a failed ask of the platform's whole wait is kept for the connection and the pad");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
