/* Controller pump + lifecycle + factory (D2 stage 2).
 *
 * One isolated bridging session per controller: its own reader + session
 * threads, HID fd, ctm_transport, settings, and per-MAC log. This is the merge
 * of tv_bridge_worker.c (DS pump: 2 threads, pacing, evdev grab, HELLO/
 * HOST_CONFIG handshake) and hidraw_bridge.c (verbatim relay). Per-type
 * behaviour is preserved via ops flags (needs_host_config / grab_evdev /
 * request_bt_mode) and the patch_output hook. */

#define _GNU_SOURCE

#include "ctm_controller.h"
#include "ctm_hid.h"   /* read_report_descriptor, derive_report_lengths */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sound/asound.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/hidraw.h>
#include <linux/input.h>
#endif

#ifndef HIDIOCGRAWINFO
struct hidraw_devinfo { unsigned int bustype; short vendor; short product; };
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)
#endif
#ifndef HIDIOCGRAWNAME
#define HIDIOCGRAWNAME(len) _IOC(_IOC_READ, 'H', 0x04, len)
#endif
#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x07, len)
#endif
#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x06, len)
#endif
#ifndef EVIOCGRAB
#define EVIOCGRAB _IOW('E', 0x90, int)
#endif

/* ------------------------------------------------------------------
 * DS5 output report layout
 *
 * The controller's output report is a fixed 48-byte block: a report ID,
 * two "which settings am I claiming?" flag bytes, then the settings
 * themselves at fixed positions. A setting is only honoured if its
 * claim bit is set -- but note that a CLAIMED setting left at zero is
 * applied AS zero, not left alone. Claiming something you don't intend
 * to set is therefore an active change, not a no-op. This is why the
 * init deliberately claims as little as possible.
 * ------------------------------------------------------------------ */
#define DS5_OUT_REPORT_ID              0x02
#define DS5_OUT_REPORT_LEN             48    /* matches every host report on the wire */

/* Positions within the report */
#define DS5_IDX_VALID_FLAG0            1
#define DS5_IDX_VALID_FLAG1            2
#define DS5_IDX_SPEAKER_VOLUME         6
#define DS5_IDX_AUDIO_CONTROL          8

/* valid_flag0 -- claim bits for the settings we care about */
#define DS5_F0_ALLOW_SPEAKER_VOLUME    0x20
#define DS5_F0_ALLOW_AUDIO_CONTROL     0x80

/* audio_control (byte 8) fields */
#define DS5_AUDIO_OUT_PATH_SPEAKER     0x30  /* route playback to the controller speaker */
#define DS5_AUDIO_ECHO_NOISE_CANCEL    0x0c  /* echo + noise cancellation ON.
                                              * The controller suppresses its own speaker
                                              * when this is off -- feedback protection,
                                              * since the mic sits centimetres away. This
                                              * is THE attenuation lever: measured on C1,
                                              * attenuated -> 80 dB -> 94 dB with this bit
                                              * alone, volume held constant. */
#define DS5_SPEAKER_VOLUME_MAX         0x64  /* 100 -- what games, the kernel driver and
                                              * dualsensectl all use */

#define MAX_REPORT 4096
#define MAX_REPORT_DESCRIPTOR 4096
#define PACED_QUEUE_CAP 32
#define MAX_EVDEV_GRABS 16
#define BUS_BLUETOOTH 0x05

typedef struct { uint8_t data[MAX_REPORT]; size_t len; } queued_report_t;
typedef struct { int fd; char path[64]; } evdev_grab_t;

/* Composite (puck): one forwarded sibling HID interface. The primary is c->hid_fd;
 * comp[] are the rest. Each input report is tagged with the interface's IN endpoint. */
typedef struct {
    ctm_controller_t *c;
    int fd;
    uint8_t in_ep;
    uint8_t out_ep;
    uint8_t iface;
    pthread_t thread;
    int started;
} comp_iface_t;

struct ctm_controller {
    const ctm_controller_ops_t *ops;
    ctm_controller_dev_t dev;
    unsigned int vid_num;
    unsigned int pid_num;
    char host[64];
    int port;

    ctm_transport_t xport;
    ctm_enet_client_t *enet;        /* process-owned client; borrowed by xport */
    int hid_fd;
    int alsa_fd;   /* wired DS5/Edge: the controller's own playback device */
    int alsa_consec_fail;      /* audio writes that failed outright, in a row */
    int alsa_retry_streak;     /* audio writes that needed any retry, in a row */
    unsigned long long alsa_writes;   /* audio chunks written, for the log */
    /* The audio settings currently in force on this controller. Start as the
     * defaults and are updated by any host report that claims them, so a
     * reopen restores what the host asked for rather than what we assumed. */
    uint8_t audio_spk_vol;
    uint8_t audio_control;
    pthread_t mic_cap_thread;
    int mic_cap_started;
    int wake_pipe[2];

    pthread_t session_thread;
    int session_started;
    pthread_t input_thread;
    int input_thread_started;
    volatile int stop;

    /* Set by a type's on_input_report to ask the app to unplug this
     * controller. Read and cleared by the app, never by the input thread. */
    volatile int unplug_requested;
    /* Scratch values per controller, owned by its type. */
    uint64_t type_state[CTM_TYPE_STATE_SLOTS];

    pthread_mutex_t hid_mutex;
    pthread_mutex_t settings_mutex;
    tv_bridge_worker_settings_t settings;

    evdev_grab_t evdev_grabs[MAX_EVDEV_GRABS];
    int evdev_grab_count;

    FILE *log;

    /* Live status for the UI panel. Counters are single-writer (reports_in:
     * reader thread; reports_out: session thread) and read advisorily;
     * connected/transport/last_event are guarded by status_mutex. */
    pthread_mutex_t status_mutex;
    volatile int st_connected;
    volatile int st_transport_enet;
    volatile unsigned long st_reports_in;
    volatile unsigned long st_reports_out;
    char st_last_event[96];

    uint8_t *enum_payload;           /* composite: forwarded enumeration (CTMB_MSG_ENUM) */
    int enum_payload_len;
    comp_iface_t comp[15];           /* composite: the non-primary HID interfaces */
    int comp_count;
    uint8_t primary_in_ep;           /* the primary hidraw's IN endpoint (input tag) */
    uint8_t primary_out_ep;          /* the primary hidraw's OUT endpoint (output route) */
    uint8_t primary_iface;           /* the primary hidraw's USB interface number */
    volatile int comp_run;           /* gates the sibling reader threads */
};

/* ENet is process-global; init once for all controllers, never deinit. */
static pthread_once_t g_enet_once = PTHREAD_ONCE_INIT;
static int g_enet_ready = 0;
static void enet_global_init_once(void)
{
    if (enet_client_global_init() == 0) g_enet_ready = 1;
    else fprintf(stderr, "controller: enet_initialize failed; ENet disabled, TCP only\n");
}

/* Audio has its own log file, and every line in it carries a wall-clock time.
 *
 * Both halves of that are deliberate. Its own file, because the app's log is
 * not readable on this platform -- no journal, no /var/log -- so anything that
 * needs diagnosing has to write somewhere reachable. And a time on every line,
 * because a reopen is the ONLY event that re-sends the settings report, which
 * makes it the single most important thing to be able to place in a session.
 * Without one, "did the audio hold across a reopen?" cannot be answered even
 * with the evidence in hand -- which is exactly what happened once, with four
 * reopens logged and no way to tell whether they fell inside the window being
 * measured.
 *
 * Seconds since the epoch to three decimals, matching the other logs, so they
 * can be read side by side with no conversion. */
