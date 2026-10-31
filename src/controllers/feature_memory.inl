/* Which feature reports a pad gives no answer to, so they are not asked again.
 *
 * ⭐ WHY (2026-10-29). The kernel holds one lock over every hidraw node for the
 * whole of a feature ioctl, and on webOS a Bluetooth pad is a uhid device whose
 * answers come from the system's Bluetooth service. The rooted monitor's
 * Bluetooth DS4 never answers 0x05, its calibration: the PC asks three times
 * at every bridge, 5 to 10 s each, and every other hidraw open, ioctl and
 * write on the TV waits behind each one (the app's loop sat 15 s in
 * hidraw_open). So a report the pad left unanswered for the platform's whole
 * wait fails at once afterwards, as the pad's own answer did:
 *   - for the rest of that connection (the set on the controller), and
 *   - on the same pad's later connections, known by its vendor and product ids
 *     and its serial or MAC, until the app stops. A new start asks once again.
 *
 * ⭐ A DUALSENSE IS ALWAYS ASKED (rhoquinn8217, 2026-10-30: "the DualSense take
 * precedence over any other controller"). On the B4 the Bluetooth service
 * stopped answering every feature request for a while, and a healthy DualSense,
 * which had answered these reports minutes before, was kept as not answering
 * its calibration, its MAC and its firmware until the app stopped. So a
 * DualSense or an Edge waits as it always did, and the wait is only logged.
 *
 * ⓘ Bits and one lock, no I/O, so tests/test_feature_memory.c can hold it. */

#ifndef FEATURE_MEMORY_INL
#define FEATURE_MEMORY_INL

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* An ask that took this long and failed was the platform's whole wait. */
#define FEATURE_NO_ANSWER_US 4000000ull
#define FEATURE_PADS_MAX 8
#define FEATURE_KEY_LEN 96

/* One bit per report id. */
typedef struct {
    uint8_t bits[32];
} feature_set_t;

static inline bool feature_set_has(const feature_set_t *s, unsigned id)
{
    return (s->bits[(id & 0xffu) >> 3] >> (id & 7u)) & 1u;
}

static inline void feature_set_add(feature_set_t *s, unsigned id)
{
    s->bits[(id & 0xffu) >> 3] |= (uint8_t)(1u << (id & 7u));
}

/* A pad's key, as "054c:05c4 30:0e:d5:a9:69:51": its serial, else its MAC.
 * "" when it has neither; such a pad is remembered per connection only. */
static inline void feature_pad_key(const char *vid, const char *pid, const char *serial,
                                   const char *mac, char *out, size_t len)
{
    if (!out || len == 0) return;
    const char *who = (serial && serial[0]) ? serial : (mac ? mac : "");
    if (!who[0]) {
        out[0] = '\0';
        return;
    }
    snprintf(out, len, "%s:%s %s", vid ? vid : "", pid ? pid : "", who);
}

/* Never kept as unanswered: a DualSense or an Edge, by its ids. */
static inline bool feature_pad_always_asked(const char *vid, const char *pid)
{
    return vid && pid && strcmp(vid, "054c") == 0 &&
           (strcmp(pid, "0ce6") == 0 || strcmp(pid, "0df2") == 0);
}

typedef struct {
    char key[FEATURE_KEY_LEN];
    feature_set_t unanswered;
} feature_pad_t;

static feature_pad_t s_feature_pads[FEATURE_PADS_MAX];
static pthread_mutex_t s_feature_pads_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Did this pad leave `id` unanswered on an earlier connection? */
static inline bool feature_pad_remembers(const char *key, unsigned id)
{
    if (!key || !key[0]) return false;
    bool found = false;
    pthread_mutex_lock(&s_feature_pads_mutex);
    for (int i = 0; i < FEATURE_PADS_MAX; ++i) {
        if (strcmp(s_feature_pads[i].key, key) == 0) {
            found = feature_set_has(&s_feature_pads[i].unanswered, id);
            break;
        }
    }
    pthread_mutex_unlock(&s_feature_pads_mutex);
    return found;
}

/* Keeps `id` as one this pad leaves unanswered. ⓘ With every slot taken by
 * other pads, it is kept for the connection only. */
