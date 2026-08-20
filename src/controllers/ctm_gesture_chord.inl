/* --- the unplug gesture, DualSense side ----------------------------------
 *
 * OURS, NOT UPSTREAM'S. A DualSense bridges perfectly well without a gesture;
 * this is an addition. Keeping it in its own file means an upstream change to
 * controller_ds5.c cannot collide with it, and removing it is deleting one
 * include and one line in the ops table.
 *
 * Included rather than compiled on its own because it IS this controller
 * type's on_input_report hook, and it reads state private to that file.
 */

/* Local gesture: hold TWO fingers on the touchpad AND press it down, for
 * CHORD_HOLD_MS, to unplug this controller without the overlay.
 *
 * Byte offsets measured from real hardware 2026-08-05 (DualSense and Edge, both
 * identical), wired input report 0x01:
 *   byte 10  bit 1     touchpad pressed
 *   byte 33  bit 7 clear   first touch point active
 *   byte 37  bit 7 clear   second touch point active
 * An inactive touch point reads 0x80; a finger clears the top bit and the low
 * bits become a touch counter, so only the top bit is meaningful here.
 *
 * The gesture is deliberately NOT swallowed -- the report is relayed unchanged.
 * Two fingers plus a press is not a combination games ask for, so the safer
 * choice is to stay a pure relay.
 *
 * Five seconds is deliberate. Unplugging is the destructive direction and the
 * fallback is trivial -- pull the cable -- so a long, obviously intentional
 * hold costs nothing and cannot happen by accident mid-game. It also matches
 * what a hold means elsewhere: powering a phone down, a PC's power button. */
#define DS5_CHORD_HOLD_MS 5000



/* Bluetooth carries the same fields one byte further along: report 0x31 adds a
 * sequence/flags byte ahead of the payload that wired report 0x01 does not
 * have.
 *
 * HYPOTHESIS, not yet measured. The wired offsets were read off hardware
 * (2026-08-06); these are those plus one. Measuring the Bluetooth ones the same
 * way was not possible: an unbridged Bluetooth DualSense sends a CUT-DOWN
 * 10-byte report with no touch data at all, and only switches to the full
 * report once a host asks for it -- which our own plug does. So the reading has
 * to happen from inside a bridged session, and the test is the measurement.
 *
 * Consequence worth knowing: the gesture can only ever UNBRIDGE over Bluetooth.
 * An unbridged Bluetooth controller is not sending fingers to detect. */
#define DS5_BT_REPORT_ID   0x31
#define DS5_BT_OFFSET      1

/* ⭐⭐ BLANK A DUALSENSE INPUT REPORT: nothing pressed, sticks centred, triggers
 * released. Used while the TV's overlay is open -- see ctm_input_set_held.
 *
 * ⛔ THE POINT IS THE HELD BUTTON. A host keeps the last state it was given, so
 * dropping reports would leave a button that was down when the overlay opened
 * down in the game, indefinitely. A blank report is what releases it, and
 * sending one per real report keeps the cadence normal so nothing upstream
 * decides the controller has gone away.
 *
 * ⓘ Offsets are the chord detector's, from the same measurement: wired report
 * 0x01 at offset 0, Bluetooth 0x31 one byte further along.
 *
 * ⚠️ THE TOUCHPAD IS DELIBERATELY LEFT ALONE. The unbridge chord is read from
 * the real report before this runs, but a game that reads touch data should see
 * fingers stop, not freeze -- and the touch bytes already read "no finger" as
 * 0x80, which is what they are set to here.
 *
 * ⓘ Everything not named is left untouched: the gyro and accelerometer, the
 * battery byte, the timestamp and the report's own counter all keep moving, so
 * the host sees a live controller doing nothing rather than a stalled one. */
static void ds5_blank_input(uint8_t *data, size_t len)
{
    size_t off;
    if (!data) return;
    if (data[0] == 0x01) {
        off = 0;
    } else if (data[0] == DS5_BT_REPORT_ID) {
        off = DS5_BT_OFFSET;
    } else {
        return;                       /* not a pad report -- audio, say */
    }
    if (len < 41 + off) return;

    data[1 + off] = 0x80;             /* left stick X, centred  */
    data[2 + off] = 0x80;             /* left stick Y           */
    data[3 + off] = 0x80;             /* right stick X          */
    data[4 + off] = 0x80;             /* right stick Y          */
    data[5 + off] = 0x00;             /* left trigger, released */
    data[6 + off] = 0x00;             /* right trigger          */

    /* Buttons and the d-pad share byte 8: the low nibble is the d-pad as a
     * direction, and 8 is its neutral value -- 0 would read as "up". */
    data[8 + off] = 0x08;
    data[9 + off] = 0x00;
    data[10 + off] = 0x00;            /* also clears the touchpad-pressed bit */

    data[33 + off] = 0x80;            /* first touch point inactive  */
    data[37 + off] = 0x80;            /* second touch point inactive */
}