static void alsa_log(const char *tag, const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    FILE *f = fopen("/tmp/alsa_debug.log", "a");
    if (!f) return;
    fprintf(f, "%s %.3f ", tag, (double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Open the DualSense's own USB audio playback device for wired ISO passthrough.
 * Scans ALSA cards for one named "DualSense", opens pcmC{n}D0p, configures
 * S16_LE 4ch 48kHz. Returns fd on success, -1 on failure.
 *
 * Ported from ds5-aurora, which is the reference implementation and was proven
 * on hardware. Two deliberate differences, both noted rather than silent:
 *
 *   - The root-broker fallback is NOT carried across. The scaffold falls back
 *     to a helper when the open is refused; the app is in group `audio` (29)
 *     and opens the device directly on every TV tested. Add it back if a set
 *     refuses -- do not add it speculatively.
 *   - The channel layout is not ours to choose: 0/1 are the speaker, 2/3 the
 *     haptics, confirmed via ALSA's own channel map. THIS IS WHY SPEAKER AND
 *     RUMBLE ARE ONE FEATURE -- writing this device drives both.
 *
 * The card index is scanned every time and never cached: it drifts per plug-in,
 * and a cached index reopens the wrong node after a re-enumeration.
 *
 * WITH TWO CONTROLLERS THIS CAN OPEN THE WRONG ONE. Both match the name
 * "DualSense" and the first is taken. The capture path carries the same flaw
 * and a measurement proving it -- on 2026-08-04 two capture threads reported
 * identical levels while one controller was muted AT THE HARDWARE, which a
 * muted microphone cannot produce.
 *
 * The way out is known and is NOT a guess: /proc/asound/cardN/usbbus names the
 * USB bus and device the card belongs to. Read with root on 2026-08-04 it gave
 * 001/004 and 001/003 for the two controllers -- genuinely different devices,
 * which is what eliminated every external explanation and forced the code
 * re-read that found the real bug. The bridge already knows each controller's
 * USB path, so this is joining two things we both have rather than parsing
 * something unknown.
 *
 * Not done here, deliberately: it is untested with two controllers on this
 * base, and the audio path is worth getting working for one before it is made
 * clever for two. */
static int open_ds5_alsa_playback(const char *want_node)
{
    char path[128];
    int fd = -1;
    for (int card = 0; card < 8; card++) {
        snprintf(path, sizeof(path), "/proc/asound/card%d/stream0", card);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[256];
        char header[256];
        int found = 0;
        header[0] = '\0';
        while (fgets(line, sizeof(line), f)) {
            /* The first line names the device AND where it is attached, e.g.
             * "...DualSense Wireless Controller at usb-generic-ehci-1.4...".
             * That tail is the only thing here that distinguishes one
             * controller's card from the other's -- the NAME is identical for
             * both. */
            if (header[0] == '\0') {
                snprintf(header, sizeof(header), "%s", line);
                char *nl = strchr(header, '\n');
                if (nl) *nl = '\0';
            }
            if (strstr(line, "DualSense")) { found = 1; break; }
        }
        fclose(f);
        if (!found) continue;

        /* MEASUREMENT, NOT A FIX -- and the same line the capture scan already
         * writes. Both controllers' scans match on the name "DualSense", so
         * with two attached there is nothing here tying a card to the
         * controller asking for it, and audio has been heard coming from a
         * controller that was not even bridged.
         *
         * Every occurrence so far has been unattributable, because this scan
         * said which card it opened and never which controller asked. It has
         * now been observed twice and reproduced neither time; this line is
         * what makes the next occurrence readable rather than another parked
         * report. Matching on the header is deliberately NOT done yet. */
        alsa_log("[alsa-scan]", "card=%d wants_node=%s header=[%s]",
                 card, want_node ? want_node : "?", header);

        snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0p", card);
        fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd < 0) continue;
        struct snd_pcm_hw_params hw;
        memset(&hw, 0, sizeof(hw));
        /* Init all intervals to full range ("any"), then constrain */
        for (int i = 0; i < (int)(sizeof(hw.intervals)/sizeof(hw.intervals[0])); i++) {
            hw.intervals[i].min = 0;
            hw.intervals[i].max = 0xFFFFFFFFU;
        }
        /* Init all masks to full range */
        for (int i = 0; i < (int)(sizeof(hw.masks)/sizeof(hw.masks[0])); i++)
            memset(&hw.masks[i], 0xFF, sizeof(hw.masks[i]));
        /* ACCESS: RW_INTERLEAVED(3) only */
        memset(&hw.masks[0], 0, sizeof(hw.masks[0]));
        hw.masks[0].bits[3 / 32] = 1u << (3 % 32);
        /* FORMAT: S16_LE(2) only */
        memset(&hw.masks[1], 0, sizeof(hw.masks[1]));
        hw.masks[1].bits[2 / 32] = 1u << (2 % 32);
        /* CHANNELS: exactly 4 */
        hw.intervals[2].min = hw.intervals[2].max = 4;
        hw.intervals[2].integer = 1;
        /* RATE: exactly 48000 */
        hw.intervals[3].min = hw.intervals[3].max = 48000;
        hw.intervals[3].integer = 1;
        /* PERIOD_SIZE: let the kernel choose what suits the hardware.
         * A hardcoded 48-frame period once mismatched the host's 480-frame
         * chunks and contributed to underruns. */
        /* BUFFER_SIZE: at least 960 frames (~20ms) to bridge gaps between the
         * host's chunks. */
        hw.intervals[9].min = 960;
        hw.intervals[9].max = 0xFFFFFFFFU;
        hw.intervals[9].integer = 0;
        int hw_rc = ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw);
        int prep_rc = (hw_rc >= 0) ? ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL) : -1;
        if (hw_rc < 0 || prep_rc < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        /* What the kernel actually chose. Restored to match the reference:
         * on a first test this is the only record of the period and buffer
         * size the hardware settled on. */
        alsa_log("[alsa-hwparams]", "card=%d period_frames=%u buffer_frames=%u",
                 card, (unsigned)hw.intervals[5].max, (unsigned)hw.intervals[9].max);
        return fd;
    }
    return -1;
}

/* Four channels of 16-bit audio, which is what the device was configured for
 * at open. Frames are what it counts in, so an arriving chunk is divided by
 * this to get them. */
#define DS5_AUDIO_CHANNELS 4

/* Write one chunk of arriving PCM to the controller's playback device.
 *
 * A write can fail two recoverable ways. The device can run dry between chunks
 * (EPIPE), and will take nothing more until it is prepared again; or its buffer
 * can be full (EAGAIN), which just needs a moment. Anything else is not worth
 * retrying. Three attempts, then the chunk is dropped -- audio is droppable by
 * nature, and a dropped chunk is a brief gap, never a reason to stop.
 *
 * Two counters watch for the device decaying, and BOTH LIVE ON THE CONTROLLER
 * rather than in this function. In the implementation this is ported from they
 * are function-level statics, which means two controllers share one pair: one
 * controller's failures would reopen the other's audio, and the one actually
 * failing would never heal. That is the same fault that took four sessions to
 * find in the microphone path.
 *
 *   - Failures in a row (20): the device is genuinely stuck.
 *   - Writes needing any retry, in a row (5): the device is decaying. This one
 *     fires before anything is audible, which is the point of having it.
 *
 * Either one reopens the device. Reopening also re-sends the settings report,
 * and that is what actually restores the sound.
 *
 * Known limitation, carried over deliberately: if the reopen itself fails, the
 * handle stays closed and nothing tries again -- later chunks are dropped
 * silently. The reference behaves the same way, and matching it matters more
 * here than improving on it.
 *
 * When: once per arriving audio message, on the session thread. */
static void write_iso_audio(ctm_controller_t *c, const uint8_t *pcm, uint32_t len)
{
    if (!c || c->alsa_fd < 0 || !pcm || len == 0) return;

    struct snd_xferi xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.buf = (void *)pcm;
    xfer.frames = len / (DS5_AUDIO_CHANNELS * sizeof(int16_t));
    if (xfer.frames == 0) return;

    int rc = ioctl(c->alsa_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
    int first_errno = (rc < 0) ? errno : 0;
    int retries = 0;
    while (rc < 0 && retries < 3) {
        if (errno == EPIPE) {
            ioctl(c->alsa_fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
        } else if (errno == EAGAIN) {
            struct timespec ts = {0, 1000000};   /* 1 ms for buffer space */
            nanosleep(&ts, NULL);
        } else {
            break;                               /* not recoverable */
        }
        xfer.result = 0;
        rc = ioctl(c->alsa_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
        retries++;
    }

    if (rc < 0) c->alsa_consec_fail++; else c->alsa_consec_fail = 0;
    if (first_errno != 0) c->alsa_retry_streak++; else c->alsa_retry_streak = 0;

    /* This line is where the fault counts come from -- there is no separate
     * one -- so it is written on anything that went wrong, and periodically
     * when nothing did, to prove the path is still alive. */
    c->alsa_writes++;
    if (first_errno != 0 || rc < 0 || (c->alsa_writes % 500) == 0) {
        alsa_log("[alsa-write]", "n=%llu frames=%lu final_rc=%d first_errno=%d retries=%d",
                 (unsigned long long)c->alsa_writes, (unsigned long)xfer.frames,
                 rc, first_errno, retries);
    }

    const char *heal_reason = NULL;
    if (c->alsa_consec_fail >= 20) heal_reason = "consecutive-fail";
    else if (c->alsa_retry_streak >= 5) heal_reason = "retry-streak";
    if (!heal_reason) return;

    alsa_log("[alsa-selfheal]", "reason=%s consec_fail=%d retry_streak=%d, reopening ALSA device",
             heal_reason, c->alsa_consec_fail, c->alsa_retry_streak);
    close(c->alsa_fd);
    c->alsa_fd = -1;
    ctm_controller_open_alsa_playback(c);
    if (c->alsa_fd >= 0) {
        ctl_log(c, "alsa: self-healed - reopened playback (fd=%d)", c->alsa_fd);
    } else {
        ctl_log(c, "alsa: self-heal reopen failed");
    }
    c->alsa_consec_fail = 0;
    c->alsa_retry_streak = 0;
}

/* Monotonic clock in microseconds. When: pacing schedules + handshake timeout. */
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Optional UI log sink, set by the app, so controller events also appear in the
 * on-screen console. NULL => file + stderr only. */
static void (*g_log_sink)(const char *line);

/* Told when a controller asks to be unplugged; the app does the work. */
static ctm_controller_unplug_cb g_unplug_cb;

void ctm_controller_set_unplug_cb(ctm_controller_unplug_cb cb)
{
    g_unplug_cb = cb;
}

/* Write a line to the gesture log -- the same file the app's plug-in watcher
 * uses, so both halves of a gesture read in order, in one place.
 *
 * The unplug half wrote nothing at all until 2026-08-07. A gesture that did
 * not unplug left no evidence of whether it was seen, whether it fired, or
 * whether the teardown it asked for ever finished, and twice in one session
 * that had to be reasoned about from silence.
 *
 * `c` may be NULL, for callers holding a key rather than a controller. */
void ctm_gesture_log(const ctm_controller_t *c, const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    FILE *f = fopen("/tmp/ctm-gesture.log", "a");
    if (!f) return;
    fprintf(f, "%.3f [%s] ", (double)ts.tv_sec + (double)ts.tv_nsec / 1e9,
            (c && c->ops && c->ops->kind) ? c->ops->kind : "app");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void ctm_controller_request_unplug(ctm_controller_t *c)
{
    if (!c) return;
    if (c->unplug_requested) {
        /* The flag is deliberately never cleared, so this is reached whenever
         * a controller asks twice -- which also means it is STILL ASKING on
         * every later pass of the worker. Worth being able to see. */
        ctm_gesture_log(c, "unplug already requested for %s, ignoring", c->dev.mac);
        return;
    }
    c->unplug_requested = 1;
    ctm_gesture_log(c, "unplug requested for %s (handler %s)",
                    c->dev.mac, g_unplug_cb ? "set" : "MISSING");
    if (g_unplug_cb) g_unplug_cb(c);
}

bool ctm_controller_unplug_requested(const ctm_controller_t *c)
{
    return c && c->unplug_requested != 0;
}

const char *ctm_controller_bus(const ctm_controller_t *c)
{
    return c ? c->dev.bus : "";
}

uint64_t ctm_controller_type_state(const ctm_controller_t *c, int slot)
{
    if (!c || slot < 0 || slot >= CTM_TYPE_STATE_SLOTS) return 0;
    return c->type_state[slot];
}

void ctm_controller_set_type_state(ctm_controller_t *c, int slot, uint64_t v)
{
    if (!c || slot < 0 || slot >= CTM_TYPE_STATE_SLOTS) return;
    c->type_state[slot] = v;
}

void ctm_controller_set_log_sink(void (*sink)(const char *line))
{
    g_log_sink = sink;
}

/* Per-controller log: writes to its MAC-named file (if open), stderr, and the
 * UI sink (if set), each line prefixed with the controller kind. When:
 * throughout a controller's lifetime (any of its threads). */
void ctl_log(ctm_controller_t *c, const char *fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    const char *kind = c->ops ? c->ops->kind : "ctl";
    fprintf(stderr, "[%s] %s\n", kind, body);
    if (c->log) {
        fprintf(c->log, "%s\n", body);
        fflush(c->log);
    }
    pthread_mutex_lock(&c->status_mutex);
    snprintf(c->st_last_event, sizeof(c->st_last_event), "%s", body);
    pthread_mutex_unlock(&c->status_mutex);
    if (g_log_sink) {
        char line[600];
        snprintf(line, sizeof(line), "%s: %s", kind, body);
        g_log_sink(line);
    }
}

/* Send one framed CTMB message over this controller's transport. When: the
 * reader thread (input reports), the session thread (HELLO + feature replies). */
static int c_send(ctm_controller_t *c, uint16_t type, uint32_t flags,
                  uint32_t request_id, const void *payload, size_t len)
{
    return ctm_transport_send_msg(&c->xport, type, flags, request_id, payload, len);
}


/* --- microphone capture ----------------------------------------------------
 *
 * Reads the controller's microphone and sends it to the host as
 * CTMB_MSG_MIC_AUDIO. Proven end to end for two independent controllers.
 *
 * WHAT IS KNOWN, MEASURED ON AN UNROOTED C1: the capture side reports S16_LE,
 * 2 channels, 48000 Hz. The two channels are a MICROPHONE ARRAY -- both carry
 * the voice; there is no dead channel to drop. Plugging a headset in switches
 * BOTH channels to the headset mic.
 *
 * GATED. Nothing happens unless /tmp/mic_capture_on exists, checked when a
 * capture thread starts.
 *   Arm:    touch /tmp/mic_capture_on
 *   Disarm: rm /tmp/mic_capture_on
 * A /tmp sentinel does not survive a TV reboot -- deliberate for a diagnostic,
 * and not acceptable for the finished feature.
 *
 * MIRRORS open_ds5_alsa_playback() above: same card scan, same hw_params
 * shape. The differences are the capture device suffix ('c' not 'p'),
 * O_RDONLY, and 2 channels not 4.
 *
 * Placed after c_send() because the reader sends from this thread. */

#define MIC_CAP_CHANNELS   2
#define MIC_CAP_RATE       48000
#define MIC_CAP_FRAMES     480          /* 10 ms, matching the host's request size */
#define MIC_CAPTURE_ON_PATH "/tmp/mic_capture_on"

static int mic_capture_armed(void)
{
    return access(MIC_CAPTURE_ON_PATH, F_OK) == 0;
}

static void mic_cap_log(const char *fmt, ...)
{
    FILE *f = fopen("/tmp/mic_capture.log", "a");
    if (!f) return;
    /* Wall-clock, the same shape as the Windows session log (20:43:01.590) so
     * the three logs -- this one, the controller log, and the host's -- can be
     * lined up by eye. The epoch seconds this replaced were correct but
     * unreadable, which pushed every investigation into counting lines and
     * taking markers before each step. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    fprintf(f, "%02d:%02d:%02d.%03d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
            (int)(ts.tv_nsec / 1000000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Open the DualSense capture device. Returns fd, or -1.
 *
 * Reports which card it opened through out_card, because every log line the
 * thread writes afterwards needs it. Two controllers share ONE log file, and
 * without the card on each line the only way to tell the two streams apart is
 * to compare counter values and infer which thread started first -- which
 * works until both have been running a while, and then does not. */
static int open_ds5_alsa_capture(int *out_card, const char *want_node)
{
    char path[128];
    int fd = -1;
    for (int card = 0; card < 8; card++) {
        snprintf(path, sizeof(path), "/proc/asound/card%d/stream0", card);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[256];
        char header[256];
        int found = 0;
        header[0] = '\0';
        while (fgets(line, sizeof(line), f)) {
            /* The first line names the device AND where it is attached, e.g.
             * "...DualSense Wireless Controller at usb-generic-ehci-1.4...".
             * That tail is the only thing here that distinguishes one
             * controller's card from the other's -- the NAME is identical for
             * both. */
            if (header[0] == '\0') {
                snprintf(header, sizeof(header), "%s", line);
                char *nl = strchr(header, '\n');
                if (nl) *nl = '\0';
            }
            if (strstr(line, "DualSense")) { found = 1; break; }
        }
        fclose(f);
        if (!found) continue;

        /* MEASUREMENT, NOT A FIX. Both capture threads scan this same list and
         * both match on the name "DualSense", so with two controllers there is
         * nothing here that ties a card to the controller asking for it.
         *
         * On 2026-08-04 both threads reported bit-identical audio levels while
         * one controller was MUTED AT THE HARDWARE -- peaks 231/231, 188/188,
         * 237/237 -- which a muted microphone cannot produce. Both were
         * reading one microphone.
         *
         * This line prints what each card says about itself and which
         * controller is asking, so a later session can see whether they
         * correspond. Matching on it is deliberately NOT done yet: the text
         * format has not been read, and guessing at formats is what produced
         * three sessions of wrong conclusions. */
        mic_cap_log("scan: card=%d wants_node=%s header=[%s]",
                    card, want_node ? want_node : "?", header);

        /* 'c' = capture. The playback path uses the same name ending in 'p'.
         *
         * O_NONBLOCK IS LOAD-BEARING, NOT TIDINESS. Opening an ALSA device
         * that another process already holds WAITS FOREVER without it. With
         * two controllers connected, the second one's thread would reach this
         * line for card N, find the first controller already holding it, and
         * park in the kernel -- no error, no log line, no fall-through to its
         * own card. Observed: a "capture thread started" line with nothing
         * after it, and a second controller whose microphone reached nothing
         * at all.
         *
         * The playback opener has always passed this flag, which is why two
         * controllers' SPEAKERS work: a busy device returns immediately and
         * the scan continues to the next card. */
        snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0c", card);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            mic_cap_log("open failed card=%d path=%s errno=%d", card, path, errno);
            continue;
        }

        struct snd_pcm_hw_params hw;
        memset(&hw, 0, sizeof(hw));
        for (int i = 0; i < (int)(sizeof(hw.intervals)/sizeof(hw.intervals[0])); i++) {
            hw.intervals[i].min = 0;
            hw.intervals[i].max = 0xFFFFFFFFU;
        }
        for (int i = 0; i < (int)(sizeof(hw.masks)/sizeof(hw.masks[0])); i++)
            memset(&hw.masks[i], 0xFF, sizeof(hw.masks[i]));
        /* ACCESS: RW_INTERLEAVED(3) */
        memset(&hw.masks[0], 0, sizeof(hw.masks[0]));
        hw.masks[0].bits[3 / 32] = 1u << (3 % 32);
        /* FORMAT: S16_LE(2) */
        memset(&hw.masks[1], 0, sizeof(hw.masks[1]));
        hw.masks[1].bits[2 / 32] = 1u << (2 % 32);
        /* CHANNELS: exactly 2 -- the capture side, not the 4-channel playback */
        hw.intervals[2].min = hw.intervals[2].max = MIC_CAP_CHANNELS;
        hw.intervals[2].integer = 1;
        /* RATE: exactly 48000 */
        hw.intervals[3].min = hw.intervals[3].max = MIC_CAP_RATE;
        hw.intervals[3].integer = 1;
        /* BUFFER_SIZE: at least 20 ms of slack, same reasoning as playback */
        hw.intervals[9].min = 960;
        hw.intervals[9].max = 0xFFFFFFFFU;
        hw.intervals[9].integer = 0;

        int hw_rc = ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw);
        int prep_rc = (hw_rc >= 0) ? ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL) : -1;
        if (hw_rc < 0 || prep_rc < 0) {
            mic_cap_log("hw_params/prepare failed card=%d hw_rc=%d prep_rc=%d errno=%d",
                        card, hw_rc, prep_rc, errno);
            close(fd);
            fd = -1;
            continue;
        }
        /* DO NOT try to switch this back to blocking with fcntl. It was
         * tried: the call REPORTS SUCCESS and reads keep returning
         * immediately anyway. Measured result was 940 reads/second against the
         * ~100 the stream produces, roughly half of them failing with
         * "nothing ready" -- the loop spinning as fast as it could and the TV
         * sending nine times more data than the audio needs.
         *
         * The device stays non-blocking, which is what keeps a busy card from
         * parking the thread. The read loop waits with poll() instead, which
         * paces against the device without depending on a mode change. */
        mic_cap_log("capture opened card=%d fd=%d period_frames=%u buffer_frames=%u",
                    card, fd, (unsigned)hw.intervals[5].max, (unsigned)hw.intervals[9].max);
        if (out_card) *out_card = card;
        return fd;
    }
    mic_cap_log("no DualSense capture device found");
    return -1;
}


/* Reader thread: pull frames, measure them, log once a second.
 *
 * Peak amplitude is the number that matters. A stream that opens and returns
 * frames of zeros looks identical to a working one in a frame count -- and
 * that exact ambiguity cost a whole evening on the Windows side. */
static void *mic_capture_thread(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    /* The device file the app opened, e.g. /dev/hidraw1. The controller log
     * prints this on its session line, so the two TV logs line up.
     *
     * The reference also carried the USB socket here -- the physical route,
     * which is what the overlay row shows and what would eventually tie a
     * CARD to a CONTROLLER. This base has no such field on the device, so it
     * is left out rather than half-invented; the scan line below still prints
     * what each card says about itself. */
    const char *node = (c && c->dev.path[0]) ? c->dev.path : "?";

    int mic_card = -1;
    int fd = open_ds5_alsa_capture(&mic_card, node);
    if (fd < 0) {
        mic_cap_log("thread exiting: no capture device");
        return NULL;
    }

    /* Start the stream. Capture needs an explicit START; playback does not,
     * because the first write starts it implicitly. */
    if (ioctl(fd, SNDRV_PCM_IOCTL_START, NULL) < 0) {
        mic_cap_log("START failed errno=%d (continuing; first read may start it)", errno);
    }

    /* ONE BUFFER PER THREAD. THIS WAS `static`, AND THAT WAS THE BUG.
     *
     * `static` inside a function means ONE instance for the whole program --
     * not one per call, and not one per thread. With two controllers there are
     * two of these threads, and they were both reading their audio into the
     * SAME memory. Whichever read last overwrote the other, and then both
     * measured their peak levels from it and both SENT from it.
     *
     * Everything that made no sense for four sessions follows from that:
     *   - identical peak levels on two controllers, often bit-for-bit
     *   - occasional divergence, when one thread measured before the other
     *     overwrote
     *   - MUTE ONE and the MUTED controller still reported audio: its own
     *     read filled the buffer with silence, but the live thread's audio was
     *     sitting there when it measured
     *   - MUTE BOTH and both read exactly zero
     *
     * INVISIBLE WITH ONE CONTROLLER. One thread, one buffer, correct
     * behaviour -- the fault could not exist until a second thread did.
     *
     * It cannot go on the stack: ~2 KB is too much for a thread here.
     * Allocated per thread and freed on exit instead. */
    int16_t *buf = (int16_t *)malloc(sizeof(int16_t) * MIC_CAP_FRAMES * MIC_CAP_CHANNELS);
    if (!buf) {
        mic_cap_log("capture buffer allocation failed; thread exiting");
        close(fd);
        return NULL;
    }
    uint64_t reads_ok = 0, reads_failed = 0, frames_total = 0;
    uint64_t sends_ok = 0, sends_failed = 0;
    int16_t peak_ch0 = 0, peak_ch1 = 0;
    time_t last_report = 0;
    /* Self-heal counters, the same pair the playback path uses: reads that
     * failed outright in a row, and reads that needed any recovery in a row.
     * Locals, so per thread by construction.
     *
     * THE THRESHOLDS ARE INHERITED FROM PLAYBACK, NOT MEASURED FOR CAPTURE.
     * No capture failure has ever been observed here -- a million reads across
     * several sessions, none failed -- so there is nothing to tune them
     * against. They are a starting point. */
    int consec_fail = 0, retry_streak = 0;
    uint64_t heals = 0;

    mic_cap_log("%s card=%d thread running fd=%d", node, mic_card, fd);

    while (!c->stop) {
        /* Sleep until the device actually has audio, instead of asking
         * repeatedly whether it does. This is what paces the loop: without
         * it, a non-blocking read returns instantly and the loop spins.
         * The 100 ms timeout exists only so c->stop is checked on plug-out;
         * in normal running poll returns in ~10 ms when the next period
         * lands. */
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int pr = poll(&pfd, 1, 100);
        if (pr == 0) {
            continue;               /* nothing yet; re-check c->stop */
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            mic_cap_log("poll failed errno=%d, stopping", errno);
            break;
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            /* Overrun or the device went away. Re-prepare and carry on; the
             * read below reports anything that is still wrong. */
            ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
            ioctl(fd, SNDRV_PCM_IOCTL_START, NULL);
            continue;
        }

        struct snd_xferi xfer;
        memset(&xfer, 0, sizeof(xfer));
        xfer.buf = buf;
        xfer.frames = MIC_CAP_FRAMES;
        int rc = ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xfer);
        if (rc < 0) {
            const int read_errno = errno;
            reads_failed++;
            consec_fail++;
            int fatal = 0;
            if (read_errno == EPIPE) {
                /* overrun: the stream ran ahead of us. Re-prepare and restart. */
                ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
                ioctl(fd, SNDRV_PCM_IOCTL_START, NULL);
                retry_streak++;
            } else if (read_errno == EAGAIN) {
                struct timespec ts = {0, 1000000};   /* 1 ms */
                nanosleep(&ts, NULL);
                retry_streak++;
            } else {
                /* Anything else means the device itself is gone or broken.
                 * This used to log and exit, which left the microphone dead
                 * for the rest of the session with nothing said about it --
                 * while the PLAYBACK side of the same USB audio device has
                 * recovered from exactly this for months. */
                fatal = 1;
            }

            /* Two triggers, mirroring playback: failing outright, or needing
             * recovery over and over. The second fires before anything is
             * audible, which is the point of having it. */
            const char *why = NULL;
            if (fatal) why = "read error";
            else if (consec_fail >= 20) why = "consecutive-fail";
            else if (retry_streak >= 5) why = "retry-streak";
            if (!why) {
                continue;
            }

            /* Reopen. The scan runs again from scratch, because the card
             * index drifts across a re-enumeration and a remembered one would
             * reopen the wrong node. */
            mic_cap_log("%s card=%d self-heal: %s errno=%d consec=%d retry=%d, reopening",
                        node, mic_card, why, read_errno, consec_fail, retry_streak);
            close(fd);
            fd = open_ds5_alsa_capture(&mic_card, node);
            if (fd < 0) {
                mic_cap_log("%s self-heal: reopen FAILED, thread stopping", node);
                break;
            }
            if (ioctl(fd, SNDRV_PCM_IOCTL_START, NULL) < 0) {
                mic_cap_log("%s self-heal: START failed errno=%d (first read may start it)",
                            node, errno);
            }
            heals++;
            consec_fail = 0;
            retry_streak = 0;
            mic_cap_log("%s self-healed: reopened card=%d fd=%d", node, mic_card, fd);
            continue;
        }
        consec_fail = 0;
        retry_streak = 0;
        /* HOW MANY FRAMES WE ACTUALLY GOT IS xfer.result, NOT xfer.frames.
         * frames is what we ASKED for and the kernel leaves it alone; result
         * is what it delivered. The device hands over 48-frame periods (see
         * period_frames in the open log) while this asks for 480, so a
         * non-blocking read returns 48 and result says so.
         *
         * Counting xfer.frames instead sent 480 frames every time: ten times
         * the real data, nine tenths of it stale buffer contents. Measured as
         * 1000 reads/second with the peak levels FROZEN at identical values
         * for 25 seconds -- the same bytes going out over and over. It was
         * invisible while the read blocked, because then the call waited for
         * all 480 and the assumption happened to hold. */
        const long got = (long)xfer.result;
        if (got <= 0) {
            continue;
        }
        reads_ok++;
        frames_total += (uint64_t)got;

        /* Send it to the host. Audio is droppable: a failed send is a brief
         * gap in what Windows hears, never a reason to stop reading or to
         * tear anything down. The transport takes its own lock, so sending
         * from this thread cannot interleave with the input reader. */
        {
            size_t bytes = (size_t)got * MIC_CAP_CHANNELS * sizeof(int16_t);
            /* Belt and braces with the start point. Sending before the
             * session's opening exchange has finished puts audio on the wire
             * where the host expects a hello, and the session dies -- see the
             * comment at mic_capture_start()'s call site. The thread now
             * starts after the link is up, so this should never be false;
             * it is here because that race hid for two days and would hide
             * again. */
            if (!ctm_transport_connected(&c->xport)) {
                continue;
            }
            if (bytes > 0 && c_send(c, CTMB_MSG_MIC_AUDIO, CTMB_FLAG_OK, 0, buf, bytes) != 0) {
                sends_failed++;
            } else {
                sends_ok++;
            }
        }
        for (long i = 0; i < got; i++) {
            int16_t a = buf[i * MIC_CAP_CHANNELS + 0];
            int16_t b = buf[i * MIC_CAP_CHANNELS + 1];
            if (a < 0) a = (int16_t)-a;
            if (b < 0) b = (int16_t)-b;
            if (a > peak_ch0) peak_ch0 = a;
            if (b > peak_ch1) peak_ch1 = b;
        }

        time_t now = time(NULL);
        if (now != last_report) {
            last_report = now;
            mic_cap_log("%s card=%d reads_ok=%llu failed=%llu frames=%llu sent=%llu send_failed=%llu peak_ch0=%d peak_ch1=%d",
                        node, mic_card,
                        (unsigned long long)reads_ok,
                        (unsigned long long)reads_failed,
                        (unsigned long long)frames_total,
                        (unsigned long long)sends_ok,
                        (unsigned long long)sends_failed,
                        (int)peak_ch0, (int)peak_ch1);
            peak_ch0 = 0;
            peak_ch1 = 0;
        }
    }

    mic_cap_log("%s card=%d thread stopping: reads_ok=%llu failed=%llu frames=%llu heals=%llu",
                node, mic_card,
                (unsigned long long)reads_ok,
                (unsigned long long)reads_failed,
                (unsigned long long)frames_total,
                (unsigned long long)heals);
    free(buf);
    close(fd);
    return NULL;
}

/* EVERY EXIT FROM HERE SAYS WHY.
 *
 * Two of these paths used to return in silence: already-started, and not
 * armed. A second controller bridged with a working speaker and a healthy
 * session and simply never captured -- no start line, no failure line, nothing
 * anywhere to say what had been decided. That cost a session, and the only
 * honest next step was "add a log line", which is this.
 *
 * A silent early return is the most expensive kind of code to debug remotely:
 * it looks identical to code that never ran. */
static void mic_capture_start(ctm_controller_t *c)
{
    if (!c) {
        return;                 /* nothing to log against */
    }
    if (c->mic_cap_started) {
        ctl_log(c, "mic: capture not started -- already running for this controller");
        return;
    }
    if (strcmp(ctm_controller_bus(c), "USB") != 0) {
        ctl_log(c, "mic: capture not started -- not a wired connection");
        return;
    }
    if (!mic_capture_armed()) {
        ctl_log(c, "mic: capture not started -- not armed (%s absent)",
                MIC_CAPTURE_ON_PATH);
        return;
    }
    if (pthread_create(&c->mic_cap_thread, NULL, mic_capture_thread, c) == 0) {
        c->mic_cap_started = 1;
        ctl_log(c, "mic: capture thread started");
    } else {
        ctl_log(c, "mic: capture thread failed to start errno=%d", errno);
    }
}

static void mic_capture_stop(ctm_controller_t *c)
{
    if (!c || !c->mic_cap_started) return;
    /* c->stop is already set by plug_out before this is called. */
    pthread_join(c->mic_cap_thread, NULL);
    c->mic_cap_started = 0;
    ctl_log(c, "mic: capture thread stopped");
}

/* Pop one received message (1=got / 0=none / -1=dropped). When: the session
 * thread loop and the handshake wait. */
static int c_recv(ctm_controller_t *c, ctmb_header_t *h, uint8_t **payload)
{
    return ctm_transport_recv_msg(&c->xport, h, payload);
}

/* Read a sysfs text attribute, trimmed. When: evdev-grab vid/pid matching. */
static int read_text_file(const char *path, char *out, size_t out_len)
{
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, out_len - 1);
    close(fd);
    if (n <= 0) return -1;
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' ' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
    return 0;
}

/* Compare a hex-string sysfs attr to a numeric vid/pid. When: evdev matching. */
static int hex_equals(const char *text, unsigned int value)
{
    if (!text || !text[0]) return 0;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 16);
    return end != text && parsed == value;
}

