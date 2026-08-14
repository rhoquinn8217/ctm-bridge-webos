/* --- microphone safety -------------------------------------------------------
 *
 * FORK-ONLY. Two call sites: one at startup, one on the input relay.
 *
 * ⛔⛔ WHAT THIS PROTECTS AGAINST, and why it is not paranoia.
 *
 * A Bluetooth DualSense can be told to stream microphone audio. When it is, it
 * sends AUDIO-ONLY reports -- same report id, same length as pad state, with
 * one flag bit to tell them apart. Nothing reads that bit. Not SDL, not the
 * Linux kernel, not this app before today.
 *
 * So every reader parses encoded sound as sticks and buttons. Measured
 * 2026-08-13: the mouse crossed a desktop continuously, and on this TV the app
 * crashed and then had its menus activated at random until the controller was
 * powered off. ⭐ That is upstream's stated reason for never implementing
 * microphone support, reproduced on demand.
 *
 * ⚠️ AND WE ARE NOT IN SDL'S PATH. SDL opens the controller itself and gets its
 * own copy of every report. We cannot filter what it reads. The only thing we
 * can do is make sure the controller is not streaming in the first place.
 *
 * ⛔⛔⛔ THERE IS DELIBERATELY NO CODE HERE THAT TURNS THE MICROPHONE ON.
 *
 * Not commented out -- ABSENT. Commented-out code gets uncommented; a
 * description has to be written from scratch, which is the friction we want.
 *
 * For the record, so nobody has to rediscover it: arming is a single bit in
 * the first payload byte of block 0x91, sent in a small output report. The
 * receiving side and the frame layout are in bt-microphone-findings.md.
 *
 * ⚠️ IF THAT IS EVER BUILT, it must be off by default, never persisted across
 * sessions, and gated behind a warning that says plainly what happens -- see
 * T-105. The reason it is not built is not that it does not work. It works.
 * It is that nothing else on the system knows how to ignore it. */

/* ⚠️ WRITES TO A FILE, not just stderr.
 *
 * The first version logged with fprintf(stderr) alone, and the app's stderr is
 * not where the logs we read end up -- so a test of the startup disarm and the
 * detector produced no evidence either way, and an hour went into guessing
 * which of them had failed. The same mistake had already been made once on the
 * wired signal path. */
static void micsafe_log(const char *fmt, ...)
{
    char body[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    fprintf(stderr, "[mic-safety] %s\n", body);

    FILE *f = fopen("/tmp/ctm-mic-safety.log", "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "%lld.%03ld [mic-safety] %s\n",
            (long long)ts.tv_sec, ts.tv_nsec / 1000000L, body);
    fclose(f);
}

/* ⛔⛔⛔ EXPERIMENTAL BRANCH ONLY -- mic-capture-experimental.
 *
 * THIS CODE DOES NOT EXIST ON THE STABLE BRANCH AND MUST NOT BE MERGED THERE.
 *
 * It arms the controller's microphone, and it switches OFF the guard that
 * would otherwise shut the app down when a controller starts streaming. Both
 * of those are deliberate here and unacceptable anywhere else.
 *
 * WHAT HAPPENS WITH THIS ON. A Bluetooth DualSense sends audio in reports that
 * look exactly like button presses to everything that reads them -- SDL
 * included, which we cannot filter. Measured on real hardware: a mouse
 * crossing a desktop continuously, and an app whose menus activated
 * themselves until the controller was powered off.
 *
 * ⚠️ IF THAT HAPPENS: power the controller off. Hold PS until the light goes
 * out. The microphone state does not survive a Bluetooth link drop, so turning
 * it off and on again always silences it -- confirmed by measurement. Nothing
 * on either machine has to be working for that to succeed.
 *
 * MICSAFE_EXPERIMENTAL_ARMING must be set to 1 by hand, every time, on
 * purpose. It is 0 here and stays 0 in the repository. */
#define MICSAFE_EXPERIMENTAL_ARMING 0

#define MICSAFE_BT_REPORT_ID   0x31
#define MICSAFE_FLAG_HID_DATA  0x01
#define MICSAFE_FLAG_MIC_AUDIO 0x02

/* The disarm packet: a small output report carrying one block, one byte.
 *
 * Bit 1 alone silences the microphone; bit 0 alongside it would arm it. Plus
 * the CRC32 every Bluetooth output report carries, computed over an 0xa2
 * prefix and everything before the checksum. */
#define MICSAFE_PKT_LEN   142
#define MICSAFE_CRC_AT    138

static uint32_t micsafe_crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int built = 0;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = 1;
    }
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

