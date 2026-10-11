/* When a cabled DualSense's microphone capture recovers from an error, and how
 * long it waits first. The decisions only, apart from the ALSA calls, so a
 * test can drive them.
 *
 * ⛔⛔ WHY (code review, 2026-10-05). Two faults in the recovery:
 * - A cable pulled from a cabled DualSense made poll() report an error at
 *   once, every time, and the error was answered by preparing the device and
 *   polling again, with no pause and no count. A CPU core spun until the pad
 *   was released, and the self-heal that would have ended it was never asked.
 * - The self-heal reopened the microphone by SCANNING for the first free
 *   DualSense card. With a second DualSense on a cable and not bridged, that
 *   is the other pad's microphone, sent to the PC as this pad's.
 * ➡️ Every error counts. A run of poll errors waits a little longer each time
 * and then asks for a reopen, and the reopen takes the card this capture was
 * using and nothing else (controller_common.c, mic_capture_thread).
 *
 * ⓘ The two thresholds are the ones the recovery always had: twenty failures
 * in a row, or five recoveries in a row. */

typedef enum {
    MIC_ERR_OVERRUN,   /* the read ran behind: re-prepared, carry on */
    MIC_ERR_AGAIN,     /* nothing to read yet */
    MIC_ERR_POLL,      /* poll() itself reported an error on the device */
    MIC_ERR_GONE       /* any other read error: the device is gone or broken */
} mic_err_t;

typedef enum { MIC_RETRY, MIC_REOPEN } mic_action_t;

typedef struct {
    int consec_fail;    /* failures in a row */
    int retry_streak;   /* recoveries in a row */
} mic_recover_t;

#define MIC_RECOVER_FAILS     20
#define MIC_RECOVER_RETRIES   5
#define MIC_POLL_WAIT_STEP_MS 10
#define MIC_POLL_WAIT_MAX_MS  200

/* A read that worked: the runs start over. */
static inline void mic_recover_ok(mic_recover_t *r)
{
    r->consec_fail = 0;
    r->retry_streak = 0;
}

/* One error. Sets how long to wait before going on, which is never zero for a
 * poll error (that was the spin), and says whether to reopen. */
static inline mic_action_t mic_recover_error(mic_recover_t *r, mic_err_t e, int *wait_ms)
{
    r->consec_fail++;
    *wait_ms = 0;
    switch (e) {
    case MIC_ERR_GONE:
        return MIC_REOPEN;
    case MIC_ERR_AGAIN:
        r->retry_streak++;
        *wait_ms = 1;
        break;
    case MIC_ERR_OVERRUN:
        r->retry_streak++;
        break;
    case MIC_ERR_POLL:
        r->retry_streak++;
        *wait_ms = MIC_POLL_WAIT_STEP_MS * r->retry_streak;
        if (*wait_ms > MIC_POLL_WAIT_MAX_MS) *wait_ms = MIC_POLL_WAIT_MAX_MS;
        break;
    }
    if (r->consec_fail >= MIC_RECOVER_FAILS || r->retry_streak >= MIC_RECOVER_RETRIES) {
        return MIC_REOPEN;
    }
    return MIC_RETRY;
}