/* Sony feature-0x05 "full BT mode" probe. When: at HID open, DS only
 * (gated by ops->request_bt_mode). */
static void request_full_bt_mode(int fd)
{
    uint8_t feature[64];
    memset(feature, 0, sizeof(feature));
    feature[0] = 0x05;
    if (ioctl(fd, HIDIOCGFEATURE(sizeof(feature)), feature) < 0) {
        fprintf(stderr, "controller: feature 0x05 failed errno=%d\n", errno);
    }
}

/* INVESTIGATION, 2026-08-08 -- read only, changes nothing.
 *
 * Does a WIRED DualSense report its own identity?
 *
 * The working assumption all along has been that a wired controller has no
 * unique identifier: `dev.mac` is empty on a cable, which is why two
 * controllers share one log file (the name falls back to the kind) and why
 * both introduce themselves to the host as the same made-up serial.
 *
 * But the controller stores its own Bluetooth MAC and reports it over USB
 * through the pairing-info feature report, 0x09 -- that is how the kernel's
 * PlayStation driver obtains a MAC for a wired DualSense. If it answers here,
 * the identifier exists and has simply never been asked for.
 *
 * The whole first line of the reply is logged as bytes rather than parsed. The
 * layout is documented but unverified on this hardware, and guessing at
 * formats is what produced three sessions of wrong conclusions elsewhere in
 * this project. Read the bytes first, decide what they mean second. */