/* Tell one controller to stop streaming microphone audio.
 *
 * Takes a device node rather than anything session-shaped: this runs before
 * SDL exists at startup, and from the input path when a session may be in an
 * unknown state. */
static int micsafe_disarm_node(const char *node)
{
    if (!node || !node[0]) return -1;
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;

    uint8_t pkt[MICSAFE_PKT_LEN];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x32;                  /* the small output report */
    pkt[1] = 0x00;                  /* sequence; a fresh one is fine here */
    pkt[2] = 0x91;                  /* the block carrying the switch */
    pkt[3] = 1;                     /* one byte of payload */
    pkt[4] = MICSAFE_FLAG_MIC_AUDIO;/* bit 1 only -- microphone OFF */

    uint8_t seeded[1 + MICSAFE_CRC_AT];
    seeded[0] = 0xa2;
    memcpy(&seeded[1], pkt, MICSAFE_CRC_AT);
    uint32_t crc = micsafe_crc32(seeded, sizeof(seeded));
    pkt[MICSAFE_CRC_AT + 0] = (uint8_t)(crc & 0xff);
    pkt[MICSAFE_CRC_AT + 1] = (uint8_t)((crc >> 8) & 0xff);
    pkt[MICSAFE_CRC_AT + 2] = (uint8_t)((crc >> 16) & 0xff);
    pkt[MICSAFE_CRC_AT + 3] = (uint8_t)((crc >> 24) & 0xff);

    ssize_t rc = write(fd, pkt, sizeof(pkt));
    close(fd);
    return (rc == (ssize_t)sizeof(pkt)) ? 0 : -1;
}

#if MICSAFE_EXPERIMENTAL_ARMING
/* ⛔ ARM one controller's microphone. EXPERIMENTAL BRANCH ONLY.
 *
 * The mirror of the disarm above: the same small output report and the same
 * block, with bit 0 set alongside bit 1 instead of bit 1 alone.
 *
 * ⚠️ It is a state change, not a stream -- sent once, and the controller
 * remembers until its Bluetooth link drops. */
static int micsafe_arm_node(const char *node)
{
    if (!node || !node[0]) return -1;
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;

    uint8_t pkt[MICSAFE_PKT_LEN];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x32;
    pkt[1] = 0x00;
    pkt[2] = 0x91;
    pkt[3] = 1;
    pkt[4] = MICSAFE_FLAG_HID_DATA | MICSAFE_FLAG_MIC_AUDIO;   /* ON */

    uint8_t seeded[1 + MICSAFE_CRC_AT];
    seeded[0] = 0xa2;
    memcpy(&seeded[1], pkt, MICSAFE_CRC_AT);
    uint32_t crc = micsafe_crc32(seeded, sizeof(seeded));
    pkt[MICSAFE_CRC_AT + 0] = (uint8_t)(crc & 0xff);
    pkt[MICSAFE_CRC_AT + 1] = (uint8_t)((crc >> 8) & 0xff);
    pkt[MICSAFE_CRC_AT + 2] = (uint8_t)((crc >> 16) & 0xff);
    pkt[MICSAFE_CRC_AT + 3] = (uint8_t)((crc >> 24) & 0xff);

    ssize_t rc = write(fd, pkt, sizeof(pkt));
    close(fd);
    micsafe_log("⛔ EXPERIMENTAL: armed the microphone on %s (rc=%d)",
                node, (int)rc);
    return (rc == (ssize_t)sizeof(pkt)) ? 0 : -1;
}
#endif /* MICSAFE_EXPERIMENTAL_ARMING */

/* ⭐⭐ SILENCE EVERY CONTROLLER BEFORE SDL OPENS ANY OF THEM.
 *
 * This is the single most valuable thing in this file, because it closes the
 * hole that actually bit us: the app crashed while a controller was streaming,
 * and came back to find SDL reading it. A controller does not forget it is
 * streaming when a program dies -- only when its Bluetooth link drops.
 *
 * ⚠️ MUST RUN BEFORE SDL_Init's controller subsystem. Called from one place at
 * startup; moving that call later would quietly remove the protection.
 *
 * Cheap and blind by design: it writes to every hidraw node it can open. A
 * node that is not a DualSense ignores a report it does not recognise, and
 * asking politely first would mean opening devices to interrogate them, which
 * is more intrusive than the write itself. */
