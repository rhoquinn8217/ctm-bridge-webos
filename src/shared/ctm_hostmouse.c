/* TV-pointer -> host mouse synthesizer. See ctm_hostmouse.h for the model. */

#include "ctm_hostmouse.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctm_bridge_protocol.h"
#include "ctm_transport.h"

/* Absolute-pointer HID report descriptor, matching the deployed AbsMouse/KVM
 * pattern Windows validates (Report ID inside the Physical collection, explicit
 * Physical Min/Max on the absolute axes). Report ID 1:
 * [buttons:1][X:u16 0..32767][Y:u16 0..32767][wheel:s8] = 6 bytes + ID. */
static const uint8_t k_mouse_report_desc[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x09, 0x01,        /*   Usage (Pointer) */
    0xA1, 0x00,        /*   Collection (Physical) */
    0x85, 0x01,        /*     Report ID (1) */
    0x05, 0x09,        /*     Usage Page (Button) */
    0x19, 0x01,        /*     Usage Minimum (1) */
    0x29, 0x03,        /*     Usage Maximum (3) */
    0x15, 0x00,        /*     Logical Minimum (0) */
    0x25, 0x01,        /*     Logical Maximum (1) */
    0x95, 0x03,        /*     Report Count (3) */
    0x75, 0x01,        /*     Report Size (1) */
    0x81, 0x02,        /*     Input (Data,Var,Abs) */
    0x95, 0x01,        /*     Report Count (1) */
    0x75, 0x05,        /*     Report Size (5) */
    0x81, 0x03,        /*     Input (Const,Var,Abs) -- pad */
    0x05, 0x01,        /*     Usage Page (Generic Desktop) */
    0x09, 0x30,        /*     Usage (X) */
    0x09, 0x31,        /*     Usage (Y) */
    0x16, 0x00, 0x00,  /*     Logical Minimum (0) */
    0x26, 0xFF, 0x7F,  /*     Logical Maximum (32767) */
    0x36, 0x00, 0x00,  /*     Physical Minimum (0) */
    0x46, 0xFF, 0x7F,  /*     Physical Maximum (32767) */
    0x75, 0x10,        /*     Report Size (16) */
    0x95, 0x02,        /*     Report Count (2) */
    0x81, 0x02,        /*     Input (Data,Var,Abs) */
    0x09, 0x38,        /*     Usage (Wheel) */
    0x35, 0x00,        /*     Physical Minimum (0) -- reset to logical */
    0x45, 0x00,        /*     Physical Maximum (0) -- reset to logical */
    0x15, 0x81,        /*     Logical Minimum (-127) */
    0x25, 0x7F,        /*     Logical Maximum (127) */
    0x75, 0x08,        /*     Report Size (8) */
    0x95, 0x01,        /*     Report Count (1) */
    0x81, 0x06,        /*     Input (Data,Var,Rel) */
    0xC0,              /*   End Collection */
    0xC0,              /* End Collection */
    /* Second top-level collection: a standard 6-key-rollover keyboard, so the
     * remote's D-pad/OK (and future keys) reach the host as real key strokes.
     * Report ID 2: [modifiers:u8][reserved:u8][keys:6 x u8 usage array]. */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x02,        /*   Report ID (2) */
    0x05, 0x07,        /*   Usage Page (Keyboard/Keypad) */
    0x19, 0xE0,        /*   Usage Minimum (LeftControl) */
    0x29, 0xE7,        /*   Usage Maximum (Right GUI) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x08,        /*   Report Count (8) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) -- modifiers */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x03,        /*   Input (Const,Var,Abs) -- reserved */
    0x19, 0x00,        /*   Usage Minimum (0) */
    0x29, 0x65,        /*   Usage Maximum (0x65) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x65,        /*   Logical Maximum (0x65) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x06,        /*   Report Count (6) */
    0x81, 0x00,        /*   Input (Data,Array,Abs) -- 6KRO key array */
    0xC0               /* End Collection */
};

#define HM_REPORT_LEN 7
#define HM_KB_REPORT_LEN 9
#define HM_KB_MAX_KEYS 6

static struct {
    pthread_t thread;
    bool thread_started;
    volatile int stop;
    volatile int active;
    volatile int connected;
    char host[128];
    int port;
    ctm_transport_t xport;
    pthread_mutex_t state_mutex;
    /* last state sent, to suppress no-change reports */
    uint16_t last_x, last_y;
    uint8_t last_buttons;
    /* pressed keyboard usages (report ID 2, 6KRO array) */
    uint8_t keys[HM_KB_MAX_KEYS];
    void (*log_sink)(const char *line);
} g_hm = { .state_mutex = PTHREAD_MUTEX_INITIALIZER };