static void probe_pairing_info(ctm_controller_t *c, int fd)
{
    uint8_t feature[20];
    memset(feature, 0, sizeof(feature));
    feature[0] = 0x09;
    if (ioctl(fd, HIDIOCGFEATURE(sizeof(feature)), feature) < 0) {
        ctl_log(c, "probe: feature 0x09 (pairing info) failed errno=%d", errno);
        return;
    }
    char hex[3 * sizeof(feature) + 1];
    int o = 0;
    for (size_t i = 0; i < sizeof(feature); ++i) {
        o += snprintf(hex + o, sizeof(hex) - (size_t)o, "%02x ", feature[i]);
    }
    ctl_log(c, "probe: feature 0x09 (pairing info) = %s", hex);
}

/* CRC32 (reflected, poly 0xedb88320) step. When: ctm_bt_sign_output only. */
static uint32_t crc32_step(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

/* Append the Sony BT HID output-report CRC32 (seed 0xa2) into the trailing 4
 * bytes. When: a DS patch_output hook, after rewriting a report. */
void ctm_bt_sign_output(uint8_t *data, size_t len)
{
    if (!data || len < 8) return;
    uint8_t seed = 0xa2;
    uint32_t crc = crc32_step(0xffffffffu, &seed, 1);
    crc = ~crc32_step(crc, data, len - 4);
    data[len - 4] = (uint8_t)(crc & 0xffu);
    data[len - 3] = (uint8_t)((crc >> 8) & 0xffu);
    data[len - 2] = (uint8_t)((crc >> 16) & 0xffu);
    data[len - 1] = (uint8_t)((crc >> 24) & 0xffu);
}

/* Open the controller's hidraw node (ops->select_node or dev.path), validate
 * vid/pid, fill caps + report descriptor. When: once by session_main before
 * the connect loop. Returns the fd, or -1. */
static int open_hid(ctm_controller_t *c, ctmb_device_caps_t *caps,
                    uint8_t *report_desc, uint32_t *report_desc_len)
{
    char node[64];
    const char *path = c->dev.path;
    if (c->ops->select_node && c->ops->select_node(&c->dev, node, sizeof(node)) == 0 && node[0]) {
        path = node;
    }

    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;

    struct hidraw_devinfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0) {
        close(fd);
        return -1;
    }
    unsigned int vid = (unsigned short)info.vendor;
    unsigned int pid = (unsigned short)info.product;
    if ((c->vid_num && vid != c->vid_num) || (c->pid_num && pid != c->pid_num)) {
        close(fd);
        return -1;
    }

    memset(caps, 0, sizeof(*caps));
    caps->vendor_id = (uint16_t)vid;
    caps->product_id = (uint16_t)pid;
    caps->bus = info.bustype ? (uint16_t)info.bustype : BUS_BLUETOOTH;
    caps->input_report_len = 1024;
    caps->output_report_len = 1024;
    caps->feature_report_len = 64;
    caps->flags = 1;
    snprintf(caps->path, sizeof(caps->path), "%s", path);
    snprintf(caps->serial, sizeof(caps->serial), "%s", c->dev.mac);
    snprintf(caps->manufacturer, sizeof(caps->manufacturer), "hidraw");
    if (ioctl(fd, HIDIOCGRAWNAME(sizeof(caps->product) - 1), caps->product) < 0 ||
        caps->product[0] == '\0') {
        snprintf(caps->product, sizeof(caps->product), "hidraw");
    }

    *report_desc_len = read_report_descriptor(fd, report_desc, MAX_REPORT_DESCRIPTOR);
    if (*report_desc_len) {
        derive_report_lengths(report_desc, *report_desc_len, caps);
        if (caps->input_report_len < 1024) caps->input_report_len = 1024;
        if (caps->output_report_len < 1024) caps->output_report_len = 1024;
    }

    if (c->ops->request_bt_mode) request_full_bt_mode(fd);

    /* Investigation only, wired DualSense. Read and log; nothing acts on it. */
    if (strcmp(ctm_controller_bus(c), "USB") == 0) probe_pairing_info(c, fd);
    return fd;
}

