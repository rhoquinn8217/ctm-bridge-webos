/* Tests for the feature reports a pad leaves unanswered:
 * src/controllers/feature_memory.inl.
 *
 * ⭐ WHY THIS EXISTS (2026-10-29). On webOS a Bluetooth pad's feature reports
 * are answered by the system's Bluetooth service, and the rooted monitor's DS4
 * never answers 0x05: each ask held every hidraw node on the TV for 5 to 10 s,
 * three times at every bridge, and the app's loop waited behind them. A report
 * left unanswered is asked once, then fails at once: for the rest of the
 * connection, and on the same pad's later connections until the app stops.
 * A DualSense is the exception: it is always asked (2026-10-30).
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

    printf("== a DualSense is always asked\n");
    ok(feature_pad_always_asked("054c", "0ce6"), "a DualSense");
    ok(feature_pad_always_asked("054c", "0df2"), "an Edge");
    ok(!feature_pad_always_asked("054c", "05c4") && !feature_pad_always_asked("054c", "09cc"),
       "not either DS4");
    ok(!feature_pad_always_asked("045e", "0ce6"), "not another vendor's 0ce6");
    ok(!feature_pad_always_asked(NULL, "0ce6") && !feature_pad_always_asked("054c", NULL),
       "not a pad with no ids");

    printf("== what a Bluetooth DualSense has answered is kept (rhoquinn8217's B4, 2026-10-30)\n");
    char frost[FEATURE_KEY_LEN], cheat[FEATURE_KEY_LEN];
    feature_pad_key("054c", "0ce6", "7c:66:ef:82:10:ed", "", frost, sizeof(frost));
    feature_pad_key("054c", "0ce6", "a0:fa:9c:ef:9b:30", "", cheat, sizeof(cheat));
    uint8_t said[41], back[64];
    for (unsigned i = 0; i < sizeof(said); ++i) said[i] = (uint8_t)(i + 1);
    ok(!feature_answer_recall(frost, 0x05, sizeof(said), back), "nothing is known before it answers");
    feature_answer_keep(frost, 0x05, said, sizeof(said));
    memset(back, 0, sizeof(back));
    ok(feature_answer_recall(frost, 0x05, sizeof(said), back) && memcmp(back, said, sizeof(said)) == 0,
       "the same question has the same bytes back");
    ok(!feature_answer_recall(frost, 0x05, 64, back), "a question of another length is asked");
    ok(!feature_answer_recall(frost, 0x09, 20, back) && !feature_answer_recall(frost, 0x20, 64, back),
       "its other reports are not answered by this one");
    ok(!feature_answer_recall(cheat, 0x05, sizeof(said), back), "another pad is asked for its own");
    feature_answer_keep(frost, 0x09, said, 20);
    feature_answer_keep(frost, 0x20, said, 41);
    ok(feature_answer_recall(frost, 0x09, 20, back) && feature_answer_recall(frost, 0x20, 41, back) &&
       feature_answer_recall(frost, 0x05, 41, back), "its MAC and firmware are kept beside its calibration");
    feature_answer_keep(frost, 0x08, said, 20);
    feature_answer_keep(frost, 0x80, said, 20);
    ok(!feature_answer_recall(frost, 0x08, 20, back) && !feature_answer_recall(frost, 0x80, 20, back),
       "a report that may change is never kept");
    uint8_t big[FEATURE_ANSWER_BYTES + 1];
    memset(big, 7, sizeof(big));
    feature_answer_keep(cheat, 0x05, big, sizeof(big));
    feature_answer_keep("", 0x05, said, sizeof(said));
    feature_answer_keep(NULL, 0x05, said, sizeof(said));
    ok(!feature_answer_recall(cheat, 0x05, sizeof(big), back) && !feature_answer_recall("", 0x05, sizeof(said), back) &&
       !feature_answer_recall(NULL, 0x05, sizeof(said), back), "too long to keep, or a pad with no key: asked");

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
    ok(unanswered && strstr(unanswered, "feature_pad_always_asked(c->dev.vid, c->dev.pid)") != NULL &&
       strstr(unanswered, "feature_pad_always_asked(c->dev.vid, c->dev.pid)") <
           strstr(unanswered, "feature_set_has(&c->feature_unanswered, id)"),
       "a DualSense is let through before either is read");
    char *note = between(common, "static void feature_wait_note(", "\n}");
    ok(note && strstr(note, "took >= FEATURE_NO_ANSWER_US") != NULL &&
       strstr(note, "feature_set_add(&c->feature_unanswered, id)") != NULL &&
       strstr(note, "feature_pad_remember(key, id)") != NULL,
       "a failed ask of the platform's whole wait is kept for the connection and the pad");
    ok(note && strstr(note, "!feature_pad_always_asked(c->dev.vid, c->dev.pid)") != NULL &&
       strstr(note, "!feature_pad_always_asked(c->dev.vid, c->dev.pid)") <
           strstr(note, "feature_set_add(&c->feature_unanswered, id)"),
       "and a DualSense's failed ask is only logged");

    ok(get && strstr(get, "feature_answer_earlier(c, payload[0], h->payload_len, earlier)") != NULL &&
       strstr(get, "feature_answer_earlier(c, payload[0], h->payload_len, earlier)") < strstr(get, "HIDIOCGFEATURE"),
       "the pad's earlier answer is looked for before it is asked");
    ok(get && strstr(get, "feature_answer_keep(key, payload[0], feature, h->payload_len)") != NULL &&
       strstr(get, "HIDIOCGFEATURE") < strstr(get, "feature_answer_keep(key, payload[0], feature, h->payload_len)"),
       "and only an answer it really gave is kept");
    char *kept_for = between(common, "static bool feature_answers_kept_for(", "\n}");
    ok(kept_for && strstr(kept_for, "c->ops->speaks_ds5") != NULL && strstr(kept_for, "\"BT\"") != NULL,
       "only for a DualSense or an Edge, only over Bluetooth");
    ok(strstr(common, "!(strcmp(c->dev.bus, \"BT\") == 0 && bt_pad_sends_full_reports(fd))") != NULL,
       "a Bluetooth pad already sending its full reports is not asked for them");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
