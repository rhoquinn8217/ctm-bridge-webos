/* Controller pump + lifecycle + factory.
 *
 * One isolated bridging session per controller: its own reader + session
 * threads, HID fd, ctm_transport, settings, and per-MAC log. It merged the
 * first standalone worker's DS pump (2 threads, pacing, evdev grab, HELLO/
 * HOST_CONFIG handshake) and its verbatim HID relay; both are gone. Per-type
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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sound/asound.h>
#include <sys/socket.h>
#include <time.h>

/* ⓘ Declared here rather than by including ctm_state.h, which carries the
 * app half's whole state. ⭐ Defined in ctm_state.c -- see the /tmp note on
 * the declaration there. read_text_file is the one sysfs reader: this file had
 * its own copy until 2026-10-23, which failed an empty file where the other
 * gave an empty string. */
const char *ctm_log_path(const char *name);
FILE *ctm_log_open(const char *name, const char *mode);
int read_text_file(const char *path, char *out, size_t out_len);
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
/* ⓘ The TV's toolchain carries older input headers (switch_held_now). */
#ifndef INPUT_PROP_ACCELEROMETER
#define INPUT_PROP_ACCELEROMETER 0x06
#endif
#ifndef HIDIOCGRAWPHYS
#define HIDIOCGRAWPHYS(len) _IOC(_IOC_READ, 'H', 0x05, len)
#endif

/* Can a node be read as HID, and which input nodes are a device's own. */
#include "hid_node_checks.inl"
#include "device_identity.inl"
#include "kbd_chord.inl"
#include "pointer_layout.inl"
#include "pad_blank.inl"
#include "switch_hold.inl"

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
#define DS5_IDX_MIC_VOLUME             7
#define DS5_IDX_AUDIO_CONTROL          8

/* valid_flag0 -- claim bits for the settings we care about */
#define DS5_F0_ALLOW_SPEAKER_VOLUME    0x20
/* ⭐ Bit 6. DERIVED, not guessed: the claim bits run in the same order as the
 * payload bytes they govern. `spk_route` used 0x90 = headphone volume (bit 4) +
 * audio control (bit 7); `spk_vol` used 0xa0 = speaker volume (bit 5) + audio
 * control. Bytes 5/6/7/8 are headphone/speaker/mic/audio-control, so mic volume
 * is bit 6.
 *
 * ⛔ BITS 0 AND 1 MUST STAY CLEAR -- they govern the rumble path, and claiming a
 * field without setting it applies ZERO. ⓘ Claiming mute in panel 1 rather than
 * panel 2 was the same class of mistake and cost a session. */
#define DS5_F0_ALLOW_MIC_VOLUME        0x40
#define DS5_F0_ALLOW_AUDIO_CONTROL     0x80

/* audio_control (byte 8) fields */
#define DS5_AUDIO_OUT_PATH_SPEAKER     0x30  /* route playback to the controller speaker */
/* ⓘ HISTORY, SUPERSEDED BY THE NOTE BELOW: ECHO CANCEL ONLY, NOISE CANCEL
 * OFF (2026-08-23). Both bits are written now, on both transports.
 *
 * ⓘ Byte 8 packs two independent switches: bit 2 echo cancel, bit 3 noise
 * cancel. They were set together because games send 0x3c, and only ONE of them
 * was ever justified.
 *
 * ⭐ ECHO CANCEL (bit 2) IS PROVEN NECESSARY. The controller suppresses its own
 * speaker without it -- feedback protection, the mic sits centimetres away.
 * Measured on a C1: attenuated -> 80 dB -> 94 dB with this bit alone, volume
 * held constant.
 *
 * ⛔ NOISE CANCEL (bit 3) WAS NEVER TESTED, and it is beamforming: on a
 * MICROPHONE ARRAY it combines the capsules to isolate one voice and suppress
 * the rest. ⚠️ MEASURED 2026-08-23 on the Monitor -- through our capture path
 * ch0 peaked at 4-31 while ch1 reached 1369, and `arecord` on the same
 * controller minutes later gave ch0 740 and ch1 1939. `arecord` sends no such
 * report. ➡️ One channel effectively cancelled away, and the other quiet.
 *
 * ⚠️ IF THE SPEAKER ATTENUATES AGAIN, PUT BIT 3 BACK AND SAY SO HERE -- that
 * would mean the two bits are not independent after all, which the staged
 * measurement suggests they are but never proved. */
/* ⛔⛔ BOTH BITS, ON BOTH TRANSPORTS. Corrected 2026-08-24, twice in one day.
 *
 * ⚠️ NOISE CANCEL IS NOT OPTIONAL FOR THE SPEAKER. Dropping it fixed the wired
 * microphone's dead channel -- that measurement stands, it came from peak_ch
 * numbers in the capture log -- but the wired speaker attenuates without it,
 * exactly as Bluetooth does.
 *
 * ⛔⛔ WHY IT LOOKED SAFE: THE RE-CHECK WAS DONE BY LAUNCHING A GAME. The
 * protocol has said since 2026-07-22 that a game writes correct settings and
 * MASKS the fault, and that is how the original attenuation hid for months.
 * ➡️ The wired speaker was never actually nominal on 0x04; the game was
 * repairing it.
 *
 * ⭐ Bisected properly, on YouTube: build 260 nominal, 261 attenuated, on both
 * transports. 261 is the build that dropped bit 3.
 *
 * ⭐⭐ AND A CHEAPER DIAGNOSTIC WAS FOUND DOING IT (rhoquinn8217, 2026-08-24):
 * THE CONFIRMATION TONE PREDICTS IT. It is generated by our own code, plays on
 * every bridge, and is audibly attenuated or nominal before anything else is
 * tried. ➡️ No need to open a browser to know.
 *
 * ⚠️ SO THIS IS A TRADE, NOT A FIX. Bit 3 on gives a loud speaker and one
 * microphone channel at a noise floor; bit 3 off gives both channels and a
 * quiet speaker. **The speaker wins, because it is the advertised feature and a
 * mono microphone is still a usable microphone.** ➡️ The dead channel returns
 * to the backlog. */
#define DS5_AUDIO_ECHO_NOISE_CANCEL    0x0c  /* bits 2 AND 3 */
/* ⭐⭐ MICROPHONE VOLUME -- byte 7, and this project has NEVER SET IT (2026-08-23).
 *
 * ⛔ THE MEASUREMENT: a recording made through the bridge peaked at -26.2 dBFS
 * with an RMS of -49.8. Normal speech sits near -20 peak and -28 RMS, so the
 * captured voice was about 20 dB down -- a tenth of the level it should be, and
 * it sounded distant to everyone who heard it.
 *
 * ⚠️ Flag panel 1 claimed only speaker volume and audio control, so byte 7 was
 * never touched and the controller kept whatever it powered up with.
 *
 * ⭐ THIS IS THE SAME FAULT THE SPEAKER HAD. Its default was also too quiet to
 * be usable, and setting it to 0x64 is what fixed it. ⓘ The speaker's usable
 * range was found to be roughly 0x3d..0x64; the microphone's is NOT documented
 * anywhere we have, so this is the maximum and a starting point rather than a
 * tuned value.
 *
 * ➡️ IF IT WORKS, THIS BECOMES A SETTING, not a constant -- Windows-side beside
 * speaker_volume and haptics_gain, because gain is per-controller and two
 * people on two pads will not want the same. */
#define DS5_MIC_VOLUME_MAX             0x24

#define DS5_SPEAKER_VOLUME_MAX         0x64  /* 100 -- what games, the kernel driver and
                                              * dualsensectl all use */

/* The Bluetooth output reports' checksum, the one copy of it (code review,
 * 2026-10-05). Before the microphone safety, which signs its packet with it. */
#include "bt_sign.inl"
/* How every signal's light is shaped, and the bridged-and-waiting breath. */
#include "signal_shape.inl"

/* Microphone safety. ⚠️ INCLUDED HERE, NEAR THE TOP, DELIBERATELY -- the input
 * relay calls into it and an include further down would be after its own
 * caller. It needs nothing from this file but the checksum just above and the
 * standard headers. Fork-only. */