/* EVIOCGRAB the device's evdev nodes so webOS doesn't double-consume input.
 * When: at session start, BT/DS only (gated by ops->grab_evdev). */
static void grab_matching_evdev(ctm_controller_t *c)
{
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char vendor_path[160], product_path[160], vendor[32] = {0}, product[32] = {0};
        snprintf(vendor_path, sizeof(vendor_path), "/sys/class/input/%s/id/vendor", ent->d_name);
        snprintf(product_path, sizeof(product_path), "/sys/class/input/%s/id/product", ent->d_name);
        if (read_text_file(vendor_path, vendor, sizeof(vendor)) != 0 ||
            read_text_file(product_path, product, sizeof(product)) != 0 ||
            !hex_equals(vendor, c->vid_num) || !hex_equals(product, c->pid_num)) {
            continue;
        }
        char input_dir[160];
        snprintf(input_dir, sizeof(input_dir), "/sys/class/input/%s", ent->d_name);
        DIR *input = opendir(input_dir);
        if (!input) continue;
        struct dirent *child;
        while ((child = readdir(input)) != NULL && c->evdev_grab_count < MAX_EVDEV_GRABS) {
            if (strncmp(child->d_name, "event", 5) != 0) continue;
            char dev_path[64];
            snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", child->d_name);
            int fd = open(dev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                int idx = c->evdev_grab_count++;
                c->evdev_grabs[idx].fd = fd;
                snprintf(c->evdev_grabs[idx].path, sizeof(c->evdev_grabs[idx].path), "%s", dev_path);
                ctl_log(c, "grabbed %s", dev_path);
            } else {
                close(fd);
            }
        }
        closedir(input);
    }
    closedir(dir);
}

/* Un-grab + close the evdev nodes. When: each time a session ends. */
static void release_evdev_grabs(ctm_controller_t *c)
{
    for (int i = 0; i < c->evdev_grab_count; ++i) {
        if (c->evdev_grabs[i].fd >= 0) {
            ioctl(c->evdev_grabs[i].fd, EVIOCGRAB, 0);
            close(c->evdev_grabs[i].fd);
            c->evdev_grabs[i].fd = -1;
        }
    }
    c->evdev_grab_count = 0;
}

/* Whether an outbound report must be rate-limited (PACED flag or a host_config
 * paced report id). When: per OUTPUT report in handle_message. */
static int should_pace(const ctmb_host_config_t *cfg, const ctmb_header_t *h,
                       const uint8_t *payload, size_t len)
{
    if ((h->flags & CTMB_FLAG_PACED) != 0) return 1;
    if (!payload || len == 0) return 0;
    for (int i = 0; i < cfg->paced_report_count && i < 16; i++) {
        if (payload[0] == cfg->paced_report_ids[i]) return 1;
    }
    return 0;
}

/* Enqueue a paced output report into the ring (drops oldest if full).
 * When: handle_message decides a report must be paced. */
static void queue_paced(queued_report_t *q, int *head, int *count,
                        const uint8_t *data, size_t len)
{
    if (len > MAX_REPORT) return;
    if (*count >= PACED_QUEUE_CAP) {
        *head = (*head + 1) % PACED_QUEUE_CAP;
        (*count)--;
    }
    int idx = (*head + *count) % PACED_QUEUE_CAP;
    memcpy(q[idx].data, data, len);
    q[idx].len = len;
    (*count)++;
}

/* Thread-safe snapshot of the live settings. When: ctm_controller_get_settings
 * (so a patch_output hook can read current sliders without racing the UI). */
static tv_bridge_worker_settings_t copy_settings(ctm_controller_t *c)
{
    tv_bridge_worker_settings_t s;
    pthread_mutex_lock(&c->settings_mutex);
    s = c->settings;
    pthread_mutex_unlock(&c->settings_mutex);
    return s;
}

/* Returns nonzero to drop the report (patch consumed it), 0 to write. */
static int apply_output_settings(ctm_controller_t *c, uint8_t *data, size_t *len_io)
{
    if (!c->ops->patch_output) return 0;
    return c->ops->patch_output(c, data, len_io);
}

/* Write one report to the device WITHOUT the patch hook, mutex-guarded. When: a
 * type sends a report of its own rather than forwarding the host's -- the
 * confirmation light, say. Skipping the hook is deliberate: the hook exists to
 * rewrite the HOST's reports, and running it on ours would recurse. */
int ctm_controller_write_raw(ctm_controller_t *c, const uint8_t *data, size_t len)
{
    if (!c || c->hid_fd < 0 || !data || len == 0) return -1;
    pthread_mutex_lock(&c->hid_mutex);
    ssize_t n = write(c->hid_fd, data, len);
    pthread_mutex_unlock(&c->hid_mutex);
    return n == (ssize_t)len ? 0 : -1;
}

/* Remember any audio setting a report claims, so a later reopen can restore it.
 *
 * The audio device can vanish mid-session and be reopened, and a reopen has to
 * re-send the settings -- a fresh handle knows nothing about volume or routing.
 * Sending the hardcoded defaults there would silently undo whatever the host
 * had set: a user at volume 60 jumps back to full the first time the audio path
 * stumbles.
 *
 * Only the CLAIMED fields are taken. A report carries flags saying which
 * settings it is setting, and a field that is not claimed is untouched by that
 * report -- reading it anyway would record a zero the host never sent.
 *
 * Deliberately NOT a copy of the report. Replaying a whole report would replay
 * everything else in it, and most outbound reports are rumble.
 *
 * When: every outbound report, from the one place they all pass through. */
static void remember_audio_settings(ctm_controller_t *c, const uint8_t *data, size_t len)
{
    if (!c || !data) return;
    if (len <= DS5_IDX_AUDIO_CONTROL) return;
    if (data[0] != DS5_OUT_REPORT_ID) return;
    uint8_t claims = data[DS5_IDX_VALID_FLAG0];
    if (claims & DS5_F0_ALLOW_SPEAKER_VOLUME) {
        c->audio_spk_vol = data[DS5_IDX_SPEAKER_VOLUME];
    }
    if (claims & DS5_F0_ALLOW_AUDIO_CONTROL) {
        c->audio_control = data[DS5_IDX_AUDIO_CONTROL];
    }
}

/* Patch (via the ops hook) then write one report to the device, mutex-guarded.
 * When: every direct OUTPUT write and every paced-queue drain. */
static int hid_write_report(ctm_controller_t *c, const uint8_t *data, size_t len)
{
    if (!c || c->hid_fd < 0 || !data || len == 0) return -1;
    uint8_t patched[MAX_REPORT];
    if (len > sizeof(patched)) return -1;
    memcpy(patched, data, len);
    size_t patched_len = len;
    if (apply_output_settings(c, patched, &patched_len)) {
        return 0;
    }
    pthread_mutex_lock(&c->hid_mutex);
    ssize_t n = write(c->hid_fd, patched, patched_len);
    pthread_mutex_unlock(&c->hid_mutex);
    if (n == (ssize_t)patched_len) {
        /* After patching and only on success: what is recorded is what the
         * controller actually received. */
        remember_audio_settings(c, patched, patched_len);
        c->st_reports_out++;
        return 0;
    }
    return -1;
}

/* Composite: write a report verbatim to a specific sibling fd (no output-setting
 * patching -- the composite is an identity passthrough). */
static int hid_write_fd_raw(ctm_controller_t *c, int fd, const uint8_t *data, size_t len)
{
    if (!c || fd < 0 || !data || len == 0 || len > MAX_REPORT) return -1;
    pthread_mutex_lock(&c->hid_mutex);
    ssize_t n = write(fd, data, len);
    pthread_mutex_unlock(&c->hid_mutex);
    if (n == (ssize_t)len) { c->st_reports_out++; return 0; }
    return -1;
}

/* Flush queued paced reports as their schedule comes due. When: top of every
 * session-loop tick. */
static void drain_paced(ctm_controller_t *c, queued_report_t *q, int *head, int *count,
                        uint64_t *next_us, uint32_t pace_us)
{
    uint64_t now = now_us();
    if (*count <= 0) { *next_us = 0; return; }
    if (*next_us == 0) *next_us = now;
    while (*count > 0 && now >= *next_us) {
        queued_report_t *r = &q[*head];
        (void)hid_write_report(c, r->data, r->len);
        *head = (*head + 1) % PACED_QUEUE_CAP;
        (*count)--;
        if (pace_us == 0) pace_us = 10667;
        *next_us += pace_us;
        if (*next_us + pace_us < now) *next_us = now + pace_us;
        now = now_us();
    }
    if (*count <= 0) *next_us = 0;
}

