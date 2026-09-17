/* A Bluetooth Xbox pad's last state report, kept so it can be sent again.
 *
 * ⭐⭐ WHY (rhoquinn8217, 2026-09-16, an Xbox One S pad on the U5s): over
 * Bluetooth the pad reports only when something changes. Put down, it sends
 * nothing, and the listener ends a gamepad session after 15 s of silence.
 * Measured: 0.2 reports a second at rest, and the bridge gone 15 s after the
 * last one, on the pad's old firmware (02fd) and its updated one (0b20) alike.
 * A cabled Xbox pad never had this, because the TV builds its report and sends
 * it every 4 ms (controller_xpad.c).
 * ➡️ So the TV keeps the last state report the pad sent, and the pump sends it
 * again whenever keepalive_ms pass with nothing sent: the same bytes the pad
 * last reported, as often as a cabled pad's.
 *
 * ⛔ ONLY REPORT 0x01. It is the state the listener's Xbox map reads
 * (maps/xbox_gip_usb_over_xbox_bt.map, source_report = 0x01). The pad also
 * sends 0x04, its battery, every 20 s (seen as "04 86"), and sending that again
 * would say nothing about what is pressed.
 *
 * ⓘ Bytes only, no I/O, so tests/test_xbox_bt_report.c can hold it. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define XBOX_BT_STATE_REPORT_ID  0x01
#define XBOX_BT_REPORT_MAX       64

typedef struct {
    uint8_t report[XBOX_BT_REPORT_MAX];
    size_t len;   /* 0 until the pad's first state report */
} xbox_bt_last_t;

/* Keep a report the pad sent, if it is its state report. Returns 1 if kept. */
static inline int xbox_bt_keep(xbox_bt_last_t *last, const uint8_t *report, size_t len)
{
    if (!last || !report || len < 2 || len > sizeof(last->report)) return 0;
    if (report[0] != XBOX_BT_STATE_REPORT_ID) return 0;
    memcpy(last->report, report, len);
    last->len = len;
    return 1;
}

/* ⭐⭐ NOTHING PRESSED, for while the TV's overlay holds input (rhoquinn8217,
 * 2026-09-16: the pad's input reached the PC behind the open overlay, because
 * this type had no blank at all). The four sticks centred (16-bit, 0x8000) and
 * everything after them zero: triggers, d-pad and buttons. ⓘ The same bytes the
 * cabled type's xpad_blank_report writes, because the listener's Xbox map reads
 * both pads at the same offsets; it is not shared only because xpad_report.inl
 * brings functions this type would never use. */
#define XBOX_BT_STATE_REPORT_LEN 17

static inline void xbox_bt_blank_report(uint8_t *data, size_t len)
{
    if (!data || len < XBOX_BT_STATE_REPORT_LEN || data[0] != XBOX_BT_STATE_REPORT_ID) return;
    for (size_t i = 0; i < 4; ++i) {
        data[1 + 2 * i] = 0x00;
        data[2 + 2 * i] = 0x80;
    }
    memset(data + 9, 0, XBOX_BT_STATE_REPORT_LEN - 9);
}

/* The pad's present state as it last reported it, copied out: its length, or
 * 0 when nothing has been kept yet or it does not fit. */
static inline size_t xbox_bt_current(const xbox_bt_last_t *last, uint8_t *out, size_t cap)
{
    if (!last || !out || last->len == 0 || last->len > cap) return 0;
    memcpy(out, last->report, last->len);
    return last->len;
}
