/* --- the unplug gesture's worker ----------------------------------------
 *
 * OURS, NOT UPSTREAM'S. The gesture is a feature we added; the bridge works
 * without it. Keeping it in its own file means an upstream change to the
 * Bluetooth path cannot collide with it, and removing it is deleting one
 * include.
 *
 * Included rather than compiled on its own because it reaches the session
 * table and the logical device list, both defined in ui_bridge.c and private
 * to it.
 */

/* ---- local unplug gesture -------------------------------------------------
 * A controller type can ask to be unplugged from inside its own input thread
 * (see ctm_controller_request_unplug). It must not do the unplug itself: that
 * joins the very thread making the request. So the request only wakes this
 * worker, which does the work on a thread of its own.
 *
 * The worker sleeps on a condition variable rather than polling -- it costs
 * nothing until a gesture actually happens. */
static pthread_mutex_t g_gesture_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gesture_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_gesture_thread;
/* Is a gesture worker running RIGHT NOW -- not "was one ever started".
 *
 * The worker's loop ends when g_running goes false, which the bridge does on
 * every teardown. That is correct: with no sessions there is nothing for it to
 * do. The fault was that it never came back, because this was set once and
 * never cleared, so the next plug refused to create a replacement while
 * g_running had already returned to true. Every unplug request after the first
 * teardown was then raised into nothing, for the life of the process.
 *
 * The stopSniff worker in the glue already works this way -- its flag is
 * cleared as it is torn down, which is why it restarts and this one did not.
 * Cleared by the worker itself on the way out, so no caller has to remember. */
static volatile bool g_gesture_worker_running;
static bool g_gesture_pending;

/* When the PROBE THREAD entered its agent probe, in monotonic microseconds; 0 when
 * it is not in one.
 *
 * Written by the worker, read by whichever controller thread raises a request.
 * Deliberately unlocked: a stale read costs a slightly wrong number in a log
 * line, and taking a lock here would put the input thread behind the very
 * thread being investigated. */
static volatile uint64_t g_probe_entered_us;

static uint64_t probe_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

/* Invoked on the requesting controller's input thread: signal only. */
static void gesture_requested(ctm_controller_t *c)
{
    (void)c;
    /* The question this answers: was the worker able to hear this?
     *
     * The worker leaves its wait every few seconds to ask the agent whether it
     * is reachable. That ask opens a socket, and a connect to a host that is
     * not answering has no timeout of its own -- so the worker can be parked
     * inside it while requests pile up unserviced, with nothing to show for it.
     * A line here, only when a request is actually raised, says whether that
     * is happening without logging every probe that goes well. */
    uint64_t entered = g_probe_entered_us;
    if (entered) {
        ctm_gesture_log(NULL, "WORKER IS IN AN AGENT PROBE, %llums so far -- request may wait",
                        (unsigned long long)((probe_now_us() - entered) / 1000));
    }
    pthread_mutex_lock(&g_gesture_mutex);
    g_gesture_pending = true;
    pthread_cond_signal(&g_gesture_cond);
    pthread_mutex_unlock(&g_gesture_mutex);
}

/* How often the worker re-checks that the agent is answering. Short enough
 * that the header is not misleading, long enough to be free. */
#define AGENT_PROBE_INTERVAL_MS 3000

static void *gesture_worker(void *arg)
{
    (void)arg;
    /* Started once per process and never replaced, so knowing when it begins
     * and when it leaves is the difference between "the request went nowhere"
     * and "there was nobody left to hear it". Two lines, no behaviour. */
    ctm_gesture_log(NULL, "worker started");
    while (g_running) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += AGENT_PROBE_INTERVAL_MS / 1000;

        pthread_mutex_lock(&g_gesture_mutex);
        while (!g_gesture_pending && g_running) {
            if (pthread_cond_timedwait(&g_gesture_cond, &g_gesture_mutex,
                                       &deadline) == ETIMEDOUT) {
                break;
            }
        }
        bool gesture = g_gesture_pending;
        g_gesture_pending = false;
        pthread_mutex_unlock(&g_gesture_mutex);
        if (!g_running) {
            break;
        }

        /* Woke on the timer with nothing to do. The agent probe used to run
         * here and no longer does -- see the probe thread below. */
        if (!gesture) {
            continue;
        }

        /* Which controller asked? Copy the keys out first: stop_session edits
         * the session table as it goes. */
        char keys[MAX_SESSIONS][96];
        int n = 0;
        for (int i = 0; i < g_session_count && n < MAX_SESSIONS; ++i) {
            if (g_sessions[i].controller &&
                ctm_controller_unplug_requested(g_sessions[i].controller)) {
                snprintf(keys[n], sizeof(keys[0]), "%s", g_sessions[i].key);
                ++n;
            }
        }
        /* The pair either side of stop_session is the point: a teardown that
         * wedges shows as "stopping" with no matching "stopped", and the
         * worker is a SINGLE thread, so nothing behind it gets serviced. */
        ctm_gesture_log(NULL, "worker woke: %d of %d session(s) asking to unplug",
                        n, g_session_count);
        for (int i = 0; i < n; ++i) {
            log_append("gesture: unplugging %s", keys[i]);
            ctm_gesture_log(NULL, "stopping %s", keys[i]);
            stop_session(keys[i]);
            ctm_gesture_log(NULL, "stopped %s", keys[i]);
        }
    }
    /* Last thing before the thread dies, so the next plug can create a
     * replacement. Both exits from the loop land here. */
    g_gesture_worker_running = false;
    ctm_gesture_log(NULL, "worker exiting -- the next plug will start a new one");
    return NULL;
}