/* Reader thread body: blocking-read hidraw input and forward it to the
 * transport. When: one per live session, started by run_session, stopped via
 * the wake pipe. */
/* --- Composite (puck): forward every HID interface ------------------------- */

/* Resolve the USB device dir (the one holding idVendor) for vid/pid via the
 * reliable /sys/class/input path -- the /sys/class/hidraw realpath is FLAKY in
 * the dev-mode jail, so the composite open must not depend on it. Mirrors
 * grab_matching_evdev / puck_usb_device_dir. 0 on success. */
static int resolve_usb_device_dir(unsigned int vid, unsigned int pid, char *out, size_t out_len)
{
    DIR *d = opendir("/sys/class/input");
    if (!d) return -1;
    struct dirent *e; int rc = -1;
    while ((e = readdir(d)) != NULL && rc != 0) {
        if (strncmp(e->d_name, "input", 5) != 0) continue;
        char attr[256], v[32] = {0}, p[32] = {0};
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/vendor", e->d_name);
        read_text_file(attr, v, sizeof(v));
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/product", e->d_name);
        read_text_file(attr, p, sizeof(p));
        if (!hex_equals(v, vid) || !hex_equals(p, pid)) continue;
        char link[256], real[1024];
        snprintf(link, sizeof(link), "/sys/class/input/%s/device", e->d_name);
        if (!realpath(link, real)) continue;
        while (real[0]) {                       /* walk up to the USB device dir */
            char idf[1100];
            snprintf(idf, sizeof(idf), "%s/idVendor", real);
            if (access(idf, F_OK) == 0) { snprintf(out, out_len, "%s", real); rc = 0; break; }
            char *s = strrchr(real, '/'); if (!s || s == real) break; *s = '\0';
        }
    }
    closedir(d);
    return rc;
}

/* Read the IN (0x80 set) and OUT (0x80 clear) endpoint addresses of a USB
 * interface dir from its ep_* children. Leaves either at 0 if absent. */
static void iface_endpoints(const char *ifdir, uint8_t *in_ep, uint8_t *out_ep)
{
    DIR *d = opendir(ifdir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "ep_", 3) != 0) continue;
        char epf[1200], val[16] = {0};
        snprintf(epf, sizeof(epf), "%s/%s/bEndpointAddress", ifdir, e->d_name);
        if (read_text_file(epf, val, sizeof(val)) != 0) continue;
        unsigned int a = (unsigned int)strtoul(val, NULL, 16);
        if (a & 0x80u) { if (in_ep && *in_ep == 0) *in_ep = (uint8_t)a; }
        else { if (out_ep && *out_ep == 0) *out_ep = (uint8_t)a; }
    }
    closedir(d);
}

/* Find /dev/hidrawN under a USB interface dir (ifdir/<hid>/hidraw/hidrawN). */
static int find_hidraw_under(const char *ifdir, char *out, size_t outlen)
{
    DIR *d = opendir(ifdir); if (!d) return -1;
    struct dirent *e; int rc = -1;
    while ((e = readdir(d)) != NULL && rc != 0) {
        if (e->d_name[0] == '.') continue;
        char hp[1200]; snprintf(hp, sizeof(hp), "%s/%s/hidraw", ifdir, e->d_name);
        DIR *h = opendir(hp);
        if (h) {
            struct dirent *he;
            while ((he = readdir(h)) != NULL) {
                if (strncmp(he->d_name, "hidraw", 6) == 0) {
                    snprintf(out, outlen, "/dev/%s", he->d_name); rc = 0; break;
                }
            }
            closedir(h);
        }
    }
    closedir(d);
    return rc;
}

/* Open every class-03 HID interface of the puck's USB device R/W (the siblings),
 * and record the primary's (c->dev.path) endpoints + interface number -- all via
 * the reliable /sys/class/input resolution (no flaky /sys/class/hidraw realpath).
 * The primary is NOT added to comp[]; its endpoints go to c->primary_*.
 * When: run_session start, for composite controllers (the puck). */
static void open_composite_siblings(ctm_controller_t *c)
{
    char usbdir[512];
    if (resolve_usb_device_dir(c->vid_num, c->pid_num, usbdir, sizeof(usbdir)) != 0) {
        ctl_log(c, "composite: USB device dir unresolved vid=%04x pid=%04x",
                c->vid_num, c->pid_num);
        return;
    }
    const char *base = strrchr(usbdir, '/'); base = base ? base + 1 : usbdir;
    size_t blen = strlen(base);
    DIR *d = opendir(usbdir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && c->comp_count < 15) {
        if (strncmp(e->d_name, base, blen) != 0 || e->d_name[blen] != ':') continue;
        char ifdir[1024], clsf[1100], cls[8] = {0};
        snprintf(ifdir, sizeof(ifdir), "%s/%s", usbdir, e->d_name);
        snprintf(clsf, sizeof(clsf), "%s/bInterfaceClass", ifdir);
        if (read_text_file(clsf, cls, sizeof(cls)) != 0 || strcmp(cls, "03") != 0) continue;
        char hidpath[64];
        if (find_hidraw_under(ifdir, hidpath, sizeof(hidpath)) != 0) continue;
        uint8_t in_ep = 0, out_ep = 0, iface = 0xff;
        char numf[1100], num[8] = {0};
        snprintf(numf, sizeof(numf), "%s/bInterfaceNumber", ifdir);
        if (read_text_file(numf, num, sizeof(num)) == 0) iface = (uint8_t)strtoul(num, NULL, 16);
        iface_endpoints(ifdir, &in_ep, &out_ep);
        if (strcmp(hidpath, c->dev.path) == 0) {            /* the primary */
            c->primary_in_ep = in_ep; c->primary_out_ep = out_ep; c->primary_iface = iface;
            ctl_log(c, "composite primary %s if=%u in_ep=0x%02x out_ep=0x%02x",
                    hidpath, iface, in_ep, out_ep);
            continue;
        }
        int fd = open(hidpath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) fd = open(hidpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        comp_iface_t *ci = &c->comp[c->comp_count++];
        ci->c = c; ci->fd = fd; ci->started = 0;
        ci->in_ep = in_ep; ci->out_ep = out_ep; ci->iface = iface;
        ctl_log(c, "composite sibling %s if=%u in_ep=0x%02x out_ep=0x%02x",
                hidpath, iface, in_ep, out_ep);
    }
    closedir(d);
}

/* Sibling reader: poll one interface's hidraw, forward input tagged with its IN
 * endpoint. When: one thread per sibling, while comp_run. */
static void *composite_reader_main(void *arg)
{
    comp_iface_t *ci = (comp_iface_t *)arg;
    ctm_controller_t *c = ci->c;
    while (c->comp_run && !c->stop) {
        struct pollfd pfd; pfd.fd = ci->fd; pfd.events = POLLIN; pfd.revents = 0;
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfd.revents & POLLIN)) continue;
        for (;;) {
            uint8_t buf[MAX_REPORT];
            ssize_t n = read(ci->fd, buf, sizeof(buf));
            if (n > 0) {
                if (c_send(c, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, ci->in_ep, buf, (size_t)n) != 0) break;
                c->st_reports_in++;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            break;
        }
    }
    return NULL;
}

static void *input_thread_main(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    while (!c->stop) {
        struct pollfd pfds[2];
        pfds[0].fd = c->hid_fd; pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = c->wake_pipe[0]; pfds[1].events = POLLIN; pfds[1].revents = 0;
        int pr = poll(pfds, 2, 1);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (pfds[1].revents & POLLIN) break;
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfds[0].revents & POLLIN)) continue;
        for (;;) {
            uint8_t buf[MAX_REPORT];
            ssize_t n = read(c->hid_fd, buf, sizeof(buf));
            if (n > 0) {
                if (c_send(c, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, c->primary_in_ep, buf, (size_t)n) != 0) {
                    c->stop = 1;
                    break;
                }
                c->st_reports_in++;
                /* Relay first, look second: the host sees the report whatever
                 * the type makes of it. */
                if (c->ops && c->ops->on_input_report) {
                    c->ops->on_input_report(c, buf, (size_t)n);
                }
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            break;
        }
    }
    return NULL;
}

/* Send HELLO (caps + HID report descriptor). When: first message of each
 * session, from handshake. */
static int send_hello(ctm_controller_t *c, const ctmb_device_caps_t *caps,
                      const uint8_t *report_desc, uint32_t report_desc_len)
{
    ctmb_hid_descriptor_info_t desc_info;
    memset(&desc_info, 0, sizeof(desc_info));
    desc_info.report_descriptor_len = report_desc_len;
    size_t hello_len = sizeof(*caps) + sizeof(desc_info) + report_desc_len;
    uint8_t *hello = (uint8_t *)malloc(hello_len);
    if (!hello) return -1;
    memcpy(hello, caps, sizeof(*caps));
    memcpy(hello + sizeof(*caps), &desc_info, sizeof(desc_info));
    if (report_desc_len) {
        memcpy(hello + sizeof(*caps) + sizeof(desc_info), report_desc, report_desc_len);
    }
    int rc = c_send(c, CTMB_MSG_HELLO, CTMB_FLAG_OK, 0, hello, hello_len);
    free(hello);
    return rc;
}

/* Composite: pick the hidraw fd for a feature request by the interface number
 * encoded in the high byte of request_id (set by the host). Non-composite, or no
 * matching interface, falls back to the primary fd. */
static int feature_fd_for(ctm_controller_t *c, uint32_t request_id)
{
    if (!(c->ops && c->ops->composite)) return c->hid_fd;
    uint8_t iface = (uint8_t)(request_id >> 24);
    if (iface == c->primary_iface) return c->hid_fd;
    for (int i = 0; i < c->comp_count; ++i) {
        if (c->comp[i].iface == iface) return c->comp[i].fd;
    }
    return c->hid_fd;
}

/* Dispatch one inbound message: OUTPUT (paced or direct write), FEATURE_GET/SET
 * (hidraw ioctl + reply), HOST_CONFIG (pacing params). When: per message
 * decoded in the session loop. */