void ctm_mic_safety_disarm_all_reason(const char *why)
{
    int silenced = 0;
    for (int i = 0; i < 16; ++i) {
        char node[32];
        snprintf(node, sizeof(node), "/dev/hidraw%d", i);
        if (micsafe_disarm_node(node) == 0) ++silenced;
    }
    micsafe_log("sent microphone-off to %d device(s) -- %s", silenced, why);
}

/* The startup call. Named separately so the log says which of the two
 * situations it was: the first version said "before input started" from both,
 * which reads as a lie in the one place someone is looking carefully. */
void ctm_mic_safety_disarm_all(void)
{
    ctm_mic_safety_disarm_all_reason("before input started");
}

/* ⛔⛔ A CONTROLLER STARTED STREAMING WHILE WE WERE RUNNING. STOP EVERYTHING.
 *
 * Called from the input relay on every report. Returns true if the report
 * carries microphone audio, which means something has armed a controller
 * behind our back -- nothing in this app can.
 *
 * ⭐ THIS IS EXACT, NOT A HEURISTIC. We are not looking at the sticks and
 * judging them erratic; we are reading the flag that says what the report is.
 * One report is proof.
 *
 * WHAT IT DOES, AND THE ORDER MATTERS:
 *
 *   1. Disarm, repeatedly, until the audio stops. Measured 2026-08-13: about
 *      TWO further reports get through, roughly ten milliseconds. That is the
 *      whole exposure.
 *
 *   2. ⚠️ ONLY THEN exit. Quitting first would be worse than useless -- it
 *      hands a still-streaming controller to the TV's own home screen, which
 *      reads controllers too, with nothing of ours left running to fix it.
 *
 * ⚠️ Exiting is deliberate and drastic. SDL has its own copy of every report
 * and we cannot filter it, so the only way to stop this app misreading audio
 * as input is to stop this app. A crash the user understands beats menus
 * activating themselves.
 *
 * ⓘ If a restart loop ever appears, the cause is a controller that stayed
 * armed and a disarm that never landed -- the startup disarm should prevent
 * that, and the log line below is how it would be spotted. */
static bool micsafe_check_report(const char *node, const uint8_t *buf, size_t n)
{
    if (!buf || n < 2) return false;
    /* Prove the check is reachable. Without this, "nothing happened" cannot be
     * told apart from "this code never ran" -- which is exactly what happened
     * the first time it was tested. */
    static bool announced = false;
    if (!announced) {
        announced = true;
        micsafe_log("watching %s for microphone reports",
                    node ? node : "a controller");
    }

    if (buf[0] != MICSAFE_BT_REPORT_ID) return false;
    if (!(buf[1] & MICSAFE_FLAG_MIC_AUDIO)) return false;

#if MICSAFE_EXPERIMENTAL_ARMING
    /* ⛔ THE SAFETY EXIT IS OFF ON THIS BRANCH.
     *
     * We armed it on purpose, so shutting down on the first report would make
     * the experiment impossible. ⚠️ That means the ONLY thing standing between
     * a bug here and an unusable machine is powering the controller off. */
    {
        static bool warned = false;
        if (!warned) {
            warned = true;
            micsafe_log("⛔ EXPERIMENTAL: audio reports arriving and the safety "
                        "exit is DISABLED -- power the controller off if the "
                        "app or the desktop starts misbehaving");
        }
    }
    return false;
#endif

    micsafe_log("⛔ %s is streaming microphone audio and nothing here asked "
                "it to -- disarming and shutting down",
                node ? node : "a controller");

    /* Send it more than once: a single write can be lost, and the cost of an
     * extra is nothing next to the cost of it not landing. */
    for (int attempt = 0; attempt < 5; ++attempt) {
        micsafe_disarm_node(node);
        struct timespec ts = {0, 20 * 1000000};   /* 20 ms */
        nanosleep(&ts, NULL);
    }

    /* And silence anything else that might be in the same state, since we are
     * about to stop being able to do anything about it. */
    ctm_mic_safety_disarm_all_reason("a controller was found streaming");

    micsafe_log("disarmed; exiting so nothing here reads audio as button "
                "presses");
    return true;
}