static void hm_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_hm.log_sink) g_hm.log_sink(line);
    else fprintf(stderr, "hostmouse: %s\n", line);
}

void ctm_hostmouse_set_logger(void (*sink)(const char *line))
{
    g_hm.log_sink = sink;
}

static int hm_send_hello(void)
{
    ctmb_device_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.vendor_id = 0x1209;
    caps.product_id = 0xC7B1;
    /* bcdDevice bumped with the keyboard collection: Windows caches parsed HID
     * devnodes by VID/PID/bcdDevice, so a descriptor change must look like a
     * new device revision or the old mouse-only parse can stick. */
    caps.version = 0x0101;
    caps.bus = 3; /* present as USB: identity map, full-speed interrupt IN */
    caps.input_report_len = HM_KB_REPORT_LEN; /* max of mouse(7)/keyboard(9) */
    caps.output_report_len = 0;
    caps.feature_report_len = 0;
    snprintf(caps.path, sizeof(caps.path), "virtual");
    snprintf(caps.serial, sizeof(caps.serial), "ctm-tv-pointer");
    snprintf(caps.product, sizeof(caps.product), "CTM TV Pointer");
    snprintf(caps.manufacturer, sizeof(caps.manufacturer), "CTM");

    ctmb_hid_descriptor_info_t info;
    memset(&info, 0, sizeof(info));
    info.report_descriptor_len = (uint32_t)sizeof(k_mouse_report_desc);

    uint8_t hello[sizeof(caps) + sizeof(info) + sizeof(k_mouse_report_desc)];
    memcpy(hello, &caps, sizeof(caps));
    memcpy(hello + sizeof(caps), &info, sizeof(info));
    memcpy(hello + sizeof(caps) + sizeof(info), k_mouse_report_desc, sizeof(k_mouse_report_desc));
    return ctm_transport_send_msg(&g_hm.xport, CTMB_MSG_HELLO, CTMB_FLAG_OK, 0,
                                  hello, sizeof(hello));
}

/* Session thread: connect (retry until stop), HELLO, then serve the host's
 * feature requests / ignore outputs until the link drops; reconnect in place. */
static void *hm_session_main(void *arg)
{
    (void)arg;
    while (!g_hm.stop) {
        ctm_transport_init(&g_hm.xport, NULL);
        while (!g_hm.stop &&
               ctm_transport_connect_once(&g_hm.xport, g_hm.host, g_hm.port, 400) != 0) {
            usleep(500 * 1000);
        }
        if (g_hm.stop) { ctm_transport_disconnect(&g_hm.xport); break; }
        if (hm_send_hello() != 0) {
            hm_log("HELLO failed");
            ctm_transport_disconnect(&g_hm.xport);
            usleep(500 * 1000);
            continue;
        }
        hm_log("connected via %s", g_hm.xport.kind == CTM_TRANSPORT_ENET ? "ENet" : "TCP");
        pthread_mutex_lock(&g_hm.state_mutex);
        g_hm.connected = 1;
        g_hm.last_buttons = 0xFF; /* force first feed to send */
        memset(g_hm.keys, 0, sizeof(g_hm.keys)); /* no stuck keys across links */
        pthread_mutex_unlock(&g_hm.state_mutex);

        for (;;) {
            if (g_hm.stop) break;
            ctmb_header_t h;
            uint8_t *payload = NULL;
            int got = ctm_transport_recv_msg(&g_hm.xport, &h, &payload);
            if (got < 0) { hm_log("link dropped"); break; }
            if (got == 0) continue;
            if (h.type == CTMB_MSG_FEATURE_GET || h.type == CTMB_MSG_FEATURE_SET) {
                /* No feature reports on this device; reply not-OK, empty. */
                (void)ctm_transport_send_msg(&g_hm.xport, CTMB_MSG_FEATURE_REPORT, 0,
                                             h.request_id, NULL, 0);
            }
            /* CTMB_MSG_OUTPUT_REPORT / HOST_CONFIG: nothing to do. */
            free(payload);
        }

        pthread_mutex_lock(&g_hm.state_mutex);
        g_hm.connected = 0;
        pthread_mutex_unlock(&g_hm.state_mutex);
        ctm_transport_disconnect(&g_hm.xport);
        ctm_transport_destroy(&g_hm.xport);
    }
    pthread_mutex_lock(&g_hm.state_mutex);
    g_hm.connected = 0;
    pthread_mutex_unlock(&g_hm.state_mutex);
    return NULL;
}