static bool ds5_chord_held(const uint8_t *data, size_t len)
{
    size_t off;
    if (!data) {
        return false;
    }
    if (data[0] == 0x01) {
        off = 0;                      /* wired, measured */
    } else if (data[0] == DS5_BT_REPORT_ID) {
        off = DS5_BT_OFFSET;          /* bluetooth, inferred */
    } else {
        return false;
    }
    if (len < 41 + off) {
        return false;
    }
    bool pressed = (data[10 + off] & 0x02) != 0;
    bool finger1 = (data[33 + off] & 0x80) == 0;
    bool finger2 = (data[37 + off] & 0x80) == 0;
    return pressed && finger1 && finger2;
}

/* on_input_report: watch for the unplug gesture. When: this controller's input
 * thread, once per relayed report. The timestamp lives in the controller's own
 * type state, never in a file-level variable -- two controllers each have their
 * own thread calling this. */
/* ⭐ THE UNBRIDGE HALF OF THE GESTURE SETTING.
 *
 * ⓘ The BRIDGE chord is detected in the app, which can see an unbridged
 * controller's touchpad. Once bridged those reports come through here instead,
 * so the app is blind to them and this side owns the other half. Both read the
 * same user setting; the app hands it over when a bridge starts.
 *
 * ⓘ Defaults ON, so a core built without an app telling it anything behaves as
 * it always has. */
static int g_gesture_enabled = 1;

void ctm_gesture_set_enabled(int on)
{
    g_gesture_enabled = on ? 1 : 0;
}

static void ds5_on_input_report(ctm_controller_t *c, const uint8_t *data, size_t len)
{
    if (!c) return;
    if (!g_gesture_enabled) return;

    /* ⛔⛔ AN AUDIO REPORT IS ALSO 0x31, AND CARRIES NO TOUCHPAD.
     *
     * With microphone capture running, roughly a quarter of the reports carry
     * sound instead of pad state -- same id, same length, one flag apart. Read
     * as pad state, the audio bytes say "no fingers".
     *
     * ⛔ THE EFFECT IS THAT NO HOLD GESTURE CAN EVER COMPLETE. The chord is seen
     * on a pad report, lost on the next audio report, and re-armed:
     *
     *     chord held, timing a 5000ms hold
     *     chord released after 1ms, short of 5000ms
     *
     * repeating forever -- measured 2026-08-16, with releases of 1 to 5ms
     * against a 5000ms target. ⚠️ And it TRAPS the user: with capture on, the
     * only way out of a bridge is the overlay, which crashes on Circle.
     *
     * ⭐ SKIPPING IS CORRECT, NOT A WORKAROUND. An audio report is not "nothing
     * pressed", it is "nothing said about buttons" -- the last pad report still
     * stands. Returning early leaves the hold timer exactly as it was.
     *
     * ⚠️ IT MUST HAPPEN HERE, NOT INSIDE ds5_chord_held(). That returns a bool,
     * and false there means RELEASED -- which is the bug, not the fix.
     *
     * ⓘ Bit 1 says audio is present, bit 0 says pad state is. They are never
     * both set on this controller (measured across three captures), so an audio
     * report has nothing here worth reading. */
    if (len > 1 && data[0] == DS5_BT_REPORT_ID && (data[1] & 0x02u)) {
        return;
    }

    /* Logged on TRANSITIONS ONLY. This runs once per input report, roughly 250
     * times a second per controller, so a line per call would drown the file
     * and pay for a file open every time. */
    if (!ds5_chord_held(data, len)) {
        uint64_t was = ctm_controller_type_state(c, DS5_SLOT_CHORD);
        if (was != 0 && was != UINT64_MAX) {
            ctm_gesture_log(c, "chord released after %llums, short of %dms",
                            (unsigned long long)(ds5_now_ms() - was),
                            DS5_CHORD_HOLD_MS);
        }
        /* Released: re-arm. */
        ctm_controller_set_type_state(c, DS5_SLOT_CHORD, 0);
        return;
    }

    uint64_t since = ctm_controller_type_state(c, DS5_SLOT_CHORD);
    uint64_t now = ds5_now_ms();

    if (since == 0) {
        ctm_gesture_log(c, "chord held, timing a %dms hold", DS5_CHORD_HOLD_MS);
        ctm_controller_set_type_state(c, DS5_SLOT_CHORD, now);
        return;
    }
    /* Already fired for this hold: wait for the fingers to lift. */
    if (since == UINT64_MAX) {
        return;
    }
    if (now - since >= DS5_CHORD_HOLD_MS) {
        ctm_gesture_log(c, "chord complete after %llums, asking to unplug",
                        (unsigned long long)(now - since));
        ctm_controller_set_type_state(c, DS5_SLOT_CHORD, UINT64_MAX);
        ctm_controller_request_unplug(c);
    }
}
