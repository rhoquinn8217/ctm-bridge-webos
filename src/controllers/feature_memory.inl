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

#endif /* FEATURE_MEMORY_INL */