int ctm_hostmouse_plug(const char *host, int port)
{
    if (g_hm.active) return -1;
    snprintf(g_hm.host, sizeof(g_hm.host), "%s", host);
    g_hm.port = port;
    g_hm.stop = 0;
    if (pthread_create(&g_hm.thread, NULL, hm_session_main, NULL) != 0) {
        hm_log("session thread create failed");
        return -1;
    }
    g_hm.thread_started = true;
    g_hm.active = 1;
    return 0;
}

void ctm_hostmouse_unplug(void)
{
    if (!g_hm.active) return;
    g_hm.stop = 1;
    /* Break a blocking TCP recv by closing the link from here. */
    ctm_transport_disconnect(&g_hm.xport);
    if (g_hm.thread_started) {
        pthread_join(g_hm.thread, NULL);
        g_hm.thread_started = false;
    }
    g_hm.active = 0;
    hm_log("unplugged");
}

bool ctm_hostmouse_active(void)
{
    return g_hm.active != 0;
}

bool ctm_hostmouse_connected(void)
{
    return g_hm.connected != 0;
}

void ctm_hostmouse_feed(int x, int y, int w, int h, unsigned buttons, int wheel_delta)
{
    if (!g_hm.connected || w <= 1 || h <= 1) return;
    if (x < 0) x = 0; if (x >= w) x = w - 1;
    if (y < 0) y = 0; if (y >= h) y = h - 1;
    uint16_t ax = (uint16_t)((uint32_t)x * 32767u / (uint32_t)(w - 1));
    uint16_t ay = (uint16_t)((uint32_t)y * 32767u / (uint32_t)(h - 1));
    int8_t wheel = (int8_t)(wheel_delta > 127 ? 127 : (wheel_delta < -127 ? -127 : wheel_delta));

    pthread_mutex_lock(&g_hm.state_mutex);
    bool changed = (ax != g_hm.last_x) || (ay != g_hm.last_y) ||
                   ((uint8_t)(buttons & 0x07) != g_hm.last_buttons) || wheel != 0;
    if (changed) {
        g_hm.last_x = ax;
        g_hm.last_y = ay;
        g_hm.last_buttons = (uint8_t)(buttons & 0x07);
    }
    pthread_mutex_unlock(&g_hm.state_mutex);
    if (!changed) return;

    uint8_t report[HM_REPORT_LEN];
    report[0] = 0x01; /* report ID */
    report[1] = (uint8_t)(buttons & 0x07);
    report[2] = (uint8_t)(ax & 0xFF);
    report[3] = (uint8_t)(ax >> 8);
    report[4] = (uint8_t)(ay & 0xFF);
    report[5] = (uint8_t)(ay >> 8);
    report[6] = (uint8_t)wheel;
    if (ctm_transport_send_msg(&g_hm.xport, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK,
                               0, report, sizeof(report)) != 0) {
        hm_log("report send failed");
    }
}

void ctm_hostmouse_feed_key(uint8_t hid_usage, bool down)
{
    if (!g_hm.connected || hid_usage == 0) return;

    pthread_mutex_lock(&g_hm.state_mutex);
    bool changed = false;
    if (down) {
        bool present = false;
        int freeSlot = -1;
        for (int i = 0; i < HM_KB_MAX_KEYS; ++i) {
            if (g_hm.keys[i] == hid_usage) present = true;
            else if (g_hm.keys[i] == 0 && freeSlot < 0) freeSlot = i;
        }
        if (!present && freeSlot >= 0) {
            g_hm.keys[freeSlot] = hid_usage;
            changed = true;
        }
    } else {
        for (int i = 0; i < HM_KB_MAX_KEYS; ++i) {
            if (g_hm.keys[i] == hid_usage) {
                g_hm.keys[i] = 0;
                changed = true;
            }
        }
    }
    uint8_t report[HM_KB_REPORT_LEN];
    report[0] = 0x02; /* report ID */
    report[1] = 0;    /* modifiers */
    report[2] = 0;    /* reserved */
    memcpy(report + 3, g_hm.keys, HM_KB_MAX_KEYS);
    pthread_mutex_unlock(&g_hm.state_mutex);
    if (!changed) return;

    if (ctm_transport_send_msg(&g_hm.xport, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK,
                               0, report, sizeof(report)) != 0) {
        hm_log("key report send failed");
    }
}