#include "ctm_mic_safety.inl"

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
    int hid_fd;
    int alsa_fd;   /* wired DS5/Edge: the controller's own playback device */
    int alsa_consec_fail;      /* audio writes that failed outright, in a row */
    int alsa_retry_streak;     /* audio writes that needed any retry, in a row */
    unsigned long long alsa_writes;   /* audio chunks written, for the log */
    /* ⓘ The speaker's handle: held to write to it, close it or reopen it. */
    pthread_mutex_t alsa_mutex;
    /* Game audio chunks dropped while a signal had the speaker, for the log.
     * The session thread's alone. */
    unsigned alsa_busy_drops;
    /* The audio settings currently in force on this controller. Start as the
     * defaults and are updated by any host report that claims them, so a
     * reopen restores what the host asked for rather than what we assumed. */
    uint8_t audio_spk_vol;
    uint8_t audio_control;

    pthread_t mic_cap_thread;
    int mic_cap_started;
    /* ⭐ Has this controller's decoder been given a stream yet? Per CONTROLLER,
     * not per app run -- see the prime in ctm_bt_signal.inl. */
    int btsig_primed;
    /* Which sound card is genuinely this controller's, worked out by muting
     * it and seeing which card falls silent. -1 until answered, and -1 stays
     * if the answer could not be trusted -- see ctm_cardmatch.inl. */
    int matched_card;
    /* Set by the matcher: no valid note existed for this node when it ran,
     * which is the first bridge since the cable went in. The first audio
     * stream after that is dead for the speaker; see ctm_feedback.inl. */
    int card_fresh;
    int wake_pipe[2];

    pthread_t session_thread;
    int session_started;
    /* ⭐⭐ A SIGNAL THREAD, and plug-out's wait for it -- see
     * controller_signal_begin. Guarded by signal_mutex, which nothing else
     * takes. ⓘ The DualSense's connected signal claims it too, since
     * 2026-09-21 (feedback_play_connected). */
    pthread_mutex_t signal_mutex;
    pthread_cond_t signal_idle;
    int signal_running;      /* a signal thread holds the controller */
    int signal_closed;       /* plug-out has begun: none may start, a running one stops */
    int signal_motors_done;  /* the running signal has let the motors go (its pulse is over) */
    uint32_t signal_kept;    /* the host's latest value, for the signal to give back */
    pthread_t input_thread;
    int input_thread_started;
    volatile int stop;
    /* A send on this connection failed, so the session loop ends the
     * connection (code review, 2026-10-05): input_link_lost(). Cleared when
     * a connection starts. */
    volatile int link_send_failed;
    /* The listener has answered this connection's greeting: its HOST_CONFIG
     * in handshake(), or for a relay type, which does not wait for that, any
     * first message. session_main's trouble clock stops on it
     * (link_trouble.inl). Cleared before each connection's session. */
    volatile int host_answered;
    /* The bridged-and-waiting light: ready_light_tick(). */
    volatile int ready_light_taken;   /* the PC has set the light: stop for good */
    uint64_t ready_light_t0_us;       /* when it started breathing; 0 = not yet */
    uint64_t ready_light_next_us;     /* when its next step is due */
    int ready_light_failed;           /* writes in a row that did not go */
    /* A colour the PC set while the bridge's own signal painted the pad, put
     * back when the signal ends: ready_light_note_host. */
    int ready_light_owed;
    uint8_t ready_light_owed_rgb[3];

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
    /* What a bridge keeps released until it is let go (switch_hold.inl): the
     * layout, built once a bridge by session_main, and what was held. Read
     * and written by the input thread after that. */
    swh_layout_t swh_layout;
    swh_state_t swh;
    bool swh_told;
    /* What tells this device's input nodes from anyone else's -- see
     * hid_node_checks.inl. ⛔ kernel_uniq is copied at create, BEFORE the
     * pairing probe can fill dev.mac: an input node carries the kernel's
     * serial, never the one we read from the pad. */
    char kernel_uniq[64];
    char hid_phys[64];
    /* An input-node type grabs the fd it reads, not a second one. */
    int self_grabbed;
    /* The report descriptor's top level is a joystick, gamepad or multi-axis
     * controller. Set by open_hid; see grab_skips_gamepads. */
    int is_gamepad;
    /* ⭐ A bridged keyboard's layout, for the app's shortcuts
     * (kbd_chord.inl); kbd.present only for a hidraw keyboard it understands.
     * kbd_chord_key: the shortcut's letter held right now, 0 for none. */
    kbd_layout_t kbd;
    char kbd_chord_key;
    /* ⭐ A bridged mouse's report (pointer_layout.inl); ptr.present only for a
     * Mouse or Pointer collection. handed_back: the TV's overlay holds input,
     * so the keyboard or mouse is ungrabbed and reaches the TV
     * (handback_sync_hold). ptr_used: its first report saying something has
     * gone to the host since the bridge, and the app was told. */
    ptr_layout_t ptr;
    int handed_back;
    int ptr_used;
    /* ⭐ The right stick's Y and its click while the overlay holds input, and
     * the hold they were read in (held_stick_note, held_stick.inl). */
    volatile int held_stick_y;
    volatile int held_stick_click;
    volatile unsigned held_stick_gen;
    /* ⭐ The touchpad while the overlay holds input, one word so it is read
     * whole, and the hold it was read in (held_touch_note, held_touch.inl). */
    volatile uint32_t held_touch_word;
    volatile unsigned held_touch_gen;
    /* ⭐ How to blank a gamepad's reports while the TV's overlay holds input,
     * for a type with no blank of its own (pad_blank.inl). present only when its
     * descriptor gave something to blank, or it is a Pro Controller. ⓘ Built
     * for every gamepad, because its rest report also stands in for a still
     * pad that has sent nothing yet (pad_keep). */
    pad_blank_plan_t pad_blank;
    /* ⭐ A still pad's last report, for ops->keepalive_ms when the type has no
     * current_report of its own (pad_blank.inl). */
    pad_keep_t pad_keep;
    /* When an input report last went to the host, for ops->keepalive_ms. */
    uint64_t last_input_us;
    /* Per-controller state owned by the type; freed at destroy. */
    void *type_ctx;

    FILE *log;

    /* Live status for the app: the flags below are guarded by status_mutex. */
    pthread_mutex_t status_mutex;
    volatile int st_connected;
    /* ⭐ T-127: the reconnect loop gave up. Set once, read by the UI tick. */
    volatile int st_host_gone;
    /* ⭐ The device itself went away mid-session. Set once, read by the UI
     * tick, which releases the session: see device_gone in ctm_controller.h. */
    volatile int st_device_gone;

    uint8_t *enum_payload;           /* composite: forwarded enumeration (CTMB_MSG_ENUM) */
    int enum_payload_len;
    comp_iface_t comp[15];           /* composite: the non-primary HID interfaces */
    int comp_count;
    uint8_t primary_in_ep;           /* the primary hidraw's IN endpoint (input tag) */
    uint8_t primary_out_ep;          /* the primary hidraw's OUT endpoint (output route) */
    uint8_t primary_iface;           /* the primary hidraw's USB interface number */
    volatile int comp_run;           /* gates the sibling reader threads */
};

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
    FILE *f = ctm_log_open("alsa_debug.log", "a");
    if (!f) return;
    fprintf(f, "%s %.3f ", tag, (double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* The speaker's hardware parameters, set on an open playback handle: four
 * channels of S16_LE at 48000, and a buffer capped at 4800 frames with the
 * unbounded request as the fallback. Returns what HW_PARAMS returned, its
 * errno in `*out_errno`, and what the kernel chose in `*out`.
 *
 * ⛔⛔ ONE COPY FOR BOTH SPEAKER OPENERS (code review, 2026-10-05). The
 * opener that moves a speaker to its pad's own card (ctm_cardmatch.inl) set
 * these through a hand copy made before the cap, so a speaker moved there
 * got the second-long buffer back. tests/test_speaker_buffer_cap.c fails if
 * a playback open sets them any other way. */
static int ds5_playback_hw_params(int fd, int card, struct snd_pcm_hw_params *out,
                                  int *out_errno)
{
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
     * host's chunks.
     *
     * ⛔⛔ AND A CEILING, BECAUSE THE BUFFER *IS* THE LATENCY. This had no
     * upper bound, and the kernel is free to take what suits the hardware:
     * measured on the U5s (webOS 26, kernel 6.12, xhci) it chose
     * **49152 frames = 1.024 SECONDS**, which is exactly the delay
     * rhoquinn8217 heard on the pad's speaker while the monitor's own audio
     * stayed on time.
     * ⛔ THIS ONCE SAID "the C1 does not do this". IT DOES. Measured on the
     * C1 2026-09-14 with the cap removed (build 313): buffer_frames=49152,
     * the same second-long buffer. The fault was never device-specific; it
     * was an unbounded parameter on both, and the cap is needed on both.
     *
     * ⚠️ 4800 frames is 100 ms: ten times the host's 480-frame chunks and
     * five times the floor, so there is still room for jitter. ⛔ A tighter
     * cap risks underruns, which are worse than lag -- see the self-heal
     * this file already carries for them.
     *
     * ➡️ AND IT FALLS BACK. If the hardware cannot satisfy the cap, HW_PARAMS
     * fails and we would open no audio at all -- a SILENT speaker, worse than
     * a late one. So the unbounded request is retried on failure. */
    hw.intervals[9].min = 960;
    hw.intervals[9].max = 4800;
    hw.intervals[9].integer = 0;
    /* Kept before the ioctl: HW_PARAMS rewrites `hw` with what it chose. */
    struct snd_pcm_hw_params hw_unbounded = hw;
    hw_unbounded.intervals[9].max = 0xFFFFFFFFU;
    int hw_rc = ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw);
    int hw_errno = (hw_rc < 0) ? errno : 0;
    if (hw_rc < 0) {
        hw = hw_unbounded;
        hw_rc = ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw);
        if (hw_rc >= 0) {
            alsa_log("[alsa-hwparams]", "card=%d buffer cap refused (errno=%d), reopened unbounded",
                     card, hw_errno);
        } else {
            hw_errno = errno;
        }
    }
    *out = hw;
    *out_errno = hw_errno;
    return hw_rc;
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
static int open_ds5_alsa_playback(const char *want_node, int prefer_card)
{
    char path[128];
    int fd = -1;
    /* When the controller's own card is known, open it directly. The scan
     * below takes the first DualSense card it can open, which with two
     * controllers is how one ends up playing through the other's speaker.
     * -1 means not known, and the scan is all there is. */
    int first = (prefer_card >= 0) ? prefer_card : 0;
    int last  = (prefer_card >= 0) ? prefer_card : 7;
    for (int card = first; card <= last; card++) {
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

        /* ⓘ Which card this scan opens, and for which node: the line that
         * makes audio heard from the wrong pad readable. The scan matches on
         * the name "DualSense" alone; the card match (ctm_cardmatch.inl) is
         * what tells two pads' cards apart. */
        alsa_log("[alsa-scan]", "card=%d wants_node=%s header=[%s]",
                 card, want_node ? want_node : "?", header);

        snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0p", card);
        fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            /* ⭐ Say why, as the microphone opener does. A card that is found
             * and then silently skipped leaves only "playback open failed",
             * which cannot tell a busy card from a bus with no room left. */
            alsa_log("[alsa-open]", "card=%d open failed errno=%d", card, errno);
            continue;
        }
        struct snd_pcm_hw_params hw;
        int hw_errno = 0;
        int hw_rc = ds5_playback_hw_params(fd, card, &hw, &hw_errno);
        int prep_rc = (hw_rc >= 0) ? ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL) : -1;
        int prep_errno = (hw_rc >= 0 && prep_rc < 0) ? errno : 0;
        if (hw_rc < 0 || prep_rc < 0) {
            /* ⭐ The errno is the finding. ENOSPC (28) means the USB bus had no
             * room left for another audio stream; depending on the kernel that
             * surfaces here or at the first write, which [alsa-write] logs.
             * EBUSY (16) means something else holds the card. */
            alsa_log("[alsa-open]", "card=%d not opened: hw_params rc=%d errno=%d, prepare rc=%d errno=%d",
                     card, hw_rc, hw_errno, prep_rc, prep_errno);
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
/* The rate the device was configured for at open, so a count of frames can be
 * read back as a duration. */
#define DS5_AUDIO_RATE     48000
/* How long past the audio's own duration to keep offering it before giving up
 * on a device that will not take it. Bounds a stall; it does not police timing. */
#define ISO_WRITE_GRACE_US 250000

static uint64_t now_us(void);

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
static void write_iso_audio_locked(ctm_controller_t *c, const uint8_t *pcm, uint32_t len)
{
    if (!c || c->alsa_fd < 0 || !pcm || len == 0) return;

    const size_t frame_bytes = DS5_AUDIO_CHANNELS * sizeof(int16_t);
    const unsigned long total = (unsigned long)(len / frame_bytes);
    if (total == 0) return;

    /* ⭐⭐ KEEP OFFERING IT UNTIL THE DEVICE HAS TAKEN ALL OF IT.
     *
     * ⛔ THE FAULT THIS FIXES: one ioctl was assumed to carry the whole buffer.
     * On a NON-BLOCKING handle it carries only what fits in the ring, reports
     * how much that was in xfer.result, and the rest was dropped without a
     * word -- the log even printed the frames ASKED FOR rather than the frames
     * taken, so a truncated write read as a clean one.
     *
     * ⛔ Invisible for as long as the buffer was whatever the kernel chose,
     * because everything fit in one go. Capping it at 4800 frames (100 ms) to
     * cure the speaker lag left the confirmation tone -- 17760 frames, 370 ms
     * -- nearly four times too big for a single write, and it was heard as one
     * clipped note where there should be two. ⭐ MEASURED on the C1 2026-09-14,
     * the same pad minutes apart: build 313 chose buffer_frames=49152 and
     * played two notes; build 333 capped at 4800 and played one.
     *
     * ⓘ The streaming path pays nothing for this. The host's chunks are about
     * 480 frames against a 4800-frame ring, so they are taken whole and the
     * loop runs exactly once, as the single write did. */
    const uint64_t deadline_us = now_us()
                               + (uint64_t)total * 1000000ull / DS5_AUDIO_RATE
                               + ISO_WRITE_GRACE_US;
    unsigned long done = 0;
    int rc = 0;
    int first_errno = 0;
    int retries = 0;

    while (done < total) {
        struct snd_xferi xfer;
        memset(&xfer, 0, sizeof(xfer));
        xfer.buf = (void *)(pcm + (size_t)done * frame_bytes);
        xfer.frames = (unsigned long)(total - done);
        rc = ioctl(c->alsa_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
        if (rc >= 0 && xfer.result > 0) {
            done += (unsigned long)xfer.result;
            continue;
        }
        /* ⓘ Taking nothing without reporting an error is the ring being full,
         * which wants exactly the wait EAGAIN asks for. */
        const int err = (rc < 0) ? errno : EAGAIN;
        if (first_errno == 0) first_errno = err;
        if (err == EPIPE) {
            ioctl(c->alsa_fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
        } else if (err == EAGAIN) {
            struct timespec ts = {0, 1000000};   /* 1 ms for buffer space */
            nanosleep(&ts, NULL);
        } else {
            break;                               /* not recoverable */
        }
        ++retries;
        if (now_us() >= deadline_us) break;
    }

    const int complete = (done == total);
    if (!complete) c->alsa_consec_fail++; else c->alsa_consec_fail = 0;
    if (first_errno != 0) c->alsa_retry_streak++; else c->alsa_retry_streak = 0;

    /* This line is where the fault counts come from -- there is no separate
     * one -- so it is written on anything that went wrong, and periodically
     * when nothing did, to prove the path is still alive. */
    c->alsa_writes++;
    if (first_errno != 0 || !complete || (c->alsa_writes % 500) == 0) {
        /* ⭐ BOTH COUNTS, because one of them was the fault. "frames" is what
         * the audio needed and "wrote" is what the device took; when they
         * differ the sound was cut short, which this line could not show while
         * it printed only what was asked for. */
        alsa_log("[alsa-write]", "n=%llu frames=%lu wrote=%lu final_rc=%d first_errno=%d retries=%d",
                 (unsigned long long)c->alsa_writes, total, done,
                 rc, first_errno, retries);
    }

    const char *heal_reason = NULL;
    if (c->alsa_consec_fail >= 20) heal_reason = "consecutive-fail";
    else if (c->alsa_retry_streak >= 5) heal_reason = "retry-streak";
    if (!heal_reason) return;

    alsa_log("[alsa-selfheal]", "reason=%s consec_fail=%d retry_streak=%d, reopening ALSA device",
             heal_reason, c->alsa_consec_fail, c->alsa_retry_streak);
    ctl_log(c, "alsa: speaker closed (self-heal reopen), fd=%d", c->alsa_fd);
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

/* ⭐⭐ ONE WRITER AT A TIME, AND NO CLOSE UNDER A WRITE (code review,
 * 2026-10-05). Game audio arrives on the session thread and a signal's tone is
 * written from its own thread, both through the function above: their chunks
 * interleaved in the device, and a self-heal on one thread closed the handle
 * under the other's write. alsa_mutex guards the handle now.
 * ⓘ Game audio does not WAIT for a tone. A chunk that finds the speaker busy
 * is dropped, as a chunk the device will not take always was, so the session
 * thread, which carries the reports, never stalls behind a tone. */
static void write_iso_audio(ctm_controller_t *c, const uint8_t *pcm, uint32_t len)
{
    if (!c) return;
    if (pthread_mutex_trylock(&c->alsa_mutex) != 0) {
        c->alsa_busy_drops++;
        return;
    }
    if (c->alsa_busy_drops > 0) {
        alsa_log("[alsa-write]", "dropped %u chunk(s) of game audio while a signal had the speaker",
                 c->alsa_busy_drops);
        c->alsa_busy_drops = 0;
    }
    write_iso_audio_locked(c, pcm, len);
    pthread_mutex_unlock(&c->alsa_mutex);
}

/* A signal's way in: it waits its turn, and then has the speaker until its
 * buffer is taken. */
static void write_iso_audio_waiting(ctm_controller_t *c, const uint8_t *pcm, uint32_t len)
{
    if (!c) return;
    pthread_mutex_lock(&c->alsa_mutex);
    write_iso_audio_locked(c, pcm, len);
    pthread_mutex_unlock(&c->alsa_mutex);
}

/* Monotonic clock in microseconds. When: pacing schedules + handshake timeout. */
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Told when a controller asks to be unplugged; the app does the work. */
static ctm_controller_unplug_cb g_unplug_cb;

void ctm_controller_set_unplug_cb(ctm_controller_unplug_cb cb)
{
    g_unplug_cb = cb;
}

/* Told when a bridged keyboard presses the overlay's shortcut; see
 * controller_set_overlay_cb in ctm_controller.h. */
static overlay_request_cb g_overlay_cb;

void controller_set_overlay_cb(overlay_request_cb cb)
{
    g_overlay_cb = cb;
}

/* Told when a bridged keyboard presses one of the app's other shortcuts, and
 * when a bridged mouse is first used; see ctm_controller.h. */
static shortcut_request_cb g_shortcut_cb;
static pointer_used_cb g_pointer_used_cb;

void controller_set_shortcut_cb(shortcut_request_cb cb)
{
    g_shortcut_cb = cb;
}

void controller_set_pointer_used_cb(pointer_used_cb cb)
{
    g_pointer_used_cb = cb;
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
    FILE *f = ctm_log_open("ctm-gesture.log", "a");
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

/* ⭐ Is a bridged controller's input being held right now? See
 * ctm_input_set_held. ⓘ Read once per report in the relay path, so it is a
 * plain int rather than anything that could block. */
static volatile int g_input_held;
/* ⓘ Counts the holds, so a pad's right stick read during an earlier one is not
 * taken for this one's (held_stick_note). */
static volatile unsigned g_input_hold_gen;

void ctm_input_set_held(int held)
{
    const int now_held = held ? 1 : 0;
    if (now_held && !g_input_held) g_input_hold_gen++;
    g_input_held = now_held;
}

/* ⭐ Which signals are allowed, and whether the microphone is captured. Read
 * once per event rather than per report, so plain ints are enough. */
static volatile int g_sig_light = 1, g_sig_rumble = 1, g_sig_tone = 1;
static volatile int g_mic_capture = 1;

/* ⭐⭐ THE MINIMUM GAP BETWEEN TWO TONES ON ONE PAD, in ms.
 *
 * ⚠️ Why it exists: once two tones can no longer overlap, the second one starts
 * about 0.1 s after the first one's audio stopped, and about a fifth of signals
 * fired a second apart still lost a note on an OLED83B4PUA -- with
 * `sent=267 failed=0` every time, so the loss is inside the pad. The hypothesis
 * this knob tests is that the decoder needs a moment after a stream ENDS before
 * it will start another.
 * ⓘ Settable from the control port (`set settle <ms>`) so the value can be
 * swept without a build each time, which is what keeps a pad's battery and a
 * room's noise pinned across the comparison. ⓘ It began as a post-teardown
 * settle for the handback and was measured unnecessary there (15 of 15 at 0), so
 * it moved to where the evidence pointed. */
static int g_ds4_tone_gap_ms = 0;

void ctm_tone_gap_set_ms(int ms)
{
    g_ds4_tone_gap_ms = (ms < 0) ? 0 : (ms > 3000 ? 3000 : ms);
}

int ctm_tone_gap_ms(void)
{
    return g_ds4_tone_gap_ms;
}

void ctm_signals_set_enabled(int light, int rumble, int tone)
{
    g_sig_light  = light  ? 1 : 0;
    g_sig_rumble = rumble ? 1 : 0;
    g_sig_tone   = tone   ? 1 : 0;
    /* ⭐ SAYS WHAT IT WAS TOLD. ⛔ Green kept appearing with the lightbar switch
     * off, and there was no way to tell "the setting never arrived" from
     * "something else paints it". ⓘ Once per call, not per report. */
    FILE *lf = ctm_log_open("ctm-signal.log", "a");
    if (lf) {
        fprintf(lf, "signals: light=%d rumble=%d tone=%d\n",
                g_sig_light, g_sig_rumble, g_sig_tone);
        fclose(lf);
    }
}

void ctm_mic_capture_set_enabled(int on)
{
    g_mic_capture = on ? 1 : 0;
    FILE *lf = ctm_log_open("ctm-signal.log", "a");
    if (lf) {
        fprintf(lf, "mic capture: %d\n", g_mic_capture);
        fclose(lf);
    }
}

static int ctm_sig_light_on(void)  { return g_sig_light; }
static int ctm_sig_rumble_on(void) { return g_sig_rumble; }
int signals_rumble_on(void) { return g_sig_rumble; }
int signals_light_on(void)  { return g_sig_light; }
static int ctm_sig_tone_on(void)   { return g_sig_tone; }

static int ctm_input_is_held(void)
{
    return g_input_held;
}

/* ⭐ A bridged pad's right stick and its click while the overlay holds input,
 * kept with the hold they were read in; ctm_controller_get_status hands them
 * out only for the hold in force. When: the input thread, on each report while
 * input is held. ⓘ Plain ints, read by the app's thread as advisory as the
 * status counters are. */
static void held_stick_note(ctm_controller_t *c, const uint8_t *buf, size_t n)
{
    int y = 0;
    bool click = false;
    if (!c->ops->read_right_stick(buf, n, &y, &click)) return;
    c->held_stick_y = y;
    c->held_stick_click = click ? 1 : 0;
    c->held_stick_gen = g_input_hold_gen;
}

/* ⭐ A bridged pad's touchpad while the overlay holds input, for the TV's
 * pointer, as the right stick is above: kept with the hold it was read in. */
static void held_touch_note(ctm_controller_t *c, const uint8_t *buf, size_t n)
{
    uint32_t word = 0;
    if (!c->ops->read_touch(buf, n, &word)) return;
    c->held_touch_word = word;
    c->held_touch_gen = g_input_hold_gen;
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

/* Per-controller log: writes to its MAC-named file (if open) and stderr; a line
 * with no controller goes to the gesture log. When: throughout a controller's
 * lifetime (any of its threads). */
void ctl_log(ctm_controller_t *c, const char *fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    /* NULL-SAFE, AND IT WAS NOT. Every line below dereferenced `c`, so a
     * caller holding no controller could not log at all without crashing --
     * which is exactly why the Bluetooth refusal tone wrote a line to stderr
     * by hand and reached neither a log file nor the sink. A refusal is the
     * one signal worth being certain of, and it was the one nothing recorded.
     * (2026-09-21) */
    const char *kind = (c && c->ops) ? c->ops->kind : "ctl";

    /* ⛔⛔ TIMESTAMPED, AT LAST. Flagged on 2026-07-31 as the obstacle to the
     * hang investigation -- "the TV log has no wall-clock time, so duration and
     * ordering across a gap cannot be read from it" -- and still true on
     * 2026-08-18, when a Bluetooth bridge took eight seconds against a cable's
     * two and this file could not say where they went.
     *
     * ⭐ MILLISECONDS, not seconds. Seconds are enough to see an eight-second
     * gap and useless for the sub-second ones that make it up. This is the file that covers the session's own work --
     * the HID open, the connect, the evdev grabs -- so it is the one that needs
     * the resolution.
     *
     * ⓘ Monotonic rather than wall-clock: a gap is what is being read, and a
     * clock that can step is the wrong instrument for measuring one. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const double t = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;

    fprintf(stderr, "[%s] %s\n", kind, body);
    if (c && c->log) {
        fprintf(c->log, "%.3f %s\n", t, body);
        fflush(c->log);
    }
    if (!c) {
        /* NO CONTROLLER: THE LINE WOULD VANISH, SO IT GOES TO THE GESTURE LOG
         * INSTEAD.
         *
         * A caller holding a controller still gets a file, `c->log`. A caller
         * without one -- the Bluetooth refusal is the only one today -- had
         * nowhere to go: `c->log` needs a controller, and the only sink for
         * these lines was the standalone UI app's, removed from this repo on
         * 2026-10-07, so on a TV the line reached `fprintf(stderr, ...)` and
         * nothing else, and stderr is not captured into the app's log files.
         *
         * MEASURED on the rooted monitor 2026-09-22, build 398: a refusal was
         * forced by killing the listener inside the TV's 11 s probe gap. It
         * fired -- `refused on /dev/hidraw8: core sounded=1 lit=1` -- and left
         * NO btsig line in any file, with the call counter not advancing. The
         * whole point of making ctl_log NULL-safe had been to record exactly
         * that, and it recorded nothing.
         *
         * The gesture log is the right destination: it is where the app
         * already writes its own `[app] refused: ...` line for the same event,
         * so the two halves land together and in order.
         *
         * ctm_gesture_log takes a NULL controller by design and writes with
         * fprintf, never through ctl_log, so this cannot recurse. */
        ctm_gesture_log(NULL, "%s", body);
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
 * ALWAYS ON FOR A WIRED CONTROLLER. This was gated behind a marker file in
 * /tmp while it was being brought up, and the gate is gone now that capture
 * is proven on one, two and three controllers, through a game, with the
 * microphone verified at the far end.
 *
 * The gate had to go rather than merely stop being used: a /tmp file does not
 * survive a TV reboot, so a power cycle silently disabled the microphone with
 * nothing to suggest why. That cost a testing session on 2026-08-09.
 *
 * MIRRORS open_ds5_alsa_playback() above: same card scan, same hw_params
 * shape. The differences are the capture device suffix ('c' not 'p'),
 * O_RDONLY, and 2 channels not 4.
 *
 * Placed after c_send() because the reader sends from this thread. */

#define MIC_CAP_CHANNELS   2
#define MIC_CAP_RATE       48000
#define MIC_CAP_FRAMES     480          /* 10 ms, matching the host's request size */
static void mic_cap_log(const char *fmt, ...)
{
    FILE *f = ctm_log_open("mic_capture.log", "a");
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

        /* ⓘ Which card this capture scan opens, and for which node. The scan
         * matches on the name "DualSense" alone: on 2026-08-04 two pads' threads
         * read one microphone (identical levels while one pad was muted at the
         * hardware), and this line is what shows such a thing. */
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
/* Defined in ctm_cardmatch.inl, which is included further down because it
 * needs hid_write_report. Declared here so the capture thread can open the
 * card the probe identified rather than scanning for one. */
static int cardmatch_open_card(int card);

/* When the capture recovers and how long it waits first: mic_recover.inl
 * (code review, 2026-10-05). */
#include "mic_recover.inl"

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

    /* Use the card the probe identified as this controller's, and fall back
     * to the scan only when it could not answer. The scan takes the first
     * free DualSense card, which is how a controller ends up reading someone
     * else's microphone. */
    int mic_card = c->matched_card;
    int fd = -1;
    if (mic_card >= 0) {
        fd = cardmatch_open_card(mic_card);
        if (fd < 0) {
            mic_cap_log("%s card=%d identified but not openable, falling back to scan",
                        node, mic_card);
            mic_card = -1;
        } else {
            mic_cap_log("%s card=%d opened by identity, not by scan", node, mic_card);
        }
    }
    if (fd < 0) {
        fd = open_ds5_alsa_capture(&mic_card, node);
    }
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
     * Allocated per thread and freed on exit. */
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
    mic_recover_t rec = {0, 0};
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
        int read_errno = 0;
        const char *why = NULL;
        if (pfd.revents & (POLLERR | POLLNVAL | POLLHUP)) {
            /* Overrun or the device went away. Re-prepare and carry on; the
             * read below reports anything that is still wrong.
             * ⛔ COUNTED AND WAITED ON (code review, 2026-10-05). A pulled
             * cable fails every poll at once, and this went straight back to
             * poll: a core spun until the pad was released. */
            ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
            ioctl(fd, SNDRV_PCM_IOCTL_START, NULL);
            int wait_ms = 0;
            const mic_action_t act = mic_recover_error(&rec, MIC_ERR_POLL, &wait_ms);
            struct timespec ts = {0, (long)wait_ms * 1000000L};
            nanosleep(&ts, NULL);
            if (act == MIC_RETRY) {
                continue;
            }
            why = "poll error";
        }

        struct snd_xferi xfer;
        memset(&xfer, 0, sizeof(xfer));
        xfer.buf = buf;
        xfer.frames = MIC_CAP_FRAMES;
        int rc = why ? -1 : ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xfer);
        if (rc < 0 && !why) {
            read_errno = errno;
            reads_failed++;
            mic_err_t kind;
            if (read_errno == EPIPE) {
                /* overrun: the stream ran ahead of us. Re-prepare and restart. */
                ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
                ioctl(fd, SNDRV_PCM_IOCTL_START, NULL);
                kind = MIC_ERR_OVERRUN;
            } else if (read_errno == EAGAIN) {
                kind = MIC_ERR_AGAIN;
            } else {
                /* Anything else means the device itself is gone or broken.
                 * This used to log and exit, which left the microphone dead
                 * for the rest of the session with nothing said about it --
                 * while the PLAYBACK side of the same USB audio device has
                 * recovered from exactly this for months. */
                kind = MIC_ERR_GONE;
            }

            /* Two triggers, mirroring playback: failing outright, or needing
             * recovery over and over. The second fires before anything is
             * audible, which is the point of having it. mic_recover.inl
             * holds both, and the waits. */
            int wait_ms = 0;
            const mic_action_t act = mic_recover_error(&rec, kind, &wait_ms);
            if (wait_ms > 0) {
                struct timespec ts = {0, (long)wait_ms * 1000000L};
                nanosleep(&ts, NULL);
            }
            if (act == MIC_RETRY) {
                continue;
            }
            why = (kind == MIC_ERR_GONE) ? "read error"
                : (rec.consec_fail >= MIC_RECOVER_FAILS) ? "consecutive-fail" : "retry-streak";
        }
        if (why) {
            /* ⛔⛔ REOPEN THIS PAD'S OWN CARD, NEVER A SCAN (code review,
             * 2026-10-05). This scanned for the first free DualSense card,
             * and with a second DualSense on a cable and not bridged, that is
             * the other pad's microphone, sent to the PC as this one's. The
             * card the capture was using is this pad's; if it will not open,
             * the pad has gone, and the capture stops. */
            mic_cap_log("%s card=%d self-heal: %s errno=%d consec=%d retry=%d, reopening",
                        node, mic_card, why, read_errno, rec.consec_fail, rec.retry_streak);
            close(fd);
            fd = mic_card >= 0 ? cardmatch_open_card(mic_card) : -1;
            if (fd < 0) {
                mic_cap_log("%s self-heal: reopen of card=%d FAILED, thread stopping", node, mic_card);
                break;
            }
            if (ioctl(fd, SNDRV_PCM_IOCTL_START, NULL) < 0) {
                mic_cap_log("%s self-heal: START failed errno=%d (first read may start it)",
                            node, errno);
            }
            heals++;
            mic_recover_ok(&rec);
            mic_cap_log("%s self-healed: reopened card=%d fd=%d", node, mic_card, fd);
            continue;
        }
        mic_recover_ok(&rec);
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

        /* ⭐⭐ COPY THE LIVE CAPSULE OVER THE QUIET ONE. 2026-08-24.
         *
         * ⛔ THE FAULT: with noise cancellation on -- which the SPEAKER
         * requires -- the array is beamformed and ch0 sits at a noise floor.
         * Measured on the monitor: **ch0 30-33 while ch1 reached 1074**, about
         * 30 dB apart, and at a higher microphone volume ch0 58-847 against a
         * clipping ch1.
         *
         * ⛔⛔ AND WINDOWS AVERAGES THE PAIR. So half the level is thrown away
         * before anyone hears it -- a straight 6 dB, on a signal that was
         * already quiet.
         *
         * ⭐ WHY DUPLICATING IS HONEST RATHER THAN A COVER-UP: the DualSense
         * microphone is a VOICE microphone. Stereo was never meaningful here,
         * and the two capsules exist to be beamformed, not to be listened to
         * separately. ⓘ Sony's own descriptor declares two channels, which is
         * why both are carried -- the shape stays, the content is mono.
         *
         * ⚠️ IT IS NOT A MIX. Summing the two would add the noise floor to the
         * voice and add nothing else; the quiet capsule carries no signal worth
         * having.
         *
         * ⓘ Which channel is live is decided PER BUFFER, by peak, rather than
         * assumed. Beamforming picks a direction, and nothing promises it is
         * always the same capsule -- an earlier 6.3 dB imbalance was measured
         * the other way round over Bluetooth.
         *
         * ➡️ If noise cancellation is ever switched off again, both capsules go
         * live and this becomes a no-op that copies the louder onto the
         * quieter, which is harmless. */
        {
            int16_t peak0 = 0, peak1 = 0;
            for (long i = 0; i < got; i++) {
                int16_t a0 = buf[i * MIC_CAP_CHANNELS + 0];
                int16_t a1 = buf[i * MIC_CAP_CHANNELS + 1];
                if (a0 < 0) a0 = (int16_t)-a0;
                if (a1 < 0) a1 = (int16_t)-a1;
                if (a0 > peak0) peak0 = a0;
                if (a1 > peak1) peak1 = a1;
            }
            /* ⓘ Only when one is clearly the quiet one -- a factor of four,
             * about 12 dB. Two genuinely live capsules are left alone. */
            /* ⚠️ Compared as int, not int16_t. peak * 4 overflows a 16-bit
             * type above about 8000 and wraps negative, which would make the
             * test fire on a loud, balanced pair. */
            if ((int)peak1 > (int)peak0 * 4) {
                for (long i = 0; i < got; i++)
                    buf[i * MIC_CAP_CHANNELS + 0] = buf[i * MIC_CAP_CHANNELS + 1];
            } else if ((int)peak0 > (int)peak1 * 4) {
                for (long i = 0; i < got; i++)
                    buf[i * MIC_CAP_CHANNELS + 1] = buf[i * MIC_CAP_CHANNELS + 0];
            }
        }

        /* Send it to the host. Audio is droppable: a failed send is a brief
         * gap in what Windows hears, never a reason to stop reading or to
         * tear anything down. The transport takes its own lock, so sending
         * from this thread cannot interleave with the input reader. */
        {
            size_t bytes = (size_t)got * MIC_CAP_CHANNELS * sizeof(int16_t);
            /* ⛔⛔ ONLY ONCE THE CONNECTION'S OPENING EXCHANGE IS DONE (code
             * review, 2026-10-05). Audio sent before HELLO lands where the
             * listener expects a hello, and it closes the connection. This
             * asked whether the link was CONNECTED, which a reconnect makes
             * true before its hello, and this thread runs across reconnects:
             * a cabled DualSense's reconnect was closed by its own
             * microphone, redialled, and closed again until the give-up. The
             * transport now holds audio back until the session marks the
             * exchange done, and stops it before a connection ends. A chunk
             * it holds back (1) is dropped, as a failed one is. */
            if (bytes > 0) {
                const int rc = ctm_transport_send_when_ready(&c->xport, CTMB_MSG_MIC_AUDIO,
                                                             CTMB_FLAG_OK, 0, buf, bytes);
                if (rc < 0) {
                    sends_failed++;
                } else if (rc == 0) {
                    sends_ok++;
                }
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
        /* Once a minute (2026-10-23): once a second, this one line grew
         * mic_capture.log by about half a megabyte an hour of play, and the
         * microphone is on by default. The counts are totals, so a minute's
         * line loses nothing; the peaks are the minute's. */
        if (now - last_report >= 60) {
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
    /* ⭐ The user's switch, checked before anything is opened. ⓘ Capture used to
     * start on every session without being asked. */
    if (!g_mic_capture) {
        if (c) ctl_log(c, "mic: capture not started -- switched off in settings");
        return;
    }
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
/* ⛔⛔ THE FOUR SECONDS BETWEEN A WIRED BRIDGE AND A BLUETOOTH ONE.
 *
 * Measured 2026-08-18 with every layer of signalling gated off (T-120 step 1):
 * a bare panel-button bridge took ~2s on a cable and ~8s over Bluetooth. Same
 * code, same button, same gates -- and this is the ONLY transport-dependent
 * call in the open path.
 *
 * The app makes the same request when a controller first appears and it was
 * timed there: 4.99s over Bluetooth against 17ms on a cable. Here it runs
 * inside open_hid, on the session thread, immediately after the bridge is
 * handed over -- which is where the interface freezes.
 *
 * ⭐ It cannot be skipped. Where a controller binds to hid-generic nothing else
 * makes this request, and without it the controller stays in its reduced
 * ten-byte report and no input arrives at all. So it is still asked -- from a
 * thread of its own, so the session opens at once and the answer, or the
 * timeout, lands whenever it lands.
 *
 * ⚠️ The fd is dup'd: the session may close its own before the request
 * returns, and the controller does not care which descriptor asked. */
static void *request_full_bt_mode_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    uint8_t feature[64];
    memset(feature, 0, sizeof(feature));
    feature[0] = 0x05;
    if (ioctl(fd, HIDIOCGFEATURE(sizeof(feature)), feature) < 0) {
        fprintf(stderr, "controller: feature 0x05 failed errno=%d\n", errno);
    }
    close(fd);
    return NULL;
}

static void request_full_bt_mode(int fd)
{
    int dup_fd = dup(fd);
    if (dup_fd < 0) return;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, request_full_bt_mode_thread, (void *)(intptr_t)dup_fd) != 0) {
        close(dup_fd);
    }
    pthread_attr_destroy(&attr);
}

/* Does a WIRED DualSense report its own identity? Asked 2026-08-08, answered
 * 2026-08-24 (below): it does, and the answer is used.
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
 * The reply was logged as bytes before anything parsed it: guessing at formats
 * is what produced three sessions of wrong conclusions elsewhere in this
 * project. Read the bytes first, decide what they mean second. */
static int reply_is_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (p[i]) return 0;
    return 1;
}

/* ⭐⭐⭐ THE LAYOUT IS NOW MEASURED, NOT DOCUMENTED. 2026-08-24.
 *
 * The investigation above asked whether a wired DualSense reports its own
 * identity. It does, and this is the reply, read on the monitor from two
 * different models:
 *
 *   ds5   09 ed 10 82 ef 66 7c  08 25 00  c9 40 7b e6 e6 34  00...
 *   ds5e  09 9d f6 cb 9a 3a 14  08 25 00  06 be 0b 9e a2 88  00...
 *
 * ⭐ Byte 0 is the report id echoed back. **BYTES 1-6 ARE THE CONTROLLER'S OWN
 * MAC, LITTLE-ENDIAN** -- reversed. Checked against the MACs the kernel reports
 * for the same two units over Bluetooth: 7c-66-ef-82-10-ed and
 * 14-3a-9a-cb-f6-9d. ⓘ Identical shape on both models.
 *
 * ⓘ Bytes 7-9 are 08 25 00 on both, a constant. ⚠️ Bytes 10-15 DIFFER between
 * the two and are almost certainly the paired host's address -- which is what a
 * pairing-info report is for. **Not parsed: not needed, and not verified.**
 *
 * ⛔ WHY THIS MATTERS: `dev.mac` comes from sysfs `/uniq`, which the kernel fills
 * only when `hid-playstation` binds the device. **On a cable that happens on
 * some televisions and not others** -- measured 2026-08-24: the B4 populates it,
 * the monitor does not. ➡️ **So a wired controller had no identity on half the
 * fleet, and the listener saw the constant `CTMUSBIP` instead.**
 *
 * ⭐ Reading 0x09 ourselves does not care which driver bound. */
static void ds_mac_from_pairing_info(const uint8_t *reply, char *out, size_t out_len)
{
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             reply[6], reply[5], reply[4], reply[3], reply[2], reply[1]);
}

/* Take a MAC the pad reported about itself as its identity. `source` names the
 * report, for the log. ⓘ One copy of these rules for every pad that can say its
 * own MAC -- a DualSense's 0x09 below, and a type's read_pad_mac (a DS4's 0x12);
 * the log lines are the ones the DualSense's probe always wrote. */
static void adopt_pad_mac(ctm_controller_t *c, const char *pad, const char *source)
{
    /* ⭐⭐ THE PAD'S OWN MAC IS WHAT THE HOST LINKS A CONFIG ON, even where the
     * kernel supplied something else (rhoquinn8217, 2026-09-13). ⛔ Through a
     * DS5dongle on the C1 the kernel's uniq was the DONGLE's serial, so a config
     * linked on it followed the dongle to whichever pad was plugged in next.
     * ⓘ On the U5s the kernel reads 0x09 itself, and the two agree. */
    if (c->dev.serial[0] != '\0' && !identity_same(c->dev.serial, pad)) {
        ctl_log(c, "identity: MAC %s from %s replaces %s as this pad's identity",
                pad, source, c->dev.serial);
    }
    snprintf(c->dev.serial, sizeof(c->dev.serial), "%s", pad);

    /* ⭐ dev.mac itself is only FILLED, never replaced. Where the kernel supplied
     * a value it stays -- the log file is named from it, and replacing it would
     * make this change capable of breaking a set that already worked. */
    if (c->dev.mac[0] != '\0') return;
    snprintf(c->dev.mac, sizeof(c->dev.mac), "%s", pad);
    ctl_log(c, "identity: MAC %s taken from %s (the kernel gave none)",
            c->dev.mac, source);
}

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

    /* ⚠️ A REPLY OF ALL ZEROES IS NOT AN ANSWER. An unpaired controller, or one
     * whose reply we misread, would otherwise be given the identity
     * 00:00:00:00:00:00 -- and EVERY such controller would share it, which is
     * exactly the silent-collision the listener's config store refuses to
     * allow. Better to leave it empty and let the host use its constant. */
    if (reply_is_zero(feature + 1, 6)) {
        ctl_log(c, "identity: 0x09 answered with zeroes -- leaving the MAC empty");
        return;
    }

    char pad[24];
    ds_mac_from_pairing_info(feature, pad, sizeof(pad));
    adopt_pad_mac(c, pad, "feature 0x09");
}

/* Append the Sony BT HID output-report CRC32 (seed 0xa2) into the trailing 4
 * bytes. When: a DS patch_output hook, after rewriting a report. The checksum
 * itself is bt_sign.inl's. */
void ctm_bt_sign_output(uint8_t *data, size_t len)
{
    bt_sign_output(data, len);
}

/* A report descriptor into the controller's log, 32 bytes a line, so a pad seen
 * in the field can join tests/test_pad_blank.c as it really is. When: opening a
 * gamepad that gets a blank built from its descriptor. */
static void log_descriptor(ctm_controller_t *c, const uint8_t *d, uint32_t n)
{
    char line[3 * 32 + 1];
    for (uint32_t i = 0; i < n; i += 32) {
        size_t at = 0;
        line[0] = '\0';
        for (uint32_t k = i; k < n && k < i + 32; ++k) {
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x ", d[k]);
        }
        ctl_log(c, "descriptor %03x: %s", (unsigned)i, line);
    }
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
    /* ⓘ Truncation leaves no terminator, hence the zeroed buffer and one byte
     * held back. Empty is fine: the grab then matches by serial or vendor. */
    memset(c->hid_phys, 0, sizeof(c->hid_phys));
    if (ioctl(fd, HIDIOCGRAWPHYS(sizeof(c->hid_phys) - 1), c->hid_phys) < 0) {
        c->hid_phys[0] = '\0';
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

    /* ⛔⛔ BEFORE THE SERIAL IS COPIED, NOT AFTER. This fills dev.mac on a wired
     * controller whose kernel driver supplied none, and the very next line is
     * the only place that value reaches the host. ⚠️ It used to run at the end
     * of this function, purely as an investigation -- moving it is the whole
     * change on this side. */
    /* ⛔ DualSense only. Asked of every USB device until 2026-09-13, and a
     * device that does not answer 0x09 makes the kernel wait out its 5 s
     * control-transfer timeout: measured on a keyboard dongle, errno=110. */
    if (c->ops->speaks_ds5 && strcmp(ctm_controller_bus(c), "USB") == 0) probe_pairing_info(c, fd);
    /* ⭐ And a type that reads its pad's MAC its own way (a cabled DS4, whose
     * kernel on the C1 leaves uniq empty). ⓘ Only a type that sets the hook is
     * asked, for the same 5 s reason. */
    else if (c->ops->read_pad_mac && strcmp(ctm_controller_bus(c), "USB") == 0) {
        char pad[24];
        if (c->ops->read_pad_mac(c, fd, pad, sizeof(pad))) adopt_pad_mac(c, pad, "the pad's pairing report");
    }

    /* ⭐⭐ THE IDENTITY THE HOST LINKS A CONFIG ON (device_identity.inl, decided
     * 2026-09-13). A DualSense is its own MAC: on a cable the probe above has
     * just put it in dev.serial, and over Bluetooth uniq is the MAC. ⛔ It is
     * never given a dongle's serial in its place -- nothing rather than the
     * wrong pad. Anything else is its serial, and never a blank or all-zeros one. */
    {
        char identity[64];
        identity_pick_serial(c->dev.serial, c->dev.mac, identity, sizeof(identity));
        if (c->ops->speaks_ds5 && !identity_mac_shaped(identity)) identity[0] = '\0';
        snprintf(caps->serial, sizeof(caps->serial), "%s", identity);
        ctl_log(c, "identity: the host is told %s", identity[0] ? identity : "nothing (no usable one)");
    }
    snprintf(caps->manufacturer, sizeof(caps->manufacturer), "hidraw");
    if (ioctl(fd, HIDIOCGRAWNAME(sizeof(caps->product) - 1), caps->product) < 0 ||
        caps->product[0] == '\0') {
        snprintf(caps->product, sizeof(caps->product), "hidraw");
    }

    memset(&c->pad_blank, 0, sizeof(c->pad_blank));
    memset(&c->pad_keep, 0, sizeof(c->pad_keep));
    memset(&c->ptr, 0, sizeof(c->ptr));
    *report_desc_len = read_report_descriptor(fd, report_desc, MAX_REPORT_DESCRIPTOR);
    if (*report_desc_len) {
        uint16_t usage_page = 0, usage = 0;
        ctm_hid_top_usage(report_desc, *report_desc_len, &usage_page, &usage, NULL);
        c->is_gamepad = usage_page == 0x01 && (usage == 0x04 || usage == 0x05 || usage == 0x08);
        /* ⭐ A keyboard gets the overlay's shortcut watched in its reports
         * (kbd_chord.inl). ⓘ Only a layout it can read: one it cannot has no
         * shortcut while bridged, and nothing else changes. */
        if (!c->is_gamepad &&
            kbd_layout_from_descriptor(report_desc, *report_desc_len, &c->kbd)) {
            ctl_log(c, "keyboard: overlay shortcut watched (report id %u, modifiers at %u, "
                       "%u keys at %u, %u bytes)",
                    (unsigned)c->kbd.report_id, (unsigned)c->kbd.mod_offset,
                    (unsigned)c->kbd.keys_count, (unsigned)c->kbd.keys_offset,
                    (unsigned)c->kbd.report_len);
        }
        /* ⭐ A mouse goes to the TV while its overlay is open, as a keyboard
         * does (handback_sync_hold, pointer_layout.inl). */
        if (!c->is_gamepad &&
            ptr_layout_from_descriptor(report_desc, *report_desc_len, &c->ptr)) {
            ctl_log(c, "mouse: the TV has it while its overlay is open (report id %u, %u bytes, %s)",
                    (unsigned)c->ptr.report_id, (unsigned)c->ptr.report_len,
                    c->ptr.relative ? "relative" : "absolute, so no report with nothing pressed");
        }
        /* ⭐ A gamepad whose type cannot blank its reports gets a blank built
         * from its descriptor, for while the TV's overlay holds input
         * (pad_blank.inl). A type with its own blank keeps it. */
        if (c->is_gamepad) {
            const uint16_t vid = (uint16_t)strtoul(c->dev.vid, NULL, 16);
            const uint16_t pid = (uint16_t)strtoul(c->dev.pid, NULL, 16);
            pad_blank_from_descriptor(report_desc, *report_desc_len, vid, pid, &c->pad_blank);
            if (!c->ops->blank_input) {
                ctl_log(c, "overlay blank: %u field(s)%s%s from a %u-byte descriptor",
                        (unsigned)c->pad_blank.count,
                        c->pad_blank.switch_layout ? ", and the Switch layout for its full reports" : "",
                        c->pad_blank.truncated ? " (more fields than fit; the rest left alone)" : "",
                        (unsigned)*report_desc_len);
                log_descriptor(c, report_desc, *report_desc_len);
            }
            if (c->ops->keepalive_ms) {
                if (c->pad_blank.rest_len) {
                    ctl_log(c, "still pad: report 0x%02x (%u bytes) sent again after %u ms "
                               "without one, nothing pressed until the pad sends its own",
                            (unsigned)c->pad_blank.rest_id, (unsigned)c->pad_blank.rest_len,
                            c->ops->keepalive_ms);
                } else {
                    ctl_log(c, "still pad: no report to send again%s",
                            c->pad_blank.switch_layout ? " (a Pro Controller never goes quiet)" : "");
                }
            }
        }
        derive_report_lengths(report_desc, *report_desc_len, caps);
        if (caps->input_report_len < 1024) caps->input_report_len = 1024;
        if (caps->output_report_len < 1024) caps->output_report_len = 1024;
    }

    if (c->ops->request_bt_mode) request_full_bt_mode(fd);
    return fd;
}

/* EVIOCGRAB the device's evdev nodes so webOS doesn't double-consume input.
 * When: at session start, for ops with grab_evdev.
 *
 * ⭐ Only THIS device's nodes: see hid_node_checks.inl for how they are told
 * apart, and why vendor and product alone were not enough. */
static void grab_matching_evdev(ctm_controller_t *c)
{
    /* ⭐ AN INPUT-NODE TYPE GRABS THE NODE IT READS, THROUGH THAT SAME FD. A
     * grab hands events only to the grabbing handle, so grabbing through a
     * second one would starve this session of its own input. ⓘ The grab still
     * does its job: SDL, joydev and the compositor stop receiving the pad. */
    if (c->ops->open_input) {
        if (c->hid_fd >= 0 && ioctl(c->hid_fd, EVIOCGRAB, 1) == 0) {
            c->self_grabbed = 1;
            ctl_log(c, "evdev: grabbed the input node it reads");
        } else {
            ctl_log(c, "evdev: grab of the input node it reads refused errno=%d", errno);
        }
        return;
    }
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return;
    struct dirent *ent;
    evdev_match_t how_matched = EVDEV_NOT_OURS;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char attr[160], vendor[32] = {0}, product[32] = {0}, phys[96] = {0}, uniq[96] = {0};
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/vendor", ent->d_name);
        if (read_text_file(attr, vendor, sizeof(vendor)) != 0) continue;
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/product", ent->d_name);
        if (read_text_file(attr, product, sizeof(product)) != 0) continue;
        /* ⓘ Either may be absent or empty; that only means it cannot decide. */
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/phys", ent->d_name);
        (void)read_text_file(attr, phys, sizeof(phys));
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/uniq", ent->d_name);
        (void)read_text_file(attr, uniq, sizeof(uniq));
        const evdev_match_t how = evdev_input_belongs(c->vid_num, c->pid_num,
                                                      c->hid_phys, c->kernel_uniq,
                                                      vendor, product, phys, uniq);
        if (how == EVDEV_NOT_OURS) continue;
        how_matched = how;
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
                ctl_log(c, "grabbed %s (%s)", dev_path, ent->d_name);
            } else {
                ctl_log(c, "grab refused for %s errno=%d", dev_path, errno);
                close(fd);
            }
        }
        closedir(input);
    }
    closedir(dir);
    /* ⭐ One line saying what decided it, so a keyboard still typing twice can
     * be told from a grab that matched nothing. */
    ctl_log(c, "evdev: %d node(s) grabbed, matched by %s (phys=\"%s\" uniq=\"%s\")",
            c->evdev_grab_count, evdev_match_name(how_matched),
            c->hid_phys, c->kernel_uniq);
}

/* ⭐ IS ANYTHING HELD ON ONE INPUT NODE RIGHT NOW? A key or button down, a
 * stick off its centre, a trigger above its rest or a hat off it, by an
 * eighth of the range. ⓘ A motion node is skipped -- gravity is not a held
 * stick -- and so are a touchpad's axes, which say where the last finger was:
 * a finger on it is BTN_TOUCH, a key. ABS_Z and ABS_RZ are a stick on some
 * pads and a trigger on others, so they count only off centre AND above rest.
 * `what` names what was found. */
static bool evdev_node_held(int fd, char *what, size_t what_len)
{
    uint8_t props[(INPUT_PROP_MAX + 8) / 8];
    memset(props, 0, sizeof(props));
    const bool have_props = ioctl(fd, EVIOCGPROP(sizeof(props)), props) >= 0;
    if (have_props && (props[INPUT_PROP_ACCELEROMETER / 8] & (1u << (INPUT_PROP_ACCELEROMETER % 8)))) {
        return false;
    }
    uint8_t keys[(KEY_MAX + 8) / 8];
    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
        for (size_t i = 0; i < sizeof(keys); ++i) {
            if (keys[i]) {
                snprintf(what, what_len, "a key or a button");
                return true;
            }
        }
    }
    if (have_props && ((props[INPUT_PROP_POINTER / 8] & (1u << (INPUT_PROP_POINTER % 8))) ||
                       (props[INPUT_PROP_BUTTONPAD / 8] & (1u << (INPUT_PROP_BUTTONPAD % 8))))) {
        return false;
    }
    uint8_t abs_bits[(ABS_MAX + 8) / 8];
    memset(abs_bits, 0, sizeof(abs_bits));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) return false;
    static const struct { int code; char kind; } axes[] = {
        { ABS_X, 's' }, { ABS_Y, 's' }, { ABS_RX, 's' }, { ABS_RY, 's' },
        { ABS_Z, 'e' }, { ABS_RZ, 'e' },
        { ABS_GAS, 't' }, { ABS_BRAKE, 't' }, { ABS_THROTTLE, 't' },
        { ABS_HAT0X, 'h' }, { ABS_HAT0Y, 'h' }, { ABS_HAT1X, 'h' }, { ABS_HAT1Y, 'h' },
    };
    for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); ++i) {
        const int code = axes[i].code;
        if (!(abs_bits[code / 8] & (1u << (code % 8)))) continue;
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(code), &ai) < 0) continue;
        const long range = (long)ai.maximum - (long)ai.minimum;
        const long eighth = range / 8;
        const bool off_centre = labs(2L * ai.value - ((long)ai.minimum + (long)ai.maximum)) > 2L * eighth;
        const bool above_rest = (long)ai.value > (long)ai.minimum + eighth;
        const char kind = axes[i].kind;
        if ((kind == 's' && off_centre) || (kind == 'e' && off_centre && above_rest) ||
            (kind == 't' && above_rest) || (kind == 'h' && ai.value != 0)) {
            snprintf(what, what_len, "%s", kind == 'h' ? "the d-pad" : kind == 't' ? "a trigger"
                                                                     : "a stick or a trigger");
            return true;
        }
    }
    return false;
}

