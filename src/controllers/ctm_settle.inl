/* How long a freshly-plugged controller needs before its speaker works. */

/* ⭐⭐ WHEN EACH DEVICE NODE WAS FIRST SEEN, and how long a cable needs before
 * its speaker works.
 *
 * ⛔ THE FAULT: plug a DualSense in with a cable during a stream and the first
 * signal -- bridge or refusal -- is silent. Every one after it plays.
 *
 * ⭐ MEASURED, 2026-08-19: wait fifteen seconds after plugging in and the
 * bridge tone plays. The controller's USB AUDIO INTERFACE takes seconds to
 * become usable, and nothing we do at signal time changes that. ⚠️ Which is
 * also why waiting never helped the REFUSAL: that path opens the card at the
 * moment of refusal, so it is always freshly opened -- the clock that matters
 * starts when the CABLE goes in, not when we open.
 *
 * ⛔ Two earlier attempts had the wrong shape and are recorded so they are not
 * retried: a lead-in of silence inside the tone (250 ms cannot cover seconds),
 * and priming keyed per app run (the device is cold on every plug-in, not once).
 *
 * ⓘ Usually costs nothing: a controller connected before the stream started is
 * long since ready and the wait is zero. */
#define FEEDBACK_READY_MS   6000
/* ⚠️ Node paths CLIMB rather than being reused -- hidraw4, hidraw5 and so on
 * within one session -- so this fills faster than the controller count
 * suggests. A full table means every signal waits, which is why it is generous
 * and why the oldest entry is replaced rather than the new one dropped. */
#define FEEDBACK_SEEN_MAX   24

static struct { char node[64]; uint64_t at_ms; } g_seen[FEEDBACK_SEEN_MAX];
static int g_seen_n;
static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t feedback_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Called when a device node appears. */
void ctm_feedback_note_appeared(const char *node)
{
    if (!node || !node[0]) return;
    pthread_mutex_lock(&g_seen_lock);
    for (int i = 0; i < g_seen_n; ++i) {
        if (strcmp(g_seen[i].node, node) == 0) {
            g_seen[i].at_ms = feedback_now_ms();   /* replugged: clock restarts */
            pthread_mutex_unlock(&g_seen_lock);
            return;
        }
    }
    if (g_seen_n < FEEDBACK_SEEN_MAX) {
        snprintf(g_seen[g_seen_n].node, sizeof(g_seen[0].node), "%s", node);
        g_seen[g_seen_n].at_ms = feedback_now_ms();
        ++g_seen_n;
    }
    pthread_mutex_unlock(&g_seen_lock);
}

/* How much longer this node needs before its speaker is worth using. */
static long feedback_settle_left_ms(const char *node)
{
    if (!node || !node[0]) return 0;
    long left = FEEDBACK_READY_MS;   /* ⭐ UNKNOWN MEANS WAIT -- see below */
    bool known = false;
    pthread_mutex_lock(&g_seen_lock);
    for (int i = 0; i < g_seen_n; ++i) {
        if (strcmp(g_seen[i].node, node) == 0) {
            known = true;
            const uint64_t age = feedback_now_ms() - g_seen[i].at_ms;
            left = (age < FEEDBACK_READY_MS) ? (long)(FEEDBACK_READY_MS - age) : 0;
            break;
        }
    }
    /* ⛔⛔ A NODE WE HAVE NEVER SEEN IS TREATED AS BRAND NEW.
     *
     * ⚠️ The first version defaulted to "ready" and it did not work: the log
     * showed `settled 0ms, key=/dev/hidraw4` on a node that had just appeared,
     * so the stamp from the hotplug callback either never arrived or arrived
     * under a different path. ⭐ Rather than depend on that, absence of
     * evidence is treated as absence of readiness -- which is the safe way to
     * be wrong here, because the cost is a short wait and the cost of the other
     * mistake is a signal nobody hears.
     *
     * ⓘ Recorded as seen, so it happens ONCE per node and every later signal on
     * it is immediate. */
    if (!known && g_seen_n < FEEDBACK_SEEN_MAX) {
        snprintf(g_seen[g_seen_n].node, sizeof(g_seen[0].node), "%s", node);
        g_seen[g_seen_n].at_ms = feedback_now_ms();
        ++g_seen_n;
    }
    pthread_mutex_unlock(&g_seen_lock);
    return left;
}