static void handle_message(ctm_controller_t *c, ctmb_host_config_t *host_cfg,
                           queued_report_t *paced_q, int *paced_head, int *paced_count,
                           const ctmb_header_t *h, uint8_t *payload)
{
    if (h->type == CTMB_MSG_OUTPUT_REPORT) {
        if (c->ops && c->ops->composite) {
            /* Route the host's OUT write to the interface that owns the OUT
             * endpoint it addressed (request_id = endpoint). Verbatim; pacing is
             * a BT-audio concern that does not apply to the composite puck. */
            int fd = c->hid_fd;
            uint8_t ep = (uint8_t)h->request_id;
            if (ep != 0 && ep != c->primary_out_ep) {
                for (int i = 0; i < c->comp_count; ++i) {
                    if (c->comp[i].out_ep == ep) { fd = c->comp[i].fd; break; }
                }
            }
            (void)hid_write_fd_raw(c, fd, payload, h->payload_len);
        } else if (should_pace(host_cfg, h, payload, h->payload_len)) {
            queue_paced(paced_q, paced_head, paced_count, payload, h->payload_len);
        } else {
            (void)hid_write_report(c, payload, h->payload_len);
        }
    } else if (h->type == CTMB_MSG_FEATURE_GET) {
        int ok = 0;
        if (h->payload_len > 0 && h->payload_len <= MAX_REPORT) {
            uint8_t feature[MAX_REPORT];
            memcpy(feature, payload, h->payload_len);
            int fd = feature_fd_for(c, h->request_id);
            pthread_mutex_lock(&c->hid_mutex);
            int rc = ioctl(fd, HIDIOCGFEATURE(h->payload_len), feature);
            pthread_mutex_unlock(&c->hid_mutex);
            if (rc >= 0) {
                ok = c_send(c, CTMB_MSG_FEATURE_REPORT, CTMB_FLAG_OK,
                            h->request_id, feature, h->payload_len) == 0;
            }
        }
        if (!ok) (void)c_send(c, CTMB_MSG_FEATURE_REPORT, 0, h->request_id, NULL, 0);
    } else if (h->type == CTMB_MSG_FEATURE_SET) {
        int ok = 0;
        if (h->payload_len > 0 && h->payload_len <= MAX_REPORT) {
            uint8_t feature[MAX_REPORT];
            memcpy(feature, payload, h->payload_len);
            int fd = feature_fd_for(c, h->request_id);
            pthread_mutex_lock(&c->hid_mutex);
            ok = ioctl(fd, HIDIOCSFEATURE(h->payload_len), feature) >= 0;
            pthread_mutex_unlock(&c->hid_mutex);
        }
        (void)c_send(c, CTMB_MSG_FEATURE_REPORT, ok ? CTMB_FLAG_OK : 0, h->request_id, NULL, 0);
    } else if (h->type == CTMB_MSG_HOST_CONFIG && h->payload_len >= sizeof(*host_cfg)) {
        memcpy(host_cfg, payload, sizeof(*host_cfg));
        if (host_cfg->bt_pace_us == 0) host_cfg->bt_pace_us = 10667;
    } else if (h->type == CTMB_MSG_ISO_AUDIO) {
        /* Wired speaker and haptics. A host that never sends this never
         * reaches here, and a controller with no audio device open drops it. */
        write_iso_audio(c, payload, h->payload_len);
    }
}

static int handshake(ctm_controller_t *c, const ctmb_device_caps_t *caps,
                     const uint8_t *report_desc, uint32_t report_desc_len,
                     ctmb_host_config_t *host_cfg)
{
    /* Composite (puck): forward the captured enumeration verbatim BEFORE HELLO so
     * the host can build the composite device from it. Identity passthrough. */
    if (c->enum_payload && c->enum_payload_len > 0 &&
        c_send(c, CTMB_MSG_ENUM, CTMB_FLAG_OK, 0, c->enum_payload, (size_t)c->enum_payload_len) != 0) {
        ctl_log(c, "ENUM send failed");
        return -1;
    }
    if (send_hello(c, caps, report_desc, report_desc_len) != 0) {
        ctl_log(c, "HELLO failed");
        return -1;
    }
    /* Relay types (puck/xbox/generic) do not require HOST_CONFIG — proceed
     * straight to the loop, which still applies HOST_CONFIG if it arrives. */
    if (!c->ops->needs_host_config) {
        host_cfg->bt_pace_us = 10667;
        return 0;
    }
    ctmb_header_t h;
    uint8_t *payload = NULL;
    uint64_t start = now_us();
    for (;;) {
        if (c->stop) return -1;
        if (c->xport.kind == CTM_TRANSPORT_ENET) {
            if (ctm_transport_service(&c->xport, 50) < 0) { ctl_log(c, "host config wait: link dropped"); return -1; }
        }
        int got = c_recv(c, &h, &payload);
        if (got < 0) { ctl_log(c, "host config receive failed"); return -1; }
        if (got == 0) {
            if (now_us() - start >= 5000000ull) { ctl_log(c, "host config timeout"); return -1; }
            continue;
        }
        if (h.type != CTMB_MSG_HOST_CONFIG || h.payload_len < sizeof(*host_cfg)) {
            ctl_log(c, "host config unexpected type=%u len=%u", h.type, h.payload_len);
            free(payload);
            return -1;
        }
        memcpy(host_cfg, payload, sizeof(*host_cfg));
        free(payload);
        break;
    }
    if (host_cfg->bt_pace_us == 0) host_cfg->bt_pace_us = 10667;
    return 0;
}

/* Run one connected session: handshake, start the reader thread, then the
 * output/feature receive loop + paced drain until the link drops or stop. Tears
 * the reader thread down on exit. When: per successful connect, from session_main. */
static void run_session(ctm_controller_t *c, const ctmb_device_caps_t *caps,
                        const uint8_t *report_desc, uint32_t report_desc_len)
{
    ctmb_host_config_t host_cfg;
    queued_report_t paced_q[PACED_QUEUE_CAP];
    int paced_head = 0, paced_count = 0;
    uint64_t next_paced_us = 0;
    memset(&host_cfg, 0, sizeof(host_cfg));
    memset(paced_q, 0, sizeof(paced_q));

    if (handshake(c, caps, report_desc, report_desc_len, &host_cfg) != 0) return;

    { uint8_t drain[64]; while (read(c->wake_pipe[0], drain, sizeof(drain)) > 0) { /* discard */ } }

    /* Composite: open siblings + resolve the primary endpoints BEFORE the input
     * thread starts (it tags primary input with c->primary_in_ep). */
    if (c->ops->composite) open_composite_siblings(c);

    /* Microphone capture starts HERE, and not a line earlier.
     *
     * It used to start when the audio device opened, which is during session
     * setup -- so microphone chunks landed on a connection still handshaking,
     * the host saw audio where it expected a hello, and EVERY SESSION DIED
     * (host config receive failed / bridge hello failed). The handshake above
     * has returned by this point, so the link is up. */
    mic_capture_start(c);

    c->input_thread_started = 0;
    if (pthread_create(&c->input_thread, NULL, input_thread_main, c) == 0) {
        c->input_thread_started = 1;
    } else {
        ctl_log(c, "input thread failed errno=%d", errno);
        for (int i = 0; i < c->comp_count; ++i)
            if (c->comp[i].fd >= 0) { close(c->comp[i].fd); c->comp[i].fd = -1; }
        c->comp_count = 0;
        return;
    }
    c->comp_run = 1;
    if (c->ops->composite) {
        for (int i = 0; i < c->comp_count; ++i) {
            if (pthread_create(&c->comp[i].thread, NULL, composite_reader_main, &c->comp[i]) == 0)
                c->comp[i].started = 1;
        }
        if (c->comp_count > 0)
            ctl_log(c, "composite: forwarding %d sibling interface(s)", c->comp_count);
    }
    ctl_log(c, "active host=%s port=%d path=%s product=%s transport=%s",
         c->host, c->port, c->dev.path, caps->product,
         c->xport.kind == CTM_TRANSPORT_ENET ? "ENet/UDP" : "TCP");

    pthread_mutex_lock(&c->status_mutex);
    c->st_connected = 1;
    c->st_transport_enet = (c->xport.kind == CTM_TRANSPORT_ENET) ? 1 : 0;
    pthread_mutex_unlock(&c->status_mutex);

    int link_alive = 1;
    while (!c->stop && link_alive) {
        drain_paced(c, paced_q, &paced_head, &paced_count, &next_paced_us, host_cfg.bt_pace_us);
        int timeout_ms = 50;
        if (paced_count > 0 && next_paced_us != 0) {
            uint64_t now = now_us();
            timeout_ms = next_paced_us <= now ? 0 : (int)((next_paced_us - now) / 1000u);
            if (timeout_ms > 50) timeout_ms = 50;
        }
        if (c->xport.kind == CTM_TRANSPORT_ENET) {
            if (ctm_transport_service(&c->xport, 1) < 0) {
                ctl_log(c, "ENet link lost");
                link_alive = 0;
            } else {
                ctmb_header_t h;
                uint8_t *payload = NULL;
                while (c_recv(c, &h, &payload) == 1) {
                    handle_message(c, &host_cfg, paced_q, &paced_head, &paced_count, &h, payload);
                    free(payload);
                    payload = NULL;
                }
            }
        } else {
            struct pollfd pfd;
            pfd.fd = c->xport.fd; pfd.events = POLLIN; pfd.revents = 0;
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                link_alive = 0;
            } else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                link_alive = 0;
            } else if (pfd.revents & POLLIN) {
                ctmb_header_t h;
                uint8_t *payload = NULL;
                if (c_recv(c, &h, &payload) != 1) {
                    free(payload);
                    link_alive = 0;
                } else {
                    handle_message(c, &host_cfg, paced_q, &paced_head, &paced_count, &h, payload);
                    free(payload);
                }
            }
        }
    }

    pthread_mutex_lock(&c->status_mutex);
    c->st_connected = 0;
    pthread_mutex_unlock(&c->status_mutex);

    c->comp_run = 0;
    if (c->wake_pipe[1] >= 0) (void)write(c->wake_pipe[1], "x", 1);
    if (c->input_thread_started) {
        pthread_join(c->input_thread, NULL);
        c->input_thread_started = 0;
    }
    for (int i = 0; i < c->comp_count; ++i) {
        if (c->comp[i].started) pthread_join(c->comp[i].thread, NULL);
        if (c->comp[i].fd >= 0) { close(c->comp[i].fd); c->comp[i].fd = -1; }
    }
    c->comp_count = 0;
}

/* Session thread body: open HID + wake pipe once, then the dual-probe
 * connect -> grab -> run_session -> release -> disconnect loop until plug_out.
 * When: the controller's own thread, started by plug_in. */
