/* How long a device's input thread may sleep before it has work to do.
 *
 * ⛔ WHY (code review, 2026-10-05). It slept 1 ms at a time whatever the
 * device: a thousand wake-ups a second for every bridged device. A report
 * wakes it anyway, so a sleep that ends with nothing to read brings only two
 * jobs: the keepalive, which is due `keepalive_ms` after the last thing sent
 * (every 4 ms for an Xbox pad, 50 for a generic one, never for a DualSense),
 * and the keyboard's handover to the TV's overlay, which a person cannot tell
 * from instant at 20 ms.
 *
 * ➡️ So: until the keepalive is due, and never longer than the cap. At once
 * (1 ms) when it is due or overdue, and when nothing has been sent yet. ⓘ The
 * floor is 1 ms, never 0: a pad with nothing to send leaves the keepalive
 * overdue, and 0 would spin where the old code slept.
 *
 * ⓘ Its own file so a test can pin it. */

#include <stdint.h>

#define INPUT_WAIT_CAP_MS 20

static inline int input_wait_ms(uint64_t now_us, uint64_t last_input_us, unsigned keepalive_ms)
{
    if (!keepalive_ms) return INPUT_WAIT_CAP_MS;
    if (last_input_us == 0) return 1;
    const uint64_t due = last_input_us + (uint64_t)keepalive_ms * 1000u;
    if (now_us >= due) return 1;
    const uint64_t left_ms = (due - now_us + 999u) / 1000u;   /* round up: never early */
    if (left_ms < 1) return 1;
    return left_ms > INPUT_WAIT_CAP_MS ? INPUT_WAIT_CAP_MS : (int)left_ms;
}