static inline void feature_pad_remember(const char *key, unsigned id)
{
    if (!key || !key[0]) return;
    pthread_mutex_lock(&s_feature_pads_mutex);
    int slot = -1;
    for (int i = 0; i < FEATURE_PADS_MAX && slot < 0; ++i) {
        if (strcmp(s_feature_pads[i].key, key) == 0) slot = i;
    }
    for (int i = 0; i < FEATURE_PADS_MAX && slot < 0; ++i) {
        if (!s_feature_pads[i].key[0]) slot = i;
    }
    if (slot >= 0) {
        snprintf(s_feature_pads[slot].key, sizeof(s_feature_pads[slot].key), "%s", key);
        feature_set_add(&s_feature_pads[slot].unanswered, id);
    }
    pthread_mutex_unlock(&s_feature_pads_mutex);
}

/* ⭐⭐ WHAT A BLUETOOTH DUALSENSE HAS ANSWERED IS KEPT, SO IT IS NOT ASKED AGAIN
 * (rhoquinn8217's B4, 2026-10-30). Every drop of both pads with a log to read began
 * with a feature request the Bluetooth service left unanswered, 5 s at a time, and
 * the service itself died within seconds of it: 10 s after the first, on the
 * clearest run. One bridge in about 25 did it, with the signal waiting well clear.
 * And each bridge asked the pad the same three things, none of which changes while
 * it is on: its calibration (0x05), its MAC and pairing (0x09), its firmware (0x20).
 * So the first answer stands for that pad until the app stops, in the same bytes. */
#define FEATURE_ANSWER_BYTES 64

typedef struct {
    bool have;
    uint16_t len;
    uint8_t data[FEATURE_ANSWER_BYTES];
} feature_answer_t;

typedef struct {
    char key[FEATURE_KEY_LEN];
    feature_answer_t answer[3];
} feature_answers_t;

static feature_answers_t s_feature_answers[FEATURE_PADS_MAX];

/* Where `id` is kept, or -1 for a report that may change and is always asked. */
static inline int feature_answer_slot(unsigned id)
{
    switch (id) {
    case 0x05: return 0;
    case 0x09: return 1;
    case 0x20: return 2;
    default:   return -1;
    }
}

/* ⓘ With every slot taken by other pads, this pad is simply asked each time. */
static inline void feature_answer_keep(const char *key, unsigned id, const uint8_t *data, size_t len)
{
    const int at = feature_answer_slot(id);
    if (!key || !key[0] || !data || at < 0 || len == 0 || len > FEATURE_ANSWER_BYTES) return;
    pthread_mutex_lock(&s_feature_pads_mutex);
    int slot = -1;
    for (int i = 0; i < FEATURE_PADS_MAX && slot < 0; ++i) {
        if (strcmp(s_feature_answers[i].key, key) == 0) slot = i;
    }
    for (int i = 0; i < FEATURE_PADS_MAX && slot < 0; ++i) {
        if (!s_feature_answers[i].key[0]) slot = i;
    }
    if (slot >= 0) {
        snprintf(s_feature_answers[slot].key, sizeof(s_feature_answers[slot].key), "%s", key);
        feature_answer_t *a = &s_feature_answers[slot].answer[at];
        a->have = true;
        a->len = (uint16_t)len;
        memcpy(a->data, data, len);
    }
    pthread_mutex_unlock(&s_feature_pads_mutex);
}

/* Copies out what the pad said to the same question: the same report, the same
 * length. False when it has not been asked that. */
static inline bool feature_answer_recall(const char *key, unsigned id, size_t len, uint8_t *out)
{
    const int at = feature_answer_slot(id);
    if (!key || !key[0] || !out || at < 0) return false;
    bool found = false;
    pthread_mutex_lock(&s_feature_pads_mutex);
    for (int i = 0; i < FEATURE_PADS_MAX; ++i) {
        if (strcmp(s_feature_answers[i].key, key) == 0) {
            const feature_answer_t *a = &s_feature_answers[i].answer[at];
            if (a->have && a->len == len) {
                memcpy(out, a->data, len);
                found = true;
            }
            break;
        }
    }
    pthread_mutex_unlock(&s_feature_pads_mutex);
    return found;
}

#endif /* FEATURE_MEMORY_INL */