/* Start the gesture worker once. When: first plug of a session. */
/* --- the agent probe, on a thread of its own ------------------------------
 *
 * WHAT IT IS FOR, and it is less than it looks: this keeps ONE FLAG honest --
 * whether the host is answering -- and that flag has exactly one visible
 * consumer, the overlay's header. Plugging does not depend on it: those paths
 * look for the agent themselves and then actually try to connect, so a dead
 * listener is reported by the attempt rather than by the flag.
 *
 * WHY IT IS NOT SIMPLY DELETED: it is the ONLY thing that ever notices the
 * host has gone away. Discovery returns immediately once the address is known
 * without touching the network, so the UI timer that calls it does nothing
 * during a stream. Remove this and the header would read "online" forever,
 * including after the listener dies.
 *
 * WHY IT MOVED OFF THE GESTURE WORKER: the call takes up to a second, and the
 * worker is what services unplugs. Measured on a remote link 2026-08-10:
 * seven consecutive failures at ~1030 ms each, while a stream played
 * perfectly over the same connection. Every one of those was a second in
 * which an unplug request would have sat unserviced.
 *
 * The one-second ceiling is not ours and does not need adding: Linux applies
 * SO_SNDTIMEO to connect(), which is why every failure measured 1003-1035 ms
 * rather than the minutes an unbounded connect would take. Earlier notes in
 * this project describing the call as unbounded are wrong.
 *
 * On its own thread rather than the interface thread, for the reason the
 * original comment gave: asking must never cost the UI anything. */
static pthread_t g_probe_thread;
static volatile bool g_probe_running;

/* ⭐ TEN SECONDS, not four. ⓘ Raised on 2026-08-20 because the probe no longer
 * needs to be quick: opening the panel asks for one immediately, so the
 * interval only has to catch changes nobody is watching for. ⚠️ Each probe
 * against a host that is not answering costs a full second of this thread, so a
 * long interval is worth having. */
#define AGENT_PROBE_INTERVAL_US 10000000

/* ⭐⭐ THE PROBE OUTLIVES A STREAM, and that is the fix rather than a preference.
 *
 * ⛔ THE FAULT: the loop used to run `while (g_running)`, and ctm_bridge_stop
 * sets that false at the end of every stream. The restart is guarded by
 * `if (!g_probe_running)`, which is correct on its own -- but the thread sleeps
 * for seconds at a time, so a stream ending and a new one starting inside one
 * sleep meant the restart was DECLINED (the flag was still set) and the thread
 * then woke and exited anyway. ➡️ After that there was no probe at all, and the
 * panel reported whatever it last saw -- stuck ONLINE or stuck OFFLINE.
 *
 * ⓘ Confirmed by timestamp: `agent probe thread exiting` at 1787282211, and two
 * hours later the app was still streaming with a frozen reading on screen.
 *
 * ⭐ Nothing about asking whether a host is reachable depends on a bridge
 * running, so the coupling was wrong as well as broken. */
static volatile bool g_probe_want;

/* ⭐⭐ ASK FOR A PROBE NOW, rather than waiting out the interval.
 *
 * ⓘ Set when the USB Bridge panel opens -- the one moment somebody is
 * definitely looking at the answer. The thread wakes, probes, and the panel
 * picks the result up on its next refresh.
 *
 * ⛔ NOT A PROBE ON THE CALLER'S THREAD. The call takes up to a second against
 * a host that is not answering, and the panel opens on the interface thread --
 * a second of frozen overlay is exactly the fault T-062 is about. */
static volatile bool g_probe_now;

void ctm_agent_probe_soon(void)
{
    g_probe_now = true;
}