static void *session_main(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    ctmb_device_caps_t caps;
    uint8_t report_desc[MAX_REPORT_DESCRIPTOR];
    uint32_t report_desc_len = 0;

    c->hid_fd = open_hid(c, &caps, report_desc, &report_desc_len);
    if (c->hid_fd < 0) {
        ctl_log(c, "hid open failed path=%s errno=%d", c->dev.path, errno);
        c->stop = 1;
        return NULL;
    }
    /* Composite primary endpoints are resolved in open_composite_siblings()
     * (reliable /sys/class/input path), at run_session start. */
    if (pipe(c->wake_pipe) != 0) {
        ctl_log(c, "wake pipe failed errno=%d", errno);
        c->stop = 1;
        return NULL;
    }
    (void)fcntl(c->wake_pipe[0], F_SETFL, fcntl(c->wake_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(c->wake_pipe[1], F_SETFL, fcntl(c->wake_pipe[1], F_GETFL, 0) | O_NONBLOCK);

    while (!c->stop) {
        while (!c->stop &&
               ctm_transport_connect_once(&c->xport, c->host, c->port, 400) != 0) {
            for (int slept = 0; slept < 500 && !c->stop; slept += 50) usleep(50000);
        }
        if (c->stop) break;
        ctl_log(c, "connected via %s", c->xport.kind == CTM_TRANSPORT_ENET ? "ENet/UDP" : "TCP");

        if (c->ops->grab_evdev) grab_matching_evdev(c);
        if (c->ops->on_plug_init) c->ops->on_plug_init(c, &c->xport);
        run_session(c, &caps, report_desc, report_desc_len);
        release_evdev_grabs(c);

        ctm_transport_disconnect(&c->xport);
        if (!c->stop) ctl_log(c, "link lost; retrying probe loop");
    }
    c->stop = 1;
    return NULL;
}

/* --- lifecycle ----------------------------------------------------------- */

/* Build an idle controller for a detected device (factory picks its ops).
 * When: the UI/monitor decides to offer a device; before plug_in. */
ctm_controller_t *ctm_controller_create(const ctm_controller_dev_t *dev)
{
    if (!dev) return NULL;
    ctm_controller_t *c = (ctm_controller_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->dev = *dev;
    c->ops = ctm_controller_ops_for(dev);
    c->vid_num = (unsigned int)strtoul(dev->vid, NULL, 16);
    c->pid_num = (unsigned int)strtoul(dev->pid, NULL, 16);
    c->hid_fd = -1;
    c->alsa_fd = -1;
    c->mic_cap_started = 0;
    /* The defaults stop being an assertion about what the settings are and
     * become the starting values. Anything the host sets replaces them, so the
     * first open behaves exactly as before and a reopen no longer reverts. */
    c->audio_spk_vol = DS5_SPEAKER_VOLUME_MAX;
    c->audio_control = DS5_AUDIO_OUT_PATH_SPEAKER | DS5_AUDIO_ECHO_NOISE_CANCEL;
    c->wake_pipe[0] = -1;
    c->wake_pipe[1] = -1;
    for (int i = 0; i < MAX_EVDEV_GRABS; ++i) c->evdev_grabs[i].fd = -1;
    pthread_mutex_init(&c->hid_mutex, NULL);
    pthread_mutex_init(&c->settings_mutex, NULL);
    pthread_mutex_init(&c->status_mutex, NULL);
    return c;
}

/* Open this controller's per-MAC log file (/tmp/ctm-<mac>.log). When: plug_in. */
static void open_log(ctm_controller_t *c)
{
    char mac[64], name[128];
    size_t o = 0;
    for (size_t i = 0; c->dev.mac[i] && o + 1 < sizeof(mac); ++i) {
        char ch = c->dev.mac[i];
        if (ch != ':') mac[o++] = ch;
    }
    mac[o] = '\0';
    snprintf(name, sizeof(name), "/tmp/ctm-%s.log", mac[0] ? mac : c->ops->kind);
    c->log = fopen(name, "a");
}

/* Start bridging: open log, bring up ENet + transport, launch the session
 * thread. Returns once the thread is started (connect happens async). When: the
 * user clicks Plug in. */
/* Composite: hand the controller the forwarded enumeration payload (CTMB_MSG_ENUM),
 * which handshake sends verbatim before HELLO. Copies the bytes. When: app plug-in. */
void ctm_controller_set_enum_payload(ctm_controller_t *c, const uint8_t *payload, int len)
{
    if (!c) return;
    free(c->enum_payload);
    c->enum_payload = NULL;
    c->enum_payload_len = 0;
    if (payload && len > 0) {
        c->enum_payload = (uint8_t *)malloc((size_t)len);
        if (c->enum_payload) {
            memcpy(c->enum_payload, payload, (size_t)len);
            c->enum_payload_len = len;
        }
    }
}

int ctm_controller_plug_in(ctm_controller_t *c, const char *host, int port)
{
    if (!c || !host || !host[0] || port <= 0) return -1;
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->port = port;
    c->stop = 0;
    open_log(c);

    pthread_once(&g_enet_once, enet_global_init_once);
    if (g_enet_ready) {
        c->enet = enet_client_create();
        if (!c->enet) ctl_log(c, "enet client create failed; ENet disabled");
    }
    ctm_transport_init(&c->xport, c->enet);

    if (pthread_create(&c->session_thread, NULL, session_main, c) != 0) {
        ctl_log(c, "session thread failed errno=%d", errno);
        ctm_transport_destroy(&c->xport);
        if (c->enet) { enet_client_destroy(c->enet); c->enet = NULL; }
        if (c->log) { fclose(c->log); c->log = NULL; }
        return -1;
    }
    c->session_started = 1;
    return 0;
}

/* Stop bridging: signal stop, wake + join the session thread, then close the
 * HID fd, transport, evdev grabs, and log. When: the user clicks Plug out, or
 * the device disconnects. */
void ctm_controller_open_alsa_playback(ctm_controller_t *c)
{
    if (!c || c->alsa_fd >= 0) return;
    c->alsa_fd = open_ds5_alsa_playback(c->dev.path);
    if (c->alsa_fd < 0) {
        ctl_log(c, "alsa: DS5 playback open failed (wired audio unavailable)");
        return;
    }
    ctl_log(c, "alsa: opened DS5 playback for ISO passthrough (fd=%d)", c->alsa_fd);

    /* ONE merged init report, written atomically.
     *
     * This was two reports 3ms apart. The first set volume while claiming
     * audio-control and supplying zero for it -- routing playback to the
     * headphone jack with echo cancel OFF -- and the second corrected the
     * routing. On TVs where writes fail, the first landing without the second
     * strands the controller routed to a headphone jack that isn't there:
     * silence, worse than the attenuation this init exists to prevent. One
     * report removes the window entirely.
     *
     * The host's own launch sequence has the same two-step flaw (measured on
     * C3: headphone then speaker 8ms apart, audible as a brief blip). We
     * deliberately do NOT copy its shape -- correct behaviour is the target,
     * not parity with the game.
     *
     * Claims ONLY speaker volume and audio control. It previously claimed
     * seven further sections while supplying zero for all of them, actively
     * zeroing settings it never intended to touch. Dropping those leaves the
     * controller's own microphone mute alone -- respecting the physical mute
     * button -- and removes an LED-blanking side effect at every open.
     *
     * Length comes from the array, not a hand-typed run of zeros: the old
     * literals were 49 bytes where every host report on the wire is 48. */
    uint8_t spk_init[DS5_OUT_REPORT_LEN] = {0};
    spk_init[0]                      = DS5_OUT_REPORT_ID;
    spk_init[DS5_IDX_VALID_FLAG0]    = DS5_F0_ALLOW_SPEAKER_VOLUME |
                                       DS5_F0_ALLOW_AUDIO_CONTROL;
    spk_init[DS5_IDX_VALID_FLAG1]    = 0x00;  /* claim nothing else */
    spk_init[DS5_IDX_SPEAKER_VOLUME] = c->audio_spk_vol;
    spk_init[DS5_IDX_AUDIO_CONTROL]  = c->audio_control;
    int svrc = hid_write_report(c, spk_init, sizeof(spk_init));
    ctl_log(c, "alsa: speaker init (merged, %zu bytes) vol=0x%02x ctrl=0x%02x rc=%d hid_fd=%d",
            sizeof(spk_init), c->audio_spk_vol, c->audio_control, svrc, c->hid_fd);

    /* ds5-aurora fires an open-tone burst here. NOT PORTED: it is a recorded
     * DECISIVE NEGATIVE -- tested on both fault classes with clean deliveries,
     * and the audio still attenuated, because audio content was never the
     * lever and control bytes were. See the disproven list in
     * controller-attenuation-investigation.md before reaching for it again. */
}

void ctm_controller_plug_out(ctm_controller_t *c)
{
    if (!c) return;
    c->stop = 1;
    if (c->xport.fd >= 0) shutdown(c->xport.fd, SHUT_RDWR);
    if (c->wake_pipe[1] >= 0) (void)write(c->wake_pipe[1], "x", 1);
    if (c->session_started) {
        pthread_join(c->session_thread, NULL);
        c->session_started = 0;
    }
    ctm_transport_disconnect(&c->xport);
    ctm_transport_destroy(&c->xport);
    if (c->hid_fd >= 0) { close(c->hid_fd); c->hid_fd = -1; }
    mic_capture_stop(c);
    if (c->alsa_fd >= 0) { close(c->alsa_fd); c->alsa_fd = -1; }
    c->alsa_consec_fail = 0;
    c->alsa_retry_streak = 0;
    if (c->wake_pipe[0] >= 0) { close(c->wake_pipe[0]); c->wake_pipe[0] = -1; }
    if (c->wake_pipe[1] >= 0) { close(c->wake_pipe[1]); c->wake_pipe[1] = -1; }
    if (c->enet) { enet_client_destroy(c->enet); c->enet = NULL; }
    release_evdev_grabs(c);
    if (c->log) { fclose(c->log); c->log = NULL; }
}

/* Push new UI settings to the controller (stored + forwarded to ops). When: a
 * detail-window slider/toggle changes while plugged in. */
void ctm_controller_set_settings(ctm_controller_t *c, const tv_bridge_worker_settings_t *s)
{
    if (!c || !s) return;
    pthread_mutex_lock(&c->settings_mutex);
    c->settings = *s;
    pthread_mutex_unlock(&c->settings_mutex);
    if (c->ops->set_settings) c->ops->set_settings(c, s);
}

/* Read the controller's live settings (snapshot). When: UI refresh, or a
 * patch_output hook reading current values. */
void ctm_controller_get_settings(ctm_controller_t *c, tv_bridge_worker_settings_t *out)
{
    if (!c || !out) return;
    *out = copy_settings(c);
}

/* Snapshot live bridging status for the UI panel. When: the UI status timer
 * (~500 ms). connected/transport/last_event are read under status_mutex; the
 * report counters are read advisorily (single-writer, monotonic). */
void ctm_controller_get_status(ctm_controller_t *c, ctm_controller_status_t *out)
{
    if (!c || !out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&c->status_mutex);
    out->connected = c->st_connected ? true : false;
    out->transport_enet = c->st_transport_enet ? true : false;
    snprintf(out->last_event, sizeof(out->last_event), "%s", c->st_last_event);
    pthread_mutex_unlock(&c->status_mutex);
    out->reports_in = c->st_reports_in;
    out->reports_out = c->st_reports_out;
}

/* Free an idle (already plugged-out) controller. When: the device is removed
 * from the list. */
void ctm_controller_destroy(ctm_controller_t *c)
{
    if (!c) return;
    pthread_mutex_destroy(&c->hid_mutex);
    pthread_mutex_destroy(&c->settings_mutex);
    pthread_mutex_destroy(&c->status_mutex);
    free(c->enum_payload);
    free(c);
}

/* --- factory: specific types first, generic last ------------------------- */

static const ctm_controller_ops_t *const k_registry[] = {
    &ctm_controller_steam_puck_ops,
    &ctm_controller_ds5_ops,
    &ctm_controller_ds5e_ops,
    &ctm_controller_ds4_ops,
    &ctm_controller_xbox_ops,
    &ctm_controller_generic_ops,
};

const ctm_controller_ops_t *ctm_controller_ops_for(const ctm_controller_dev_t *dev)
{
    for (size_t i = 0; i < sizeof(k_registry) / sizeof(k_registry[0]); ++i) {
        const ctm_controller_ops_t *ops = k_registry[i];
        if (ops->matches && ops->matches(dev)) return ops;
    }
    return &ctm_controller_generic_ops;
}