/* ⭐ IS ANYTHING HELD ON THIS DEVICE RIGHT NOW? Asked of every one of its input
 * nodes, found as the grab finds them, for a device that reports only when
 * something changes (switch_hold.inl). ⓘ An ioctl, so a grab -- ours or
 * anyone's -- does not stand in the way. */
static bool switch_held_now(ctm_controller_t *c, char *what, size_t what_len)
{
    if (what_len) what[0] = '\0';
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return false;
    bool held = false;
    struct dirent *ent;
    while (!held && (ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char attr[160], vendor[32] = {0}, product[32] = {0}, phys[96] = {0}, uniq[96] = {0};
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/vendor", ent->d_name);
        if (read_text_file(attr, vendor, sizeof(vendor)) != 0) continue;
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/product", ent->d_name);
        if (read_text_file(attr, product, sizeof(product)) != 0) continue;
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/phys", ent->d_name);
        (void)read_text_file(attr, phys, sizeof(phys));
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/uniq", ent->d_name);
        (void)read_text_file(attr, uniq, sizeof(uniq));
        if (evdev_input_belongs(c->vid_num, c->pid_num, c->hid_phys, c->kernel_uniq,
                                vendor, product, phys, uniq) == EVDEV_NOT_OURS) {
            continue;
        }
        char input_dir[160];
        snprintf(input_dir, sizeof(input_dir), "/sys/class/input/%s", ent->d_name);
        DIR *input = opendir(input_dir);
        if (!input) continue;
        struct dirent *child;
        while (!held && (child = readdir(input)) != NULL) {
            if (strncmp(child->d_name, "event", 5) != 0) continue;
            char dev_path[64];
            snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", child->d_name);
            const int fd = open(dev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            held = evdev_node_held(fd, what, what_len);
            close(fd);
        }
        closedir(input);
    }
    closedir(dir);
    return held;
}

/* ⭐⭐ A BRIDGE IS A SWITCH (rhoquinn8217, 2026-10-09: "if you bridge while
 * hold anything down should automatically cancel every input"). What is held
 * as it starts reaches the PC released until it is let go: switch_hold.inl.
 * ⓘ Once a bridge, not at a reconnect, through which the PC keeps the same
 * device. When: session_main, after the device is open and described. */
static void switch_hold_begin(ctm_controller_t *c, const uint8_t *desc, uint32_t desc_len)
{
    if (c->ops->switch_layout) {
        c->ops->switch_layout(&c->swh_layout);
    } else if (c->kbd.present) {
        swh_layout_keyboard(&c->swh_layout, c->kbd.report_id, c->kbd.mod_offset,
                            c->kbd.keys_offset, c->kbd.keys_count);
    } else if (c->is_gamepad) {
        swh_layout_from_pad_blank(&c->swh_layout, &c->pad_blank);
    } else {
        swh_layout_buttons_from_descriptor(&c->swh_layout, desc, desc_len);
    }
    memset(&c->swh, 0, sizeof(c->swh));
    c->swh_told = false;
    if (c->swh_layout.count == 0 && !c->swh_layout.keys) {
        ctl_log(c, "switch: nothing in this device's reports is followed");
        return;
    }
    if (c->ops->reports_steadily) {
        swh_arm(&c->swh);   /* its first report is its state now */
        return;
    }
    char what[48];
    if (switch_held_now(c, what, sizeof(what))) {
        swh_arm(&c->swh);
        ctl_log(c, "switch: %s is held as the bridge starts; its first report says which", what);
    }
}

/* ⭐⭐ A BRIDGED KEYBOARD OR MOUSE GOES TO THE TV WHILE ITS OVERLAY IS OPEN, AND
 * BACK. rhoquinn8217, 2026-09-13: the shortcut "hands the keyboard back to the
 * TV while the overlay is open"; and 2026-10-11, of the mouse: "Mouse moves the
 * TV cursor in the streaming overlay even when bridged". The overlay holds
 * input (ctm_input_set_held), so: the host is sent one report with nothing
 * pressed (a keyboard's, and a relative mouse's with no movement), the evdev
 * grabs are let go so the TV reads the keys and moves its cursor, and nothing
 * more goes to the host until the hold ends and the grabs are taken again.
 * ⓘ Keyboards and mice only; the fds stay open. When: the input thread, on
 * each report and each idle poll. */
static void handback_sync_hold(ctm_controller_t *c)
{
    if (!c->kbd.present && !c->ptr.present) return;
    const int held = ctm_input_is_held();
    if (held == c->handed_back) return;
    if (held && c->kbd.present && c->kbd.report_len > 0 && c->kbd.report_len <= MAX_REPORT) {
        uint8_t released[MAX_REPORT];
        memset(released, 0, c->kbd.report_len);
        if (c->kbd.report_id) released[0] = c->kbd.report_id;
        (void)c_send(c, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, c->primary_in_ep,
                     released, c->kbd.report_len);
    }
    if (held && c->ptr.present) {
        uint8_t released[MAX_REPORT];
        const size_t len = ptr_released_report(&c->ptr, released, sizeof(released));
        if (len > 0) {
            (void)c_send(c, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, c->primary_in_ep,
                         released, len);
        }
    }
    int changed = 0;
    for (int i = 0; i < c->evdev_grab_count; ++i) {
        if (c->evdev_grabs[i].fd >= 0 &&
            ioctl(c->evdev_grabs[i].fd, EVIOCGRAB, held ? 0 : 1) == 0) {
            ++changed;
        }
    }
    c->handed_back = held;
    ctl_log(c, "%s: %s (%d grab(s) %s)",
            c->kbd.present && c->ptr.present ? "keyboard and mouse" :
            c->kbd.present ? "keyboard" : "mouse",
            held ? "handed to the TV while its overlay is open" : "taken back from the TV",
            changed, held ? "let go" : "taken again");
}

/* Un-grab + close the evdev nodes. When: each time a session ends. */
static void release_evdev_grabs(ctm_controller_t *c)
{
    if (c->self_grabbed) {
        if (c->hid_fd >= 0) (void)ioctl(c->hid_fd, EVIOCGRAB, 0);
        c->self_grabbed = 0;
    }
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

/* Ask the host to open its settings window on this device: one message with
 * no payload, on the session's own connection (ctm_bridge_protocol.h). When:
 * the TV's button for it. ⓘ Not while the session is between connections:
 * there is nothing to send on, and a request delivered late would open a
 * window nobody was waiting for. ⛔ Never a send that waits (code review,
 * 2026-10-27): the button runs on the interface thread with the app's device
 * locks held, and a PC that stopped reading would hold all of them. */
int ctm_controller_open_config(ctm_controller_t *c)
{
    if (!c) return -1;
    pthread_mutex_lock(&c->status_mutex);
    const int connected = c->st_connected;
    pthread_mutex_unlock(&c->status_mutex);
    if (!connected) return -1;
    const int rc = ctm_transport_try_send_msg(&c->xport, CTMB_MSG_OPEN_CONFIG, CTMB_FLAG_OK, 0,
                                              NULL, 0);
    ctl_log(c, "settings window: the host was asked to open it on this device%s",
            rc == 0 ? "" :
            rc == 1 ? " -- not sent: the link is full, the host is not reading it" :
                      " -- the request could not be sent");
    return rc == 0 ? 0 : -1;
}

/* --- a type's signal thread, and plug-out's wait for it --------------------
 *
 * ⛔⛔ WHY ANY OF THIS EXISTS. A signal is handed the bare controller on a
 * detached thread: it writes the node and the speaker for up to a couple of
 * seconds, while plug-out closes both and every caller frees the controller the
 * moment plug-out returns. A release inside that window writes to a closed or
 * reused descriptor and reads freed memory. ➡️ These give a signal thread the
 * one guarantee it needs -- plug-out does not close anything until it has
 * finished. ⓘ The DualSense's connected signal was the one that waited for
 * nothing, until it claimed as well (feedback_play_connected, 2026-09-21).
 *
 * ⓘ Every touch is under signal_mutex, including the value kept for the
 * signal to give back: the session thread writes it, the signal thread reads
 * it, and the check that nothing newer arrived has to be one step with ending
 * the signal, or a colour the host sets in between is lost. */
bool controller_signal_begin(ctm_controller_t *c)
{
    if (!c) return false;
    pthread_mutex_lock(&c->signal_mutex);
    const bool ok = !c->signal_running && !c->signal_closed;
    if (ok) {
        c->signal_running = 1;
        c->signal_motors_done = 0;
    }
    pthread_mutex_unlock(&c->signal_mutex);
    return ok;
}

void controller_signal_motors_done(ctm_controller_t *c)
{
    if (!c) return;
    pthread_mutex_lock(&c->signal_mutex);
    c->signal_motors_done = 1;
    pthread_mutex_unlock(&c->signal_mutex);
}

bool controller_signal_motors_held(ctm_controller_t *c)
{
    if (!c) return false;
    pthread_mutex_lock(&c->signal_mutex);
    /* ⓘ Once plug-out has begun the pad is going back to the TV, so the host's
     * motors stay withheld to the end, as its light does. */
    const bool held = c->signal_closed || (c->signal_running && !c->signal_motors_done);
    pthread_mutex_unlock(&c->signal_mutex);
    return held;
}

bool controller_signal_end(ctm_controller_t *c, const uint32_t *gave_back)
{
    if (!c) return true;
    pthread_mutex_lock(&c->signal_mutex);
    /* ⓘ Once plug-out has begun the host's wishes no longer matter: the
     * controller is on its way back to the TV. */
    const bool done = !gave_back || c->signal_closed || c->signal_kept == *gave_back;
    if (done) {
        c->signal_running = 0;
        pthread_cond_broadcast(&c->signal_idle);
    }
    pthread_mutex_unlock(&c->signal_mutex);
    return done;
}

bool controller_signal_stopping(ctm_controller_t *c)
{
    if (!c) return true;
    pthread_mutex_lock(&c->signal_mutex);
    const bool closed = c->signal_closed != 0;
    pthread_mutex_unlock(&c->signal_mutex);
    return closed;
}

/* TRUE ONLY INSIDE A CLAIMED SIGNAL THAT PLUG-OUT IS NOW WAITING FOR, which is
 * the question a signal has to ask before it cuts itself short.
 *
 * NOT controller_signal_stopping() on its own, and the difference is the whole
 * point. The release tone plays AFTER signal_close() has set the closed flag --
 * that is the order the unplug requires, because the audio device is torn down
 * straight afterwards. So "closed" alone reads true throughout the release
 * tone, and a signal that trusted it would abort the very tone the release
 * exists to play, then let the teardown close the device with it still queued.
 *
 * The CLAIM is what tells the two apart: the connect signal holds one, the
 * release tone does not. Both flags together mean "you are the claimed signal,
 * and something is blocked waiting for you to finish".
 *
 * A signal with no controller -- the refusal path -- can have nothing waiting
 * on it, so it is never cancelled. That is the opposite of
 * controller_signal_stopping's answer for NULL, deliberately: that one is asked
 * by callers who own the controller, this one by a write loop that may not. */
bool controller_signal_cancelled(ctm_controller_t *c)
{
    if (!c) return false;
    pthread_mutex_lock(&c->signal_mutex);
    const bool cancelled = c->signal_running && c->signal_closed;
    pthread_mutex_unlock(&c->signal_mutex);
    return cancelled;
}

bool controller_signal_host_report(ctm_controller_t *c, bool keep, uint32_t value)
{
    if (!c) return false;
    pthread_mutex_lock(&c->signal_mutex);
    if (keep) c->signal_kept = value;
    const bool held = c->signal_running || c->signal_closed;
    pthread_mutex_unlock(&c->signal_mutex);
    return held;
}

/* Is a signal painting this pad, or is the pad on its way back to the TV?
 * Either way its light is not the ready light's to touch. */
static bool controller_signal_busy(ctm_controller_t *c)
{
    pthread_mutex_lock(&c->signal_mutex);
    const bool busy = c->signal_running || c->signal_closed;
    pthread_mutex_unlock(&c->signal_mutex);
    return busy;
}

/* --- the bridged-and-waiting light ----------------------------------------
 *
 * ⭐⭐ A BRIDGED PAD BREATHES GREEN UNTIL THE PC TAKES ITS LIGHT. rhoquinn8217,
 * 2026-10-08: "I want a continuous breathing to show that it's ready and allow
 * anything to take it, most likely steam. But I want it to signify it's
 * already bridge but nothing has taken it yet."
 *
 * ⓘ On the session thread, which writes the PC's reports anyway: one light
 * report every SIGNAL_READY_STEP_MS, from when the bridge's own signal has
 * finished -- it ends on solid green, where the breath starts -- and never
 * while a signal holds the pad or the pad is going back to the TV. The PC's
 * first report that sets the light ends it for the rest of the session, a
 * reconnect included: the PC still has the pad then. Nothing is restored when
 * it stops, because the report that stopped it has just set the colour. */
static void ready_light_note_host(ctm_controller_t *c, const uint8_t *data, size_t len)
{
    if (!c->ops->host_sets_light) return;
    if (c->ready_light_taken && !c->ready_light_owed) return;   /* nothing left to follow */
    if (!c->ops->host_sets_light(data, len)) return;
    /* ⭐⭐ A COLOUR SET WHILE THE BRIDGE'S OWN SIGNAL PAINTS THE PAD IS KEPT
     * (rhoquinn8217's C3, 2026-10-09). Steam set its colour 1.7 s into the
     * bridge's 2.4 s green and tone; the signal's next frame covered it, the PC
     * never sent it again, and the pad sat on solid green -- neither the PC's
     * colour nor the breath, since the PC had "set the light". So it is kept
     * and put back the moment the signal ends (ready_light_tick). A colour the
     * PC sets after that is the latest, and nothing is owed. */
    uint8_t rgb[3];
    if (c->ops->paint_light && c->ops->host_light_colour && ctm_sig_light_on() &&
        controller_signal_busy(c) && c->ops->host_light_colour(data, len, rgb)) {
        if (!c->ready_light_owed) {
            ctl_log(c, "ready light: the PC set the light to %u,%u,%u while the bridge's "
                       "signal painted the pad; it goes back on when the signal ends",
                    rgb[0], rgb[1], rgb[2]);
        }
        memcpy(c->ready_light_owed_rgb, rgb, sizeof rgb);
        c->ready_light_owed = 1;
    } else if (c->ready_light_owed) {
        c->ready_light_owed = 0;
        ctl_log(c, "ready light: the PC set the light again after the signal, so nothing is put back");
    }
    if (c->ready_light_taken) return;
    c->ready_light_taken = 1;
    ctl_log(c, "ready light: the PC set the light%s",
            c->ready_light_t0_us ? ", so it stops breathing" : " before it began to breathe");
}

static void ready_light_tick(ctm_controller_t *c)
{
    /* The PC's colour, back on once the signal that covered it has ended
     * (ready_light_note_host). Never while a signal holds the pad, so never
     * on the way back to the TV. */
    if (c->ready_light_owed && !controller_signal_busy(c)) {
        c->ready_light_owed = 0;
        const uint8_t *k = c->ready_light_owed_rgb;
        const int rc = c->ops->paint_light(c, k[0], k[1], k[2]);
        ctl_log(c, "ready light: the PC's colour %u,%u,%u is back on, the signal has ended (rc=%d)",
                k[0], k[1], k[2], rc);
    }
    if (!c->ops->ready_light || c->ready_light_taken || !ctm_sig_light_on()) return;
    const uint64_t now = now_us();
    if (c->ready_light_next_us != 0 && now < c->ready_light_next_us) return;
    c->ready_light_next_us = now + (uint64_t)SIGNAL_READY_STEP_MS * 1000u;
    if (controller_signal_busy(c)) return;
    if (c->ready_light_t0_us == 0) {
        c->ready_light_t0_us = now;
        ctl_log(c, "ready light: breathing green until the PC sets the light");
    }
    const int level = signal_ready_level((long)((now - c->ready_light_t0_us) / 1000u));
    if (c->ops->ready_light(c, level) == 0) {
        c->ready_light_failed = 0;
    } else if (++c->ready_light_failed == 20) {
        /* ⓘ A pad that refused a second of these refuses the rest: stop
         * asking, rather than writing into nothing for the whole session. */
        c->ready_light_taken = 1;
        ctl_log(c, "ready light: 20 writes in a row failed, so it stops");
    }
}

uint32_t controller_signal_kept(ctm_controller_t *c)
{
    if (!c) return 0;
    pthread_mutex_lock(&c->signal_mutex);
    const uint32_t kept = c->signal_kept;
    pthread_mutex_unlock(&c->signal_mutex);
    return kept;
}

/* Plug-out's half: no signal may start from here on, and one already running
 * is told to stop and waited for. ⚠️ Unbounded on purpose -- the other choice
 * is closing the node under a write. A signal thread checks between steps of a
 * few tens of milliseconds, so the wait is one step long -- or one write, if
 * the pad is stalling its writes. ⓘ Instant when no signal is playing. */
static void signal_close(ctm_controller_t *c)
{
    pthread_mutex_lock(&c->signal_mutex);
    c->signal_closed = 1;
    while (c->signal_running) pthread_cond_wait(&c->signal_idle, &c->signal_mutex);
    pthread_mutex_unlock(&c->signal_mutex);
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
    /* An input-node type has no HID reports to write: it acts on the host's
     * report itself (rumble, for a wired Xbox pad). */
    if (c->ops->write_output) {
        const int rc = c->ops->write_output(c, c->hid_fd, data, len);
        return rc;
    }
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
    if (n == (ssize_t)len) return 0;
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

/* ⛔⛔ A FAILED SEND IS A LOST LINK, NOT A STOP (code review, 2026-10-05).
 * The input thread set `stop` when a report or a keepalive would not send.
 * That ended the session for good and said nothing: no "link lost", no
 * reconnect, and neither host gone nor device gone, so the app never
 * released the row, which stayed "bridged" with nothing reaching the PC
 * until Release or the end of the stream. ➡️ Now the session loop ends the
 * connection, as it does when a receive fails: the reconnect runs, and if
 * the PC does not answer within CTM_HOST_GONE_MS it raises host gone, which
 * the app releases. When: the input thread and the sibling readers. */
static void input_link_lost(ctm_controller_t *c, const char *what)
{
    if (!c->link_send_failed) {
        ctl_log(c, "link lost: the %s would not send (errno=%d)", what, errno);
    }
    c->link_send_failed = 1;
}

/* Sibling reader: poll one interface's hidraw, forward input tagged with its IN
 * endpoint. When: one thread per sibling, while comp_run. */
static void *composite_reader_main(void *arg)
{
    comp_iface_t *ci = (comp_iface_t *)arg;
    ctm_controller_t *c = ci->c;
    while (c->comp_run && !c->stop && !c->link_send_failed) {
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
                if (c_send(c, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, ci->in_ep, buf, (size_t)n) != 0) {
                    input_link_lost(c, "sibling's report");
                    break;
                }
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            break;
        }
    }
    return NULL;
}

/* ⭐ What the host gets while the TV's overlay holds input: the type's own blank
 * where it has one, else the blank built from the pad's descriptor, else the
 * report as it came (a keyboard or a mouse is handed back to the TV instead;
 * see handback_sync_hold). */
static void blank_held_report(ctm_controller_t *c, uint8_t *buf, size_t n)
{
    if (c->ops && c->ops->blank_input) c->ops->blank_input(buf, n);
    else pad_blank_apply(&c->pad_blank, buf, n);
}

/* ⭐ Send a still pad's present state when nothing has gone to the host for
 * ops->keepalive_ms -- and at once when the session starts, since
 * last_input_us begins at 0. An input node, and a hidraw pad that reports only
 * on a change, says nothing while the pad is still, and the listener drops a
 * pad that is silent for 15 s. Returns -1 if the link is gone. When: the input
 * thread, whenever its poll times out.
 *
 * ⭐⭐ The type's current_report first; when it has none, or nothing yet, the
 * pump's own: the pad's last report, else one with nothing pressed from its
 * descriptor (pad_keep_current). ⛔ Without that second half a pad bridged from
 * the panel and never touched had nothing to send, and left after 15 s
 * (rhoquinn8217, 2026-09-16, two Bluetooth Xbox pads on the rooted LG). */
static int send_keepalive(ctm_controller_t *c)
{
    if (!c->ops->keepalive_ms) return 0;
    const uint64_t now = now_us();
    if (c->last_input_us != 0 &&
        now - c->last_input_us < (uint64_t)c->ops->keepalive_ms * 1000u) {
        return 0;
    }
    uint8_t buf[MAX_REPORT];
    int n = c->ops->current_report ? c->ops->current_report(c, buf, sizeof(buf)) : 0;
    if (n <= 0) n = (int)pad_keep_current(&c->pad_keep, &c->pad_blank, buf, sizeof(buf));
    if (n <= 0) return 0;
    /* ⓘ Kept at rest as the pad's own reports are (switch_hold.inl). */
    if (swh_active(&c->swh)) swh_apply(&c->swh_layout, &c->swh, buf, (size_t)n);
    if (ctm_input_is_held()) blank_held_report(c, buf, (size_t)n);
    if (c_send(c, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, c->primary_in_ep, buf, (size_t)n) != 0) {
        return -1;
    }
    c->last_input_us = now;
    return 0;
}

/* How long the loop below may sleep: until the keepalive is due, 20 ms at
 * most. It was 1 ms for every device (code review, 2026-10-05). */
#include "input_wait.inl"
/* When a link in trouble gives its pad back: link_trouble.inl. */
#include "link_trouble.inl"

static void *input_thread_main(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    c->last_input_us = 0;
    while (!c->stop && !c->link_send_failed) {
        struct pollfd pfds[2];
        pfds[0].fd = c->hid_fd; pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = c->wake_pipe[0]; pfds[1].events = POLLIN; pfds[1].revents = 0;
        int pr = poll(pfds, 2, input_wait_ms(now_us(), c->last_input_us, c->ops->keepalive_ms));
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) {
            /* ⓘ A keyboard or mouse at rest still follows the overlay opening
             * and closing. */
            handback_sync_hold(c);
            if (send_keepalive(c) != 0) { input_link_lost(c, "keepalive"); break; }
            continue;
        }
        if (pfds[1].revents & POLLIN) break;
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            /* ⭐⭐ THE DEVICE HAS GONE, AND THE SESSION ENDS WITH IT.
             *
             * ⛔ THE FAULT (rhoquinn8217 on the C3, 2026-10-01: "the keyboard
             * has just connected mid stream and it is un responsive"): a
             * bridged Bluetooth keyboard went to sleep. This thread saw its
             * node hang up and simply returned; the session carried on with
             * nothing to read, its link to the host up and its handles on
             * the keyboard's input nodes still open.
             *
             * ⛔⛔ AND AN OPEN HANDLE ON A DEVICE THAT HAS GONE POISONS ITS
             * NODE. A C3 keeps /dev/input/event0..31 as permanent files, and
             * the kernel leaves such a file tied to the device it last
             * opened until every handle on it is closed. The keyboard came
             * back under the same number and NOTHING could open it: not the
             * TV's own keyboard reader, and not a new session of ours, which
             * logged `evdev: 0 node(s) grabbed`. Ending the stream closed
             * the handles and the node worked at once.
             *
             * ➡️ So: say so, stop, and raise the flag. The session loop sees
             * `stop` within 50 ms and lets go of the input nodes; the app's
             * tick sees the flag and releases the session, which closes the
             * device's own node as well. */
            ctl_log(c, "device gone: its node hung up -- ending the session");
            pthread_mutex_lock(&c->status_mutex);
            c->st_device_gone = 1;
            pthread_mutex_unlock(&c->status_mutex);
            c->stop = 1;
            break;
        }
        if (!(pfds[0].revents & POLLIN)) continue;
        for (;;) {
            uint8_t buf[MAX_REPORT];
            ssize_t n;
            if (c->ops->read_input) {
                /* ⓘ 0 is "no complete frame yet", not an error. */
                n = c->ops->read_input(c, c->hid_fd, buf, sizeof(buf));
                if (n == 0) break;
            } else {
                n = read(c->hid_fd, buf, sizeof(buf));
            }
            if (n > 0) {
                /* ⛔ Nothing here arms a microphone, so a report carrying audio
                 * means something else did -- and every reader on this TV,
                 * including SDL, will parse it as sticks and buttons. See the
                 * long note in ctm_mic_safety.inl. */
                /* ⚠️ DualSense reports only. Any other device may use report
                 * id 0x31 for something else entirely, and this exits the app. */
                if (c->ops->speaks_ds5 && micsafe_check_report(c->dev.path, buf, (size_t)n)) {
                    ctl_log(c, "mic-safety: shutting down, see stderr");
                    exit(1);
                }
                /* ⭐⭐ A BRIDGED KEYBOARD: THE OVERLAY'S SHORTCUT, AND THE TV'S TURN.
                 * rhoquinn8217, 2026-09-13: Ctrl+Alt+Shift+S opens the streaming
                 * overlay bridged or not. The grab keeps these keys from the
                 * app, so the shortcut is found here; the report that completes
                 * it goes to the host with nothing pressed, and so does every
                 * report while it is held. While the overlay is open the TV has
                 * the keyboard, or the mouse, and the host gets nothing
                 * (handback_sync_hold). */
                if (c->kbd.present || c->ptr.present) {
                    handback_sync_hold(c);
                    if (c->handed_back) {
                        continue;
                    }
                }
                /* ⭐ The app's other shortcuts, A, C, M and N, are found the same
                 * way (kbd_chord.inl) and handed to the app, and their report
                 * goes to the host with nothing pressed as well. */
                if (c->kbd.present) {
                    const char key = kbd_shortcut_letter(&c->kbd, buf, (size_t)n);
                    if (key != 0) {
                        if (key != c->kbd_chord_key) {
                            c->kbd_chord_key = key;
                            if (key == 'S') {
                                ctl_log(c, "keyboard: overlay shortcut pressed%s",
                                        g_overlay_cb ? "" : " (no app to tell)");
                                if (g_overlay_cb) g_overlay_cb();
                            } else {
                                ctl_log(c, "keyboard: shortcut Ctrl+Alt+Shift+%c pressed%s", key,
                                        g_shortcut_cb ? "" : " (no app to tell)");
                                if (g_shortcut_cb) g_shortcut_cb(key);
                            }
                        }
                        kbd_released_report(&c->kbd, buf, (size_t)n);
                    } else {
                        c->kbd_chord_key = 0;
                    }
                }
                /* ⭐ A bridged mouse's first report that says something, on its
                 * way to the host: the app asks then whether to turn the TV's
                 * cursor off (ptr_report_active, pointer_layout.inl). */
                if (c->ptr.present && !c->ptr_used && ptr_report_active(&c->ptr, buf, (size_t)n)) {
                    c->ptr_used = 1;
                    ctl_log(c, "mouse: first used since the bridge%s",
                            g_pointer_used_cb ? "" : " (no app to tell)");
                    if (g_pointer_used_cb) g_pointer_used_cb();
                }
                /* ⭐⭐ LOOK FIRST WHEN INPUT IS HELD, because the copy the host
                 * gets is about to be blanked and the chord lives in the real
                 * one.
                 *
                 * ⓘ The order used to be "relay first, look second", which was
                 * right when the two saw the same bytes. While the TV's overlay
                 * is open they do not: the type still needs the true report to
                 * find an unbridge chord in it, and the host must see nothing
                 * pressed. ➡️ So the peek moves ahead of the send, and only for
                 * that case -- see ctm_input_set_held. */
                const int held = ctm_input_is_held();
                if (held && c->ops && c->ops->on_input_report) {
                    c->ops->on_input_report(c, buf, (size_t)n);
                }
                /* ⭐ The overlay's volume: the right stick and its click, read
                 * from the real report before the blank (held_stick.inl). */
                if (held && c->ops && c->ops->read_right_stick) {
                    held_stick_note(c, buf, (size_t)n);
                }
                /* ⭐ The TV's pointer: where the finger is, read from the real
                 * report before the blank (held_touch.inl). */
                if (held && c->ops && c->ops->read_touch) {
                    held_touch_note(c, buf, (size_t)n);
                }
                /* ⭐ A still pad's state is kept as the pad sent it, before a
                 * hold blanks it, so what is sent again after the overlay
                 * closes is what the pad still holds. */
                if (c->ops->keepalive_ms && !c->ops->current_report) {
                    pad_keep_update(&c->pad_keep, &c->pad_blank, buf, (size_t)n);
                }
                /* ⭐⭐ NOTHING HELD AT THE SWITCH REACHES THE PC (switch_hold.inl):
                 * the first report says what is held, and the copy the PC is
                 * sent keeps each at rest until it is let go. ⓘ The type reads
                 * the real report, below: the unbridge chord lives in it. */
                uint8_t *to_host = buf;
                uint8_t released[MAX_REPORT];
                if (c->swh.armed) {
                    const int found = swh_note(&c->swh_layout, &c->swh, buf, (size_t)n);
                    if (found > 0) {
                        c->swh_told = true;
                        ctl_log(c, "switch: %d input(s) held as the bridge started, sent as released until let go",
                                found);
                    }
                    if (swh_active(&c->swh)) {
                        memcpy(released, buf, (size_t)n);
                        swh_apply(&c->swh_layout, &c->swh, released, (size_t)n);
                        to_host = released;
                    } else if (c->swh_told) {
                        c->swh_told = false;
                        ctl_log(c, "switch: everything held as the bridge started has been let go");
                    }
                }
                if (held) {
                    blank_held_report(c, to_host, (size_t)n);
                }
                if (c_send(c, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, c->primary_in_ep, to_host, (size_t)n) != 0) {
                    input_link_lost(c, "input report");
                    break;
                }
                if (c->ops->keepalive_ms) c->last_input_us = now_us();
                /* Relay first, look second: the host sees the report whatever
                 * the type makes of it. */
                if (!held && c->ops && c->ops->on_input_report) {
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
/* ⭐ How long the reconnect loop tries before deciding the host is gone.
 * ⭐⭐ THREE SECONDS, AND THE LISTENER WAITS FIVE. Read off the logs, not
 * guessed -- see the note at the give-up itself in session_main. */
#define CTM_HOST_GONE_MS 3000
/* How long a connection waits for the listener to answer its greeting. */
#define CTM_HOST_ANSWER_US 5000000ull

static void handle_message(ctm_controller_t *c, ctmb_host_config_t *host_cfg,
                           queued_report_t *paced_q, int *paced_head, int *paced_count,
                           const ctmb_header_t *h, uint8_t *payload)
{
    /* Anything at all from the listener answers the greeting. */
    c->host_answered = 1;
    if (h->type == CTMB_MSG_OUTPUT_REPORT) {
        /* ⓘ Before anything rewrites it: the PC setting the light ends the
         * ready light (ready_light_note_host). */
        ready_light_note_host(c, payload, h->payload_len);
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

        /* ⭐ THE HOST OWNS THE AUDIO BUFFER. Windows is the authority while a
         * stream is up, and a controller is only ever bridged during one.
         *
         * Written straight into the live settings rather than carried
         * separately: the patch hook already reads them on every outbound
         * report, so the next report carries the new value. ⭐ That is what
         * makes it apply mid-game, which is the whole point -- audio going
         * choppy is not something to fix by unbridging.
         *
         * ⛔ CTMB_LATENCY_UNSET leaves the TV's own value alone, so a client
         * that never sets it behaves exactly as before. */
        if (host_cfg->latency_ms != CTMB_LATENCY_UNSET) {
            tv_bridge_worker_settings_t s;
            ctm_controller_get_settings(c, &s);
            if (s.latency_ms != (unsigned int)host_cfg->latency_ms) {
                s.latency_ms = (unsigned int)host_cfg->latency_ms;
                ctm_controller_set_settings(c, &s);
                ctl_log(c, "host set audio latency to %u ms", s.latency_ms);
            }
        }

        /* ⭐⭐ AND THE AUDIO SETTINGS, THE SAME WAY. T-130, 2026-08-25.
         *
         * ⛔ THE FAULT: Windows patches these into the host's outbound report,
         * and every one of those patchers checks for report id 0x02 -- the
         * WIRED id. Over Bluetooth the host sends 0x36, so speaker volume,
         * headset volume, routing and the three rumble gains silently did
         * nothing. ⓘ Heard, not inferred: `speaker_volume = 0` left the
         * controller at full.
         *
         * ⚠️ WHY THE VALUES TRAVEL RATHER THAN WINDOWS LEARNING 0x36: a
         * Bluetooth output report is signed, and the TV re-signs only when it
         * patched something itself. A report Windows edited alone would arrive
         * with a stale signature and be dropped. ➡️ The TV already walks the
         * 0x90 block and already re-signs; it only lacked the numbers.
         *
         * ⛔⛔ AN OLDER CLIENT SENDS ZEROES HERE, AND ZERO IS A LEGAL
         * PERCENTAGE. Trusting it would mute every controller bridged from an
         * unpatched host. ➡️ An all-zero triple is treated as unset, the same
         * as 0xFF. */
        {
            const uint8_t spk  = host_cfg->speaker_volume_pct;
            const uint8_t hset = host_cfg->headset_volume_pct;
            const uint8_t mode = host_cfg->audio_mode;
            const int all_zero = (spk == 0 && hset == 0 && mode == 0);

            if (!all_zero) {
                tv_bridge_worker_settings_t s;
                int changed = 0;
                ctm_controller_get_settings(c, &s);

                if (spk != CTMB_AUDIO_UNSET && spk <= 100) {
                    if (s.speaker_volume_percent != (unsigned int)spk) {
                        s.speaker_volume_percent = (unsigned int)spk;
                        changed = 1;
                    }
                    /* ⭐ Mark it as the USER'S, not a slider default. Without
                     * this the AUTO path leaves it alone whenever the game has
                     * asked for anything -- which it always has. */
                    if (!s.host_audio_set) { s.host_audio_set = 1; changed = 1; }
                }
                if (hset != CTMB_AUDIO_UNSET && hset <= 100) {
                    if (s.headset_volume_percent != (unsigned int)hset) {
                        s.headset_volume_percent = (unsigned int)hset;
                        changed = 1;
                    }
                    if (!s.host_audio_set) { s.host_audio_set = 1; changed = 1; }
                }
                /* ⓘ BOTH is the highest mode, not OFF -- the enum runs
                 * AUTO, OFF, SPEAKER, HEADSET, BOTH. */
                if (mode != CTMB_AUDIO_UNSET && mode <= TV_BRIDGE_AUDIO_BOTH &&
                    s.audio_mode != (tv_bridge_audio_mode_t)mode) {
                    s.audio_mode = (tv_bridge_audio_mode_t)mode;
                    changed = 1;
                }
                if (changed) {
                    ctm_controller_set_settings(c, &s);
                    ctl_log(c, "host set audio: speaker %u%%, headset %u%%, mode %d",
                            s.speaker_volume_percent, s.headset_volume_percent,
                            (int)s.audio_mode);
                }
            }
        }
    } else if (h->type == CTMB_MSG_ISO_AUDIO) {
        /* Wired speaker and haptics. A host that never sends this never
         * reaches here, and a controller with no audio device open drops it. */
        write_iso_audio(c, payload, h->payload_len);
    }
}

/* Card matching lives in its own file: it is a feature of ours rather than a
 * gap in the wired support, so an upstream change to the Bluetooth path
 * cannot collide with it. Included rather than compiled separately because
 * ctm_controller_t is defined here and opaque everywhere else. */
/* Defined below, after the ALSA opener it belongs with. Declared here so the
 * card-matching reopen can send the same settings report rather than keeping
 * a second copy that could drift. */
void ctm_controller_send_speaker_init(ctm_controller_t *c);

#include "ctm_cardmatch.inl"
/* The Bluetooth confirmation signal. Included BEFORE the feedback file, which
 * calls into it. Fork-only: deleting these two lines and the file removes the
 * feature. */
#include "ctm_bt_signal.inl"

/* Signals with no session behind them, for a refused plug. Included after the
 * Bluetooth signal (whose pattern names it shares) and after the card
 * matching (whose playback open it uses). Fork-only. */
#include "ctm_signal_wired.inl"

/* Which way a DualSense's signal goes. Before the feedback file, which asks it,
 * and the plug-time speaker wake, which asks it too. */
#include "signal_route.inl"

#include "ctm_feedback.inl"

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
        /* ⛔ OVER TCP THE READ BLOCKS UNTIL BYTES ARRIVE (code review,
         * 2026-10-05), so the 5 s below could never fire: a listener that
         * accepted and said nothing held this session for ever, with no
         * host-gone. Wait for something to read, a slice at a time, and
         * count the slices that bring nothing. */
        int ready = ctm_transport_wait_readable(&c->xport, 50);
        if (ready < 0) { ctl_log(c, "host config wait: link dropped"); return -1; }
        if (ready == 0) {
            if (now_us() - start >= CTM_HOST_ANSWER_US) { ctl_log(c, "host config timeout"); return -1; }
            continue;
        }
        int got = c_recv(c, &h, &payload);
        if (got < 0) { ctl_log(c, "host config receive failed"); return -1; }
        if (got == 0) {
            if (now_us() - start >= CTM_HOST_ANSWER_US) { ctl_log(c, "host config timeout"); return -1; }
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
    /* A DualSense's handshake waited for the listener's answer; a relay
     * type's did not, and its first message counts (handle_message). */
    if (c->ops->needs_host_config) c->host_answered = 1;
    /* The opening exchange is done: the microphone may speak (code review,
     * 2026-10-05; see ctm_transport_send_when_ready). */
    ctm_transport_set_ready(&c->xport, 1);

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
    c->link_send_failed = 0;
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
    /* Buttons work from the line above. Everything below takes about three
     * seconds, and it is deliberately after the input thread rather than
     * before it: the card probe used to run first, so a controller was
     * unresponsive while its microphone was being identified. Audio can wait
     * three seconds; the sticks cannot.
     *
     * Order here is forced, not preference. The probe moves the speaker onto
     * the right card, so anything that makes a sound has to come after it or
     * it plays from whichever controller the scan happened to pick -- the
     * fault this all exists to fix. */
    /* DELIBERATELY NOT SERIALISED BEYOND THE PROBE.
     *
     * Two controllers bridged a second apart lose tones, and once left one
     * with no audio device at all. Wrapping all of this in the probe's lock
     * would very likely hide that -- which is exactly why it is not done yet.
     * A guard that works by accident cannot be told from one that works for
     * the right reason until it fails somewhere nobody expected.
     *
     * The speaker's closes are logged now. Reproduce the fault first, read
     * what actually closed the device, THEN decide what the guard should be.
     * The lock is one line away when that is answered. */
    /* ONE CONTROLLER SETTLES ITS AUDIO AT A TIME.
     *
     * The probe alone was serialised; everything after it was not -- the
     * speaker moving to the identified card, capture taking that card, and
     * the tone played through it. Two controllers bridged within a second of
     * each other interleaved those, and one ended up holding a card the other
     * had not yet released.
     *
     * The open-before-close change above means that can no longer cost a
     * controller its speaker. This makes it CORRECT rather than merely safe:
     * the second controller waits for the first to finish, then takes its own
     * card properly instead of keeping someone else's.
     *
     * A lock rather than a retry deliberately. A retry needs a count and a
     * delay, both guesses, and it can still give up and leave the audio on
     * the wrong controller. A lock waits exactly as long as necessary.
     *
     * Costs the second controller a few seconds before its audio settles --
     * its buttons already work by then. rhoquinn8217: "sometimes computers need to
     * think more", which is exactly what is happening. */
    /* ⛔ A DUALSENSE'S SOUND CARD, MICROPHONE AND TONE, SO ONLY A DUALSENSE.
     * For anything else the card match went looking for a free DualSense card
     * and could claim one by elimination, and the tone was written to the
     * device as DualSense reports. */
    /* ⚠️ THIS BRANCH IS THE ONE ctm_controller_will_signal_connect() ANSWERS FOR
     * THE TV (T-212): the `if` plays a DualSense's signal, the `else` plays a
     * type's own. A pad that reaches neither gets nothing from the core, and
     * before T-212 the TV stood aside for it anyway. Change either side and
     * change the other. */
    if (c->ops->speaks_ds5) {
        pthread_mutex_lock(&g_cardmatch_lock);
        /* ⭐ THE CARD IS FOUND ONCE PER BRIDGE (2026-10-23). A reconnect ran the
         * match again, and with no note to read (a usbbus that cannot be read)
         * it answered by elimination, where this pad's own card is not free
         * (its microphone holds it): the speaker moved to another, unbridged
         * cabled DualSense. A reconnect keeps the card it found, and opens the
         * speaker there again if it was closed. */
        if (c->matched_card < 0) {
            cardmatch_identify(c);
        } else {
            pthread_mutex_lock(&c->alsa_mutex);
            if (c->alsa_fd < 0) {
                ctm_controller_open_alsa_playback(c);
            }
            pthread_mutex_unlock(&c->alsa_mutex);
        }
        mic_capture_start(c);
        feedback_play_connected(c);
        pthread_mutex_unlock(&g_cardmatch_lock);
    } else {
        ctl_log(c, "not a DualSense: no sound card, microphone or tone for this device");
        /* ⭐ A type with a signal of its own starts it here, where the
         * DualSense's starts -- a cabled DS4's light and rumble. It returns at
         * once; the signal plays on its own thread. */
        if (c->ops->signal_connected) c->ops->signal_connected(c);
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
    ctl_log(c, "active host=%s port=%d path=%s product=%s transport=TCP",
         c->host, c->port, c->dev.path, caps->product);

    pthread_mutex_lock(&c->status_mutex);
    c->st_connected = 1;
    pthread_mutex_unlock(&c->status_mutex);

    int link_alive = 1;
    /* ⛔ A RELAY TYPE'S GREETING IS NOT WAITED FOR: handshake() returns once
     * HELLO is out. So a listener that never answered it -- a port that took
     * the connection with nothing behind it -- held this connection for ever,
     * the pad "bridged" for the whole stream (code review, 2026-10-05). Every
     * listener answers HELLO with HOST_CONFIG at once, for every kind, as it
     * has since the first import: nothing at all for as long as a DualSense's
     * handshake waits ends the connection, and session_main counts it. */
    const uint64_t answer_by_us = c->host_answered ? 0 : now_us() + CTM_HOST_ANSWER_US;
    /* ⓘ A failed send ends the connection too: input_link_lost(). */
    while (!c->stop && link_alive && !c->link_send_failed) {
        if (answer_by_us != 0 && !c->host_answered && now_us() >= answer_by_us) {
            ctl_log(c, "host never answered the greeting in %llums -- ending this connection",
                    (unsigned long long)(CTM_HOST_ANSWER_US / 1000u));
            break;
        }
        drain_paced(c, paced_q, &paced_head, &paced_count, &next_paced_us, host_cfg.bt_pace_us);
        /* The bridged-and-waiting light: ready_light_tick. */
        ready_light_tick(c);
        int timeout_ms = 50;
        if (paced_count > 0 && next_paced_us != 0) {
            uint64_t now = now_us();
            timeout_ms = next_paced_us <= now ? 0 : (int)((next_paced_us - now) / 1000u);
            if (timeout_ms > 50) timeout_ms = 50;
        }
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

    /* Nothing more from the microphone on this connection. */
    ctm_transport_set_ready(&c->xport, 0);
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
    /* First line the session writes. If the eight seconds is BEFORE this, the
     * cost is in the app or in ctm_controller_plug_in, not in the session. */
    ctl_log(c, "session thread running");
    ctmb_device_caps_t caps;
    uint8_t report_desc[MAX_REPORT_DESCRIPTOR];
    uint32_t report_desc_len = 0;

    /* ⏱️ TIMED, because a Bluetooth bridge takes eight seconds and every other
     * stage has been measured and cleared: the TV's own work is 138ms, the
     * session's logged work is 8ms, and the connect succeeds on the FIRST
     * attempt -- so the retry loop is not it either. open_hid is the only
     * stretch left that logs nothing at all. */
    struct timespec oh0, oh1;
    clock_gettime(CLOCK_MONOTONIC, &oh0);
    /* ⓘ An input-node type opens its own node and has no report descriptor:
     * the host's device for it is a fixed profile. */
    c->hid_fd = c->ops->open_input
                    ? c->ops->open_input(c, &c->dev, &caps)
                    : open_hid(c, &caps, report_desc, &report_desc_len);
    clock_gettime(CLOCK_MONOTONIC, &oh1);
    ctl_log(c, "open_hid took %ldms",
            (long)((oh1.tv_sec - oh0.tv_sec) * 1000 +
                   (oh1.tv_nsec - oh0.tv_nsec) / 1000000));
    /* ⛔ A SESSION THAT CANNOT START SAYS SO (code review, 2026-10-05). These
     * two exits set only `stop`, which no one outside reads: the app releases
     * a session when it sees host gone or device gone, so the row stayed
     * "bridged" over a session that had ended. Device gone is the flag the
     * app's sweep acts on for a session that cannot use its device; the line
     * above it in the log says why. */
    if (c->hid_fd < 0) {
        ctl_log(c, "hid open failed path=%s errno=%d -- the session cannot start", c->dev.path, errno);
        pthread_mutex_lock(&c->status_mutex);
        c->st_device_gone = 1;
        pthread_mutex_unlock(&c->status_mutex);
        c->stop = 1;
        return NULL;
    }
    /* ⭐ A bridge is a switch: switch_hold_begin. */
    switch_hold_begin(c, report_desc, report_desc_len);
    /* Composite primary endpoints are resolved in open_composite_siblings()
     * (reliable /sys/class/input path), at run_session start. */
    if (pipe(c->wake_pipe) != 0) {
        ctl_log(c, "wake pipe failed errno=%d -- the session cannot start", errno);
        pthread_mutex_lock(&c->status_mutex);
        c->st_device_gone = 1;
        pthread_mutex_unlock(&c->status_mutex);
        c->stop = 1;
        return NULL;
    }
    (void)fcntl(c->wake_pipe[0], F_SETFL, fcntl(c->wake_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(c->wake_pipe[1], F_SETFL, fcntl(c->wake_pipe[1], F_GETFL, 0) | O_NONBLOCK);

    /* ⭐ The trouble clock: from the first connect since the listener last
     * answered a greeting (link_trouble.inl). */
    link_trouble_t trouble = {0, 0};
    while (!c->stop) {
        link_trouble_begin(&trouble, now_us());
        /* ⛔⛔ THIS LOOP IS WHERE A SLOW BRIDGE GOES, AND IT LOGGED NOTHING.
         *
         * Measured 2026-08-18: the TV finished its side of a Bluetooth bridge
         * in 138ms, the session's own log covered 8ms, and the controller was
         * unusable for ~7 seconds. The listener put it plainly -- ready=1 at
         * age_ms=7000 on Bluetooth against 2000 on a cable -- and the gap sat
         * here, invisible, because a failed attempt said nothing.
         *
         * Each pass then cost ENet's 400ms timeout plus a 500ms sleep, so seven
         * seconds was about eight attempts before the host's listener was up.
         *
         * ⭐ Counted and logged now: the number of attempts is what says whether
         * the answer is to wait less, retry faster, or start later. */
        int attempts = 0;
        struct timespec w0;
        clock_gettime(CLOCK_MONOTONIC, &w0);
        for (;;) {
            if (c->stop) break;
            /* ⭐⭐ GIVE UP AFTER THREE SECONDS (2026-10-08; fifteen until then).
             *
             * ⛔ THE FIRST FAULT (2026-08-23): this loop retried FOREVER. Close
             * the listener's window and the controller stayed claimed by a host
             * that no longer existed -- no speaker, no triggers, no microphone,
             * and nothing on screen saying why. ⚠️ The user's only way out was
             * to know to press Release.
             *
             * ⛔ THE SECOND: FIFTEEN SECONDS WAS A FAULT OF ITS OWN. Quit the
             * listener with a pad bridged and the pad answered NOTHING, on the
             * PC or the TV, for sixteen seconds. Fifteen was the listener's
             * reconnect grace, matched on purpose; what the logs say that grace
             * protected (2026-10-08):
             * - In every listener log on the PC, a bridge that dropped and came
             *   back did so within half a second (twice: 0.2 s and 0.5 s).
             * - The C1 held 46 give-ups, every one after the whole wait, and
             *   not one link that came back late.
             * ➡️ So three seconds, six times the longest recovery seen, and the
             * listener's grace went to five, so it still holds the port for the
             * last of these redials (agent.inl, set_session_timeouts). ⭐ The
             * two move together: a TV that dials for longer than the listener
             * holds the port is waiting for nothing.
             *
             * ⭐ AND ONLY TIME TELLS "THE LISTENER QUIT" FROM "THE NETWORK
             * BLINKED". Each of those 46 made 6 attempts in 15 s, two seconds
             * each: the PC never refused a redial, it dropped it in silence
             * (Windows' firewall, with nothing listening on the port). A refusal
             * would have said "gone" at once; that PC never gives one.
             *
             * ⛔ THIS LOOP REDIALS THE BRIDGE SESSION PORT, NOT THE AGENT. The
             * agent is always listening; the session port exists only while a
             * bridge is running, and the agent creates it when the TV asks.
             * ➡️ A restarted listener has nothing on it, so a redial after a
             * restart can never succeed, however long it waits.
             *
             * ➡️ The cost of being wrong is small: the controller returns to
             * Moonlight's own path and still works, just without the extras,
             * and re-bridging is one gesture.
             *
             * ⓘ Timed by the trouble clock, not from this round of connects: a
             * connect that succeeds into a greeting nobody answers must not
             * start the count again (link_trouble.inl). ⚠️ It bounds the
             * connects: one that succeeds near the end still waits out its
             * greeting, so a hung listener that takes connections is 5 to 8 s. */
            const long left_ms = link_trouble_left_ms(&trouble, now_us(), CTM_HOST_GONE_MS);
            if (left_ms == 0) {
                const long waited_ms = link_trouble_ms(&trouble, now_us());
                ctl_log(c, "host gone: no answer for %ldms over %d attempt(s) -- giving up",
                        waited_ms, attempts);
                pthread_mutex_lock(&c->status_mutex);
                c->st_host_gone = 1;
                pthread_mutex_unlock(&c->status_mutex);
                c->stop = 1;
                break;
            }
            /* ⭐ NO ATTEMPT WAITS PAST THE GIVE-UP. A PC that drops a connect
             * holds it for the transport's whole two seconds, so a second one
             * begun at two carried the give-up to four or five. ⓘ The hang on
             * Release that a dead PC once caused was the length of ONE connect,
             * bounded in ctm_transport.c, and not this loop. */
            const uint64_t tried_us = now_us();
            if (ctm_transport_connect_within(&c->xport, c->host, c->port,
                                             (unsigned int)left_ms) == 0) {
                break;
            }
            ++attempts;
            /* ⓘ One attempt each half second at most, counting the attempt's
             * own wait: a refused connect is not tried again at once, and one
             * that waited out its time goes again straight away. */
            while (!c->stop && now_us() - tried_us < 500000ull &&
                   link_trouble_left_ms(&trouble, now_us(), CTM_HOST_GONE_MS) > 0) {
                usleep(50000);
            }
        }
        if (c->stop) break;
        if (attempts > 0) {
            struct timespec w1;
            clock_gettime(CLOCK_MONOTONIC, &w1);
            const long waited_ms = (long)((w1.tv_sec - w0.tv_sec) * 1000 +
                                          (w1.tv_nsec - w0.tv_nsec) / 1000000);
            ctl_log(c, "connect took %ldms over %d failed attempt(s)", waited_ms, attempts);
        }
        ctl_log(c, "connected via TCP");

        if (c->ops->grab_evdev && c->ops->grab_skips_gamepads && c->is_gamepad) {
            ctl_log(c, "evdev: not grabbed -- a gamepad, which the TV must still see "
                       "for the overlay combo");
        } else if (c->ops->grab_evdev) {
            grab_matching_evdev(c);
        }
        /* ⓘ A fresh grab means the host has the keyboard or mouse again,
         * whatever the last session left, and a mouse's first use is this
         * bridge's to tell. */
        c->handed_back = 0;
        c->kbd_chord_key = 0;
        c->ptr_used = 0;
        if (c->ops->on_plug_init) c->ops->on_plug_init(c, &c->xport);

        /* ⭐⭐ WAKE THE BLUETOOTH SPEAKER HERE -- BEFORE THE SESSION, NOT BESIDE
         * THE CONFIRMATION.
         *
         * ⛔ THE FAULT: the FIRST bridge of a session played no tone. Every one
         * after it did, and so did the first unbridge seconds later, so the
         * speaker worked by then -- it was not ready yet. ⚠️ And it had no
         * chance to be: the speaker settings rode on the tone's OWN first
         * report, so configure and play happened in the same breath.
         *
         * ⛔⛔ FIRST ATTEMPT PUT THIS IN feedback_play_connected AND IT BROUGHT
         * THE FLICKER BACK. That runs at the instant the app draws its green
         * breath, so a 398-byte write landed in the middle of the app's own
         * light writes -- two writers on one light, which is what flickering
         * has meant every time. ⓘ It also gave almost no head start, which was
         * the entire point.
         *
         * ⭐ Here it is before the session loop and well before anything is
         * drawn. Same place the wired path opens its audio device, and for the
         * same reason.
         *
         * ⓘ Wired takes the other branch: it has a real audio device, opened
         * in on_plug_init just above. */
        /* ⚠️ "No audio device" is not "Bluetooth DualSense": a keyboard has no
         * audio device either, and was sent this 398-byte report. ⛔ Nor is a
         * cabled DualSense, which has none YET: its card is opened by the card
         * match inside the session, after this line, so every cabled bridge
         * was sent the Bluetooth report too (code review, 2026-10-05). The bus
         * decides: signal_route.inl. */
        if (c->ops->speaks_ds5 &&
            signal_route_for(c->dev.bus, c->alsa_fd >= 0) == SIGNAL_ROUTE_BLUETOOTH) {
            btsig_wake_speaker(c);
        }

        c->host_answered = 0;
        run_session(c, &caps, report_desc, report_desc_len);
        release_evdev_grabs(c);

        ctm_transport_disconnect(&c->xport);
        if (c->host_answered) {
            link_trouble_clear(&trouble);
        } else if (!c->stop) {
            /* ⛔ THE GREETING WAS NOT ANSWERED: the listener took the
             * connection and turned it down, or said nothing. That is trouble,
             * as a failed connect is, and the pad goes back to the TV when it
             * has lasted as long (link_trouble.inl). */
            ++trouble.unanswered;
            const long troubled_ms = link_trouble_ms(&trouble, now_us());
            if (troubled_ms >= CTM_HOST_GONE_MS) {
                ctl_log(c, "host gone: no greeting answered for %ldms over %d connection(s) -- giving up",
                        troubled_ms, trouble.unanswered);
                pthread_mutex_lock(&c->status_mutex);
                c->st_host_gone = 1;
                pthread_mutex_unlock(&c->status_mutex);
                c->stop = 1;
            } else {
                /* Not straight back: a listener that turns every greeting down
                 * would be asked again as fast as it could answer. */
                for (int slept = 0; slept < 500 && !c->stop; slept += 50) usleep(50000);
            }
        }
        if (!c->stop) ctl_log(c, "link lost; retrying probe loop");
    }
    c->stop = 1;
    return NULL;
}

/* --- lifecycle ----------------------------------------------------------- */

void *controller_type_ctx(const ctm_controller_t *c)
{
    return c ? c->type_ctx : NULL;
}

void controller_set_type_ctx(ctm_controller_t *c, void *ctx)
{
    if (c) c->type_ctx = ctx;
}

int controller_preflight(const ctm_controller_dev_t *dev)
{
    if (!dev) return EINVAL;
    const ctm_controller_ops_t *ops = ctm_controller_ops_for(dev);
    /* An input-node type knows how to open what it reads. */
    if (ops->preflight) return ops->preflight(dev);
    /* ⭐ The node the session will actually open, which for a composite
     * device may not be dev->path. */
    char node[64];
    const char *path = dev->path;
    if (ops->select_node && ops->select_node(dev, node, sizeof(node)) == 0 && node[0]) {
        path = node;
    }
    return hid_node_preflight(path, (unsigned int)strtoul(dev->vid, NULL, 16),
                              (unsigned int)strtoul(dev->pid, NULL, 16));
}

/* Build an idle controller for a detected device (factory picks its ops).
 * When: the UI/monitor decides to offer a device; before plug_in. */
ctm_controller_t *ctm_controller_create(const ctm_controller_dev_t *dev)
{
    if (!dev) return NULL;
    ctm_controller_t *c = (ctm_controller_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->dev = *dev;
    snprintf(c->kernel_uniq, sizeof(c->kernel_uniq), "%s", dev->mac);
    c->ops = ctm_controller_ops_for(dev);
    c->vid_num = (unsigned int)strtoul(dev->vid, NULL, 16);
    c->pid_num = (unsigned int)strtoul(dev->pid, NULL, 16);
    c->hid_fd = -1;
    c->alsa_fd = -1;
    c->mic_cap_started = 0;
    c->btsig_primed = 0;
    c->matched_card = -1;
    c->card_fresh = 0;
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
    pthread_mutex_init(&c->signal_mutex, NULL);
    pthread_mutex_init(&c->alsa_mutex, NULL);
    pthread_cond_init(&c->signal_idle, NULL);
    return c;
}

/* Open this controller's per-MAC log file, ctm-<mac>.log in the app's logs
 * folder ($HOME/logs, or /tmp with no HOME). When: plug_in. */
static void open_log(ctm_controller_t *c)
{
    char mac[64], name[128];
    size_t o = 0;
    for (size_t i = 0; c->dev.mac[i] && o + 1 < sizeof(mac); ++i) {
        char ch = c->dev.mac[i];
        if (ch != ':') mac[o++] = ch;
    }
    mac[o] = '\0';
    {
        char leaf[128];
        snprintf(leaf, sizeof(leaf), "ctm-%s.log", mac[0] ? mac : c->ops->kind);
        snprintf(name, sizeof(name), "%s", ctm_log_path(leaf));
    }
    c->log = fopen(name, "a");
}

/* Start bridging: open log, bring up the transport, launch the session
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
    /* ⓘ A controller plugged in again after a plug-out may signal again. Left
     * closed, a type's patcher would withhold the host's claims for good. No
     * signal thread can be running here: plug-out waited for it. */
    pthread_mutex_lock(&c->signal_mutex);
    c->signal_closed = 0;
    pthread_mutex_unlock(&c->signal_mutex);
    open_log(c);

    ctm_transport_init(&c->xport);

    /* ⏱️ ON THE SAME CLOCK AS THE SESSION'S OWN LINES, deliberately.
     *
     * The gesture log and this one use different clocks, so the gap between
     * "plugged" over there and "session thread running" over here has been
     * invisible. Everything on each side measures fast -- the plug call is
     * 15ms, the session is 1s -- while the host reports seven seconds between
     * asking and the TV connecting. The missing stretch is between this line
     * and the thread's first, and only one clock can show it. */
    ctl_log(c, "spawning session thread");
    if (pthread_create(&c->session_thread, NULL, session_main, c) != 0) {
        ctl_log(c, "session thread failed errno=%d", errno);
        ctm_transport_destroy(&c->xport);
        if (c->log) { fclose(c->log); c->log = NULL; }
        return -1;
    }
    c->session_started = 1;
    return 0;
}

/* Stop bridging: signal stop, wake + join the session thread, then close the
 * HID fd, transport, evdev grabs, and log. When: the user clicks Plug out, or
 * the device disconnects. */
/* Send the speaker's settings report. Called after ANY fresh handle: a new
 * one knows nothing about volume or routing, and this is what puts them back.
 * Split out so the card-matching reopen sends exactly the same thing rather
 * than a second copy that could drift. */
void ctm_controller_send_speaker_init(ctm_controller_t *c)
{
    if (!c) return;
    uint8_t spk_init[DS5_OUT_REPORT_LEN] = {0};
    spk_init[0]                      = DS5_OUT_REPORT_ID;
    spk_init[DS5_IDX_VALID_FLAG0]    = DS5_F0_ALLOW_SPEAKER_VOLUME |
                                       DS5_F0_ALLOW_MIC_VOLUME |
                                       DS5_F0_ALLOW_AUDIO_CONTROL;
    spk_init[DS5_IDX_VALID_FLAG1]    = 0x00;  /* claim nothing else */
    spk_init[DS5_IDX_SPEAKER_VOLUME] = c->audio_spk_vol;
    spk_init[DS5_IDX_MIC_VOLUME]     = DS5_MIC_VOLUME_MAX;
    spk_init[DS5_IDX_AUDIO_CONTROL]  = c->audio_control;
    int svrc = hid_write_report(c, spk_init, sizeof(spk_init));
    ctl_log(c, "alsa: speaker init (merged, %zu bytes) vol=0x%02x mic=0x%02x ctrl=0x%02x rc=%d hid_fd=%d",
            sizeof(spk_init), c->audio_spk_vol, DS5_MIC_VOLUME_MAX,
            c->audio_control, svrc, c->hid_fd);
}

void ctm_controller_open_alsa_playback(ctm_controller_t *c)
{
    if (!c || c->alsa_fd >= 0) return;
    c->alsa_fd = open_ds5_alsa_playback(c->dev.path, c->matched_card);
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
    ctm_controller_send_speaker_init(c);

    /* ds5-aurora fires an open-tone burst here. NOT PORTED: it is a recorded
     * DECISIVE NEGATIVE -- tested on both fault classes with clean deliveries,
     * and the audio still attenuated, because audio content was never the
     * lever and control bytes were. See the disproven list in
     * controller-attenuation-investigation.md before reaching for it again. */
}

void ctm_controller_plug_out(ctm_controller_t *c)
{
    ctm_controller_plug_out_reason(c, CTM_UNPLUG_REQUESTED);
}

volatile bool ctm_unplug_quietly = false;   /* see ctm_controller.h */

void ctm_controller_plug_out_reason(ctm_controller_t *c, ctm_unplug_reason_t why)
{
    if (!c) return;

    /* Silence this controller's microphone before anything else is torn down,
     * while the node is still ours to write to. There is no second chance --
     * the unplug closes the path this would travel.
     *
     * ⭐ ORDERED BEFORE THE SIGNAL ON PURPOSE. If this process dies partway
     * through a teardown, the write has already landed. The detector uses the
     * same ordering for the same reason.
     *
     * ⓘ One write (micsafe_disarm_node), so it costs a few milliseconds.
     * It was five writes 20 ms apart once; the comment outlived them.
     *
     * ⭐ ON A FULL SHUTDOWN, SWEEP EVERYTHING rather than only this
     * controller. A device this app never tracked is exactly the one nothing
     * else will reach, and the sweep costs nothing when there is nothing to
     * silence.
     *
     * ⓘ Nothing on this branch turns a microphone on. This is the same class
     * of guard as the startup sweep above: it exists for a state we cannot
     * cause, and it runs on the one path where we still own the device. */
    /* ⓘ The node write is a DualSense report, so it goes to a DualSense only.
     * The shutdown sweep below stays unconditional: it is not about this
     * device, and it is the one pass that reaches a pad nothing tracked. */
    /* ⭐ A DEVICE THAT IS ALREADY GONE GETS NO WRITES (code review,
     * 2026-10-05). Its node hung up, so every write below fails, and the
     * release signal was still written whole: 1.2 s of failed Bluetooth
     * reports for a DualSense switched off while bridged, 2.4 s when it was
     * never primed, on the app's interface thread, which is where the reaper
     * that brings a gone device here runs. The C3 logged it as "0 sent, 122
     * failed, took 1220ms". */
    pthread_mutex_lock(&c->status_mutex);
    const int gone = c->st_device_gone;
    pthread_mutex_unlock(&c->status_mutex);

    if (c->ops->speaks_ds5 && !gone) micsafe_disarm_node(c->dev.path);
    if (why == CTM_UNPLUG_SHUTDOWN) {
        ctm_mic_safety_disarm_all_reason("the bridge is shutting down");
    }

    /* Before anything is torn down, while the audio device is still open.
     * There is no "after" -- the unplug closes the very thing that would
     * play it. ⛔ DualSense only: for anything else this was 2.4 s of
     * DualSense sound reports written to it, and the release waited for them. */
    /* ⭐ First, no type's signal thread may still be writing: nothing below
     * may close what it writes to while it does, and two signals on one light
     * is a flicker. See controller_signal_begin. */
    signal_close(c);
    if (gone) {
        ctl_log(c, "unplugging -- the device is gone, so no release signal");
    } else if (ctm_unplug_quietly) {
        ctl_log(c, "unplugging -- the app is quitting, so no release signal");
    } else if (c->ops->speaks_ds5) {
        feedback_play_unplugging(c, why);
    } else if (c->ops->signal_unplugging) {
        /* ⓘ A type's own release signal -- a cabled DS4's. Played here for the
         * same reason as the DualSense's: afterwards there is no node. */
        c->ops->signal_unplugging(c, why);
    } else {
        ctl_log(c, "unplugging -- no DualSense signal for this device");
    }
    c->stop = 1;
    if (c->xport.fd >= 0) shutdown(c->xport.fd, SHUT_RDWR);
    if (c->wake_pipe[1] >= 0) (void)write(c->wake_pipe[1], "x", 1);
    if (c->session_started) {
        pthread_join(c->session_thread, NULL);
        c->session_started = 0;
    }
    /* ⛔ THE MICROPHONE THREAD FIRST, THEN ITS TRANSPORT (code review,
     * 2026-10-05). It sends each capture on c->xport, and it was joined AFTER
     * the transport was destroyed, so a capture in flight at a release could
     * send on a transport that was gone. c->stop is set and the socket shut
     * down above, so a send in progress returns and the thread leaves. */
    mic_capture_stop(c);
    ctm_transport_disconnect(&c->xport);
    ctm_transport_destroy(&c->xport);
    if (c->hid_fd >= 0) { close(c->hid_fd); c->hid_fd = -1; }
    pthread_mutex_lock(&c->alsa_mutex);
    if (c->alsa_fd >= 0) {
        ctl_log(c, "alsa: speaker closed (plug out), fd=%d", c->alsa_fd);
        close(c->alsa_fd);
        c->alsa_fd = -1;
    }
    pthread_mutex_unlock(&c->alsa_mutex);
    c->alsa_consec_fail = 0;
    c->alsa_retry_streak = 0;
    if (c->wake_pipe[0] >= 0) { close(c->wake_pipe[0]); c->wake_pipe[0] = -1; }
    if (c->wake_pipe[1] >= 0) { close(c->wake_pipe[1]); c->wake_pipe[1] = -1; }
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
 * (~500 ms). The flags are read under status_mutex. */
void ctm_controller_get_status(ctm_controller_t *c, ctm_controller_status_t *out)
{
    if (!c || !out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&c->status_mutex);
    out->connected = c->st_connected ? true : false;
    out->host_gone = c->st_host_gone ? true : false;
    out->device_gone = c->st_device_gone ? true : false;
    pthread_mutex_unlock(&c->status_mutex);
    /* ⓘ Advisory as the counters are: set once by the plug and once by the
     * input thread. */
    out->pointer = c->ptr.present ? true : false;
    out->pointer_used = c->ptr_used ? true : false;
    /* ⓘ Only what was read in the hold in force (held_stick_note). */
    if (ctm_input_is_held() && c->held_stick_gen == g_input_hold_gen) {
        out->held_stick = true;
        out->held_stick_y = c->held_stick_y;
        out->held_stick_click = c->held_stick_click ? true : false;
    }
    if (ctm_input_is_held() && c->held_touch_gen == g_input_hold_gen) {
        out->held_touch = true;
        out->held_touch_word = c->held_touch_word;
    }
}

/* Free an idle (already plugged-out) controller. When: the device is removed
 * from the list. */
void ctm_controller_destroy(ctm_controller_t *c)
{
    if (!c) return;
    pthread_mutex_destroy(&c->hid_mutex);
    pthread_mutex_destroy(&c->settings_mutex);
    pthread_mutex_destroy(&c->status_mutex);
    pthread_mutex_destroy(&c->signal_mutex);
    pthread_mutex_destroy(&c->alsa_mutex);
    pthread_cond_destroy(&c->signal_idle);
    free(c->enum_payload);
    free(c->type_ctx);
    free(c);
}

/* --- factory: specific types first, generic last ------------------------- */

static const ctm_controller_ops_t *const k_registry[] = {
    &ctm_controller_steam_puck_ops,
    &ctm_controller_ds5_ops,
    &ctm_controller_ds5e_ops,
    &ctm_controller_ds4_ops,
    /* ⭐ A cabled DS4. ⛔ It must stay ahead of generic, which claims anything
     * and ran this pad until now: no chord, no blanking, no signal. */
    &controller_ds4_usb_ops,
    /* ⓘ Before the Bluetooth Xbox type, which only takes hidraw nodes anyway. */
    &controller_xpad_ops,
    &ctm_controller_xbox_ops,
    &ctm_controller_generic_ops,
};

/* T-212. ⚠️ THE TWO FIELDS HERE ARE THE TWO THE BRANCH IN start_session USES --
 * `speaks_ds5` for the DualSense's tone and light, `signal_connected` for a type
 * with a signal of its own. If a type gains one, it becomes true here for free;
 * if this is ever rewritten as a list of kinds, it will drift the first time. */
int ctm_controller_will_signal_connect(const ctm_controller_dev_t *dev)
{
    if (dev == NULL) return 0;
    const ctm_controller_ops_t *ops = ctm_controller_ops_for(dev);
    if (ops == NULL) return 0;
    return (ops->speaks_ds5 || ops->signal_connected != NULL) ? 1 : 0;
}

const ctm_controller_ops_t *ctm_controller_ops_for(const ctm_controller_dev_t *dev)
{
    for (size_t i = 0; i < sizeof(k_registry) / sizeof(k_registry[0]); ++i) {
        const ctm_controller_ops_t *ops = k_registry[i];
        if (ops->matches && ops->matches(dev)) return ops;
    }
    return &ctm_controller_generic_ops;
}