static void *agent_probe_thread(void *arg)
{
    (void)arg;
    ctm_gesture_log(NULL, "agent probe thread started");
    while (g_probe_want) {
        if (g_agent_host[0]) {
            char probe[256];
            /* Marked for the whole call, so a gesture raised while this runs
             * can say so. Logged afterwards only when it was slow: this runs
             * every few seconds, and a line each time would bury the gesture
             * lines this file exists for. */
            uint64_t t0 = probe_now_us();
            g_probe_entered_us = t0;
            g_agent_online =
                send_agent_command("STATUS", probe, sizeof(probe)) == 0;
            g_probe_entered_us = 0;
            uint64_t took_ms = (probe_now_us() - t0) / 1000;
            if (took_ms > 250) {
                ctm_gesture_log(NULL, "agent probe took %llums (%s)",
                                (unsigned long long)took_ms,
                                g_agent_online ? "online" : "OFFLINE");
            }
        }
        /* ⓘ Slept in short steps so a request can be answered promptly without
         * probing any harder when nobody has asked. */
        for (int i = 0; i < 20 && g_probe_want && !g_probe_now; ++i) {
            usleep(AGENT_PROBE_INTERVAL_US / 20);
        }
        g_probe_now = false;
    }
    /* ⛔⛔ THIS FLAG MUST BE CLEARED HERE, AND FOR YEARS IT WAS NOT ENOUGH.
     *
     * ⚠️ THE FAULT IT CAUSED: the thread loops `while (g_running)`, and
     * ctm_bridge_stop sets g_running false at the end of every stream -- so the
     * probe died with the first stream that ended. The start below is guarded
     * by `if (!g_probe_running)`, which is correct, but the thread has to
     * actually reach this line for a later start to be allowed.
     *
     * ⓘ Confirmed by timestamp on 2026-08-20: `agent probe thread exiting` at
     * 1787282211, and two hours later the app was still streaming with the
     * panel showing a frozen ONLINE while the last real reading had been
     * OFFLINE. ➡️ After it dies the panel reports whatever it last saw, in
     * either direction. */
    g_probe_running = false;
    ctm_gesture_log(NULL, "agent probe thread exiting");
    return NULL;
}

void ctm_bridge_gesture_init(void)
{
    if (g_gesture_worker_running) {
        return;
    }
    ctm_controller_set_unplug_cb(gesture_requested);
    /* Set BEFORE the thread starts: the worker clears it on its way out, and
     * setting it afterwards would race a worker that exited immediately.
     *
     * Detached because nothing joins it. A worker that has ended has already
     * cleared the flag, so the thread handle it left behind is never used
     * again -- joining it would only be to reclaim it, which detaching does
     * for us. */
    g_gesture_worker_running = true;
    if (pthread_create(&g_gesture_thread, NULL, gesture_worker, NULL) == 0) {
        pthread_detach(g_gesture_thread);
    } else {
        g_gesture_worker_running = false;
        ctm_gesture_log(NULL, "worker could not be started");
    }

    /* Started alongside, and separately: a probe that cannot start must not
     * stop unplugs from working.
     *
     * ⭐ Set BEFORE the check: if a thread is already running it simply keeps
     * going, and if one was on its way out this is what a fresh one will read. */
    g_probe_want = true;
    if (!g_probe_running) {
        g_probe_running = true;
        if (pthread_create(&g_probe_thread, NULL, agent_probe_thread, NULL) == 0) {
            pthread_detach(g_probe_thread);
        } else {
            g_probe_running = false;
            ctm_gesture_log(NULL, "agent probe thread could not be started");
        }
    }
}

/* Plug whichever device owns this /dev/hidrawN. When: a local gesture named a
 * device by its node -- the only identifier that distinguishes two otherwise
 * identical controllers. Enumerates first, so a device connected since the last
 * scan is still found. */
bool plug_in_by_node(const char *node)
{
    if (!node || !node[0]) {
        return false;
    }
    enumerate_devices(&g_scan);
    build_logical_devices(&g_scan, &g_devices);

    for (int i = 0; i < g_devices.count; ++i) {
        logical_device_t *item = &g_devices.items[i];
        for (int k = 0; k < item->device_count; ++k) {
            int j = item->device_indices[k];
            if (j < 0 || j >= g_scan.count) {
                continue;
            }
            if (strcmp(g_scan.devices[j].node, node) != 0) {
                continue;
            }
            if (plug_key_is_set(item->key)) {
                log_append("gesture: %s is already plugged", item->name);
                /* Each refusal below also goes to the gesture log. The reasons
                 * were already recorded, but only to the app's console, which
                 * cannot be read on this platform -- so a refusal arrived as a
                 * bare "refused" with four possible causes and no way to tell
                 * them apart. */
                ctm_gesture_log(NULL, "refused: %s is already plugged (%s)",
                                item->name, node);
                return false;
            }
            if (plug_in_item(item)) {
                item->plugged = true;
                log_append("gesture: plugged %s (%s)", item->name, node);
                return true;
            }
            log_append("gesture: could not plug %s (%s)", item->name, node);
            ctm_gesture_log(NULL, "refused: the plug attempt failed for %s (%s)",
                            item->name, node);
            return false;
        }
    }
    log_append("gesture: no device owns %s", node);
    ctm_gesture_log(NULL, "refused: no device owns %s", node);
    return false;
}
