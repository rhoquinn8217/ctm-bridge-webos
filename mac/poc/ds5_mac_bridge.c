/* A proof of concept: a DualSense on a Mac's USB port, bridged to DS5-USBIP
 * on the gaming PC the way the TV bridges one.
 *
 * It speaks the TV's own protocol through the core's shared transport
 * (src/shared/ctm_transport.c, unchanged). The listener's agent, on port
 * 48054, answers BRIDGE_START with a port for this bridge; HELLO carries the
 * pad's details and its own HID report descriptor; the listener answers with
 * HOST_CONFIG; then input reports go to the PC while output and feature
 * reports come back to the pad. A client never speaks USB/IP: the listener
 * does, on the PC.
 *
 * ⛔ The bridge is asked for as "ds5_usb" ("ds5e_usb" for an Edge), as the TV
 * asks for a cabled pad (bridge_kind_for_item). "ds5" is the Bluetooth pad:
 * the listener would read this pad's USB reports through its Bluetooth map,
 * and nothing would parse.
 *
 * What the TV's core does and this does not, yet: Bluetooth, the pad's sound
 * and microphone, the gestures and signals, reconnecting, and the pairing
 * proof the listener asks of a caller from the internet. It is for a home
 * network.
 *
 * The pad is opened exclusively ("seized"), so macOS and any game on the Mac
 * leave it alone while it is bridged. macOS may ask for Input Monitoring the
 * first time; when the pad cannot be held, it is opened shared, as
 * --no-seize opens it from the start.
 *
 * Reports travel with their report id in front, as Linux's hidraw gives them
 * and as hidapi hands them to IOKit. --strip-id passes IOHIDDeviceSetReport
 * the report without its id byte instead, as Twilight's dualsense_hid_mac.mm
 * does. The first run on a Mac says which of the two the pad takes. */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ctm_bridge_protocol.h"
#include "ctm_hid.h"
#include "ctm_transport.h"
#include "agent_reply.inl"
#include "protocol_level.inl"
#include "session_name.inl"

#define SONY_VID 0x054C
#define DS5_PID 0x0CE6
#define DS5_EDGE_PID 0x0DF2
#define AGENT_PORT_DEFAULT 48054
#define HOST_ANSWER_MS 5000        /* as long as the TV waits (CTM_HOST_ANSWER_US) */
#define MAX_REPORT 1024
#define FEATURE_LOG_MAX 20         /* the PC's first feature requests go to the log, then only failures */

static struct {
    const char *pc;
    int agent_port;
    bool seize;
    bool strip_id;
    IOHIDDeviceRef dev;
    pthread_mutex_t dev_mutex;
    ctm_transport_t xport;
    volatile sig_atomic_t stop;
    volatile int link_lost;
    char busid[64];
    uint8_t in_buf[MAX_REPORT];
    int input_has_id;              /* -1 not known yet, 0 IOKit leaves it off, 1 it is there */
    unsigned long inputs;
    unsigned long outputs;
    unsigned long features;
    unsigned long ignored;
} g;

static void say(const char *fmt, ...)
{
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03d  ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* The IOKit answers this program can meet, in words. */
static const char *ioreturn_text(IOReturn r)
{
    switch (r) {
    case kIOReturnSuccess: return "ok";
    case kIOReturnNotPrivileged: return "not privileged (macOS may want Input Monitoring for this terminal)";
    case kIOReturnExclusiveAccess: return "exclusive access (another program holds the pad)";
    case kIOReturnNotPermitted: return "not permitted";
    case kIOReturnNoDevice: return "no such device";
    case kIOReturnTimeout: return "timed out";
    case kIOReturnBadArgument: return "bad argument";
    default: return "failed";
    }
}

/* ---- the listener's agent: one command per connection, as the TV's
 * send_agent_line does (src/app/ui_bridge.c). ---- */

static int agent_command(const char *command, char *reply, size_t reply_len)
{
    reply[0] = '\0';
    ctm_transport_t t;
    ctm_transport_init(&t);
    if (ctm_transport_connect_within(&t, g.pc, g.agent_port, 2000) != 0) {
        ctm_transport_destroy(&t);
        return agent_command_rc(false, false, 0, NULL);
    }
    struct timeval tv = {2, 0};
    setsockopt(t.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char line[512];
    const int len = snprintf(line, sizeof(line), "%s\n", command);
    const bool sent = len > 0 && send(t.fd, line, (size_t)len, 0) == (ssize_t)len;
    ssize_t n = sent ? recv(t.fd, reply, reply_len - 1, 0) : -1;
    ctm_transport_disconnect(&t);
    ctm_transport_destroy(&t);
    reply[n > 0 ? n : 0] = '\0';
    size_t l = strlen(reply);
    while (l > 0 && (reply[l - 1] == '\n' || reply[l - 1] == '\r')) {
        reply[--l] = '\0';
    }
    return agent_command_rc(true, sent, n, reply);
}

/* ---- the pad, through IOKit ---- */

static CFMutableDictionaryRef match_for(int vid, int pid)
{
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                         &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    CFDictionarySetValue(d, CFSTR(kIOHIDVendorIDKey), v);
    CFDictionarySetValue(d, CFSTR(kIOHIDProductIDKey), p);
    CFRelease(v);
    CFRelease(p);
    return d;
}

static int prop_int(IOHIDDeviceRef dev, CFStringRef key)
{
    int out = 0;
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
    if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &out);
    }
    return out;
}

static void prop_str(IOHIDDeviceRef dev, CFStringRef key, char *out, size_t len)
{
    out[0] = '\0';
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
    if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID()) {
        if (!CFStringGetCString((CFStringRef)v, out, (CFIndex)len, kCFStringEncodingUTF8)) {
            out[0] = '\0';
        }
    }
}

static bool on_usb(IOHIDDeviceRef dev)
{
    char transport[64];
    prop_str(dev, CFSTR(kIOHIDTransportKey), transport, sizeof(transport));
    return strcmp(transport, "USB") == 0;
}

/* A DualSense or a DualSense Edge on a cable. Not the manager's Open: that
 * would open every pad it matched, shared, before this one is seized. */
static IOHIDDeviceRef find_dualsense(IOHIDManagerRef manager)
{
    CFMutableArrayRef matches = CFArrayCreateMutable(kCFAllocatorDefault, 2, &kCFTypeArrayCallBacks);
    CFMutableDictionaryRef ds5 = match_for(SONY_VID, DS5_PID);
    CFMutableDictionaryRef edge = match_for(SONY_VID, DS5_EDGE_PID);
    CFArrayAppendValue(matches, ds5);
    CFArrayAppendValue(matches, edge);
    CFRelease(ds5);
    CFRelease(edge);
    IOHIDManagerSetDeviceMatchingMultiple(manager, matches);
    CFRelease(matches);

    CFSetRef set = IOHIDManagerCopyDevices(manager);
    if (set == NULL) {
        return NULL;
    }
    IOHIDDeviceRef found = NULL;
    const CFIndex n = CFSetGetCount(set);
    if (n > 0) {
        const void **devs = calloc((size_t)n, sizeof(*devs));
        if (devs != NULL) {
            CFSetGetValues(set, devs);
            for (CFIndex i = 0; i < n && found == NULL; ++i) {
                if (on_usb((IOHIDDeviceRef)devs[i])) {
                    found = (IOHIDDeviceRef)devs[i];
                }
            }
            free(devs);
        }
        if (found == NULL) {
            say("a DualSense is connected, but not by cable: this proof of concept bridges a cabled one only");
        } else {
            CFRetain(found);
        }
    }
    CFRelease(set);
    return found;
}

/* Output and feature reports to the pad. `data` starts with its report id.
 * hidapi's way, the default: the whole report, id included, with the id
 * also given apart. --strip-id: the report after its id. */
static IOReturn dev_set(IOHIDReportType type, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return kIOReturnBadArgument;
    }
    const uint8_t id = data[0];
    const uint8_t *buf = data;
    CFIndex n = (CFIndex)len;
    if (id == 0 || g.strip_id) {
        buf = data + 1;
        n -= 1;
    }
    pthread_mutex_lock(&g.dev_mutex);
    const IOReturn r = IOHIDDeviceSetReport(g.dev, type, id, buf, n);
    pthread_mutex_unlock(&g.dev_mutex);
    return r;
}

/* A feature report from the pad. buf[0] holds the id going in; coming back
 * it holds the report, id first (as hidapi reads it). */
static IOReturn dev_get_feature(uint8_t *buf, size_t len, CFIndex *got)
{
    CFIndex n = (CFIndex)len;
    pthread_mutex_lock(&g.dev_mutex);
    const IOReturn r = IOHIDDeviceGetReport(g.dev, kIOHIDReportTypeFeature, buf[0], buf, &n);
    pthread_mutex_unlock(&g.dev_mutex);
    *got = n;
    return r;
}

/* The pad's own MAC from its pairing report 0x09, as the TV's
 * probe_pairing_info reads it (src/controllers/controller_common.c): bytes 6
 * down to 1. The bytes go to the log, so the first run on a Mac shows whether
 * IOKit put the id in front as expected. */
static bool read_mac(char *out, size_t len)
{
    uint8_t f[20];
    memset(f, 0, sizeof(f));
    f[0] = 0x09;
    CFIndex got = 0;
    const IOReturn r = dev_get_feature(f, sizeof(f), &got);
    if (r != kIOReturnSuccess) {
        say("feature 0x09 (pairing info): %s (0x%08x)", ioreturn_text(r), (unsigned int)r);
        return false;
    }
    say("feature 0x09 (pairing info): %ld bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
        (long)got, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    bool zero = true;
    for (int i = 1; i <= 6; ++i) {
        if (f[i] != 0) {
            zero = false;
        }
    }
    if (zero) {
        say("feature 0x09 answered with zeroes: no MAC for the PC");
        return false;
    }
    snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x", f[6], f[5], f[4], f[3], f[2], f[1]);
    return true;
}

/* ---- the bridge's own connection ---- */

static int send_hello(const ctmb_device_caps_t *caps, const uint8_t *desc, uint32_t desc_len)
{
    ctmb_hid_descriptor_info_t info;
    memset(&info, 0, sizeof(info));
    info.report_descriptor_len = desc_len;
    const size_t len = sizeof(*caps) + sizeof(info) + desc_len;
    uint8_t *hello = malloc(len);
    if (hello == NULL) {
        return -1;
    }
    memcpy(hello, caps, sizeof(*caps));
    memcpy(hello + sizeof(*caps), &info, sizeof(info));
    if (desc_len > 0) {
        memcpy(hello + sizeof(*caps) + sizeof(info), desc, desc_len);
    }
    const int rc = ctm_transport_send_msg(&g.xport, CTMB_MSG_HELLO, CTMB_FLAG_OK, 0, hello, len);
    free(hello);
    return rc;
}

/* The listener answers HELLO with its settings for this pad; the TV waits
 * for them before it sends any input (run_session's handshake). */
static int wait_host_config(ctmb_host_config_t *cfg)
{
    const uint64_t start = now_ms();
    for (;;) {
        if (now_ms() - start >= HOST_ANSWER_MS) {
            say("the listener sent no settings within %d s", HOST_ANSWER_MS / 1000);
            return -1;
        }
        const int ready = ctm_transport_wait_readable(&g.xport, 100);
        if (ready < 0) {
            say("the link dropped while waiting for the listener's settings");
            return -1;
        }
        if (ready == 0) {
            continue;
        }
        ctmb_header_t h;
        uint8_t *payload = NULL;
        if (ctm_transport_recv_msg(&g.xport, &h, &payload) < 0) {
            say("receiving the listener's settings failed");
            return -1;
        }
        if (h.type == CTMB_MSG_HOST_CONFIG && h.payload_len >= sizeof(*cfg)) {
            memcpy(cfg, payload, sizeof(*cfg));
            free(payload);
            return 0;
        }
        say("message type %u (%u bytes) came before the listener's settings", h.type, h.payload_len);
        free(payload);
        return -1;
    }
}

static void on_input(void *context, IOReturn result, void *sender, IOHIDReportType type,
                     uint32_t report_id, uint8_t *report, CFIndex report_len)
{
    (void)context;
    (void)sender;
    (void)type;
    if (result != kIOReturnSuccess || report_len <= 0 || g.stop || g.link_lost) {
        return;
    }
    /* The id goes in front, as hidraw reads it. Whether IOKit leaves it there
     * is decided once, on the first report, when the sticks are still. */
    if (g.input_has_id < 0) {
        g.input_has_id = (report_id == 0 || report[0] == (uint8_t)report_id) ? 1 : 0;
        say("input reports from IOKit %s their report id", g.input_has_id ? "carry" : "leave off");
    }
    const uint8_t *out = report;
    size_t n = (size_t)report_len;
    uint8_t with_id[MAX_REPORT + 1];
    if (!g.input_has_id) {
        if (n + 1 > sizeof(with_id)) {
            return;
        }
        with_id[0] = (uint8_t)report_id;
        memcpy(with_id + 1, report, n);
        out = with_id;
        n += 1;
    }
    if (ctm_transport_send_msg(&g.xport, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, 0, out, n) != 0) {
        g.link_lost = 1;
        return;
    }
    if (++g.inputs == 1) {
        say("first input report sent to the PC: id 0x%02x, %zu bytes", out[0], n);
    }
}

static void handle(const ctmb_header_t *h, const uint8_t *payload)
{
    switch (h->type) {
    case CTMB_MSG_OUTPUT_REPORT:
        if (h->payload_len > 0) {
            const IOReturn r = dev_set(kIOHIDReportTypeOutput, payload, h->payload_len);
            ++g.outputs;
            if (g.outputs == 1 || r != kIOReturnSuccess) {
                say("output report 0x%02x (%u bytes) to the pad: %s (0x%08x)%s", payload[0], h->payload_len,
                    ioreturn_text(r), (unsigned int)r, g.outputs == 1 ? ", the first" : "");
            }
        }
        break;
    case CTMB_MSG_FEATURE_GET: {
        bool ok = false;
        if (h->payload_len > 0 && h->payload_len <= MAX_REPORT) {
            uint8_t f[MAX_REPORT];
            memcpy(f, payload, h->payload_len);
            CFIndex got = 0;
            const IOReturn r = dev_get_feature(f, h->payload_len, &got);
            /* As the TV answers: as many bytes as were asked for, id first. */
            if (r == kIOReturnSuccess) {
                ok = ctm_transport_send_msg(&g.xport, CTMB_MSG_FEATURE_REPORT, CTMB_FLAG_OK, h->request_id,
                                            f, h->payload_len) == 0;
            }
            if (g.features < FEATURE_LOG_MAX || !ok) {
                say("feature 0x%02x asked for by the PC (%u bytes): %s", payload[0], h->payload_len,
                    ok ? "answered" : ioreturn_text(r));
            }
        }
        if (!ok) {
            (void)ctm_transport_send_msg(&g.xport, CTMB_MSG_FEATURE_REPORT, 0, h->request_id, NULL, 0);
        }
        ++g.features;
        break;
    }
    case CTMB_MSG_FEATURE_SET: {
        IOReturn r = kIOReturnBadArgument;
        if (h->payload_len > 0) {
            r = dev_set(kIOHIDReportTypeFeature, payload, h->payload_len);
        }
        if (g.features < FEATURE_LOG_MAX || r != kIOReturnSuccess) {
            say("feature 0x%02x set by the PC (%u bytes): %s", h->payload_len > 0 ? payload[0] : 0,
                h->payload_len, ioreturn_text(r));
        }
        (void)ctm_transport_send_msg(&g.xport, CTMB_MSG_FEATURE_REPORT,
                                     r == kIOReturnSuccess ? CTMB_FLAG_OK : 0, h->request_id, NULL, 0);
        ++g.features;
        break;
    }
    case CTMB_MSG_HOST_CONFIG:
        break;   /* a later change of the listener's settings: nothing on a cabled pad uses them yet */
    default:
        if (++g.ignored == 1) {
            say("message type %u from the PC left alone (the pad's sound is not bridged yet)", h->type);
        }
        break;
    }
}

static void *net_main(void *arg)
{
    (void)arg;
    while (!g.stop) {
        const int ready = ctm_transport_wait_readable(&g.xport, 200);
        if (ready < 0) {
            g.link_lost = 1;
            break;
        }
        if (ready == 0) {
            continue;
        }
        ctmb_header_t h;
        uint8_t *payload = NULL;
        if (ctm_transport_recv_msg(&g.xport, &h, &payload) < 0) {
            g.link_lost = 1;
            break;
        }
        handle(&h, payload);
        free(payload);
    }
    return NULL;
}

/* ---- the program ---- */

static void on_signal(int sig)
{
    (void)sig;
    g.stop = 1;
}

static void on_removed(void *context, IOReturn result, void *sender)
{
    (void)context;
    (void)result;
    (void)sender;
    say("the pad was unplugged");
    g.stop = 1;
}

static void watch(CFRunLoopTimerRef timer, void *info)
{
    (void)timer;
    (void)info;
    if (g.stop || g.link_lost) {
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
}

static void usage(const char *me)
{
    fprintf(stderr,
            "usage: %s <the gaming PC's address> [--agent-port N] [--no-seize] [--strip-id]\n"
            "  Bridges a DualSense on this Mac's USB port to DS5-USBIP on the PC.\n"
            "  Ctrl+C releases it.\n",
            me);
}

int main(int argc, char **argv)
{
    memset(&g, 0, sizeof(g));
    g.agent_port = AGENT_PORT_DEFAULT;
    g.seize = true;
    g.input_has_id = -1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--agent-port") == 0 && i + 1 < argc) {
            g.agent_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-seize") == 0) {
            g.seize = false;
        } else if (strcmp(argv[i], "--strip-id") == 0) {
            g.strip_id = true;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (g.pc == NULL) {
            g.pc = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (g.pc == NULL || g.agent_port <= 0) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);   /* the transport sends without MSG_NOSIGNAL here */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    pthread_mutex_init(&g.dev_mutex, NULL);
    ctm_transport_init(&g.xport);
    int status = 1;

    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    g.dev = find_dualsense(manager);
    if (g.dev == NULL) {
        say("no DualSense found on a cable: connect it to this Mac and run this again");
        CFRelease(manager);
        return 1;
    }
    IOReturn r = IOHIDDeviceOpen(g.dev, g.seize ? kIOHIDOptionsTypeSeizeDevice : kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess && g.seize) {
        say("the pad could not be held for this program alone: %s (0x%08x)", ioreturn_text(r), (unsigned int)r);
        say("allow this terminal in System Settings, Privacy & Security, Input Monitoring, and run this again;"
            " trying it shared for now");
        g.seize = false;
        r = IOHIDDeviceOpen(g.dev, kIOHIDOptionsTypeNone);
    }
    if (r != kIOReturnSuccess) {
        say("the pad could not be opened: %s (0x%08x)", ioreturn_text(r), (unsigned int)r);
        CFRelease(g.dev);
        CFRelease(manager);
        return 1;
    }

    char manufacturer[64];
    char product[64];
    char serial_prop[64];
    char mac[24] = "";
    prop_str(g.dev, CFSTR(kIOHIDManufacturerKey), manufacturer, sizeof(manufacturer));
    prop_str(g.dev, CFSTR(kIOHIDProductKey), product, sizeof(product));
    prop_str(g.dev, CFSTR(kIOHIDSerialNumberKey), serial_prop, sizeof(serial_prop));
    CFTypeRef rd = IOHIDDeviceGetProperty(g.dev, CFSTR(kIOHIDReportDescriptorKey));
    const UInt8 *desc = NULL;
    uint32_t desc_len = 0;
    if (rd != NULL && CFGetTypeID(rd) == CFDataGetTypeID()) {
        desc = CFDataGetBytePtr((CFDataRef)rd);
        desc_len = (uint32_t)CFDataGetLength((CFDataRef)rd);
    }
    (void)read_mac(mac, sizeof(mac));
    say("found %s, %04x:%04x, on a cable, opened %s; MAC %s (IOKit's serial: %s); report descriptor %u bytes",
        product[0] ? product : "a DualSense", prop_int(g.dev, CFSTR(kIOHIDVendorIDKey)),
        prop_int(g.dev, CFSTR(kIOHIDProductIDKey)), g.seize ? "for this program alone" : "shared",
        mac[0] ? mac : "none", serial_prop[0] ? serial_prop : "none", desc_len);

    /* What the TV sends for a cabled pad (open_hidraw in controller_common.c):
     * bus 3 is USB as Linux numbers it, and the listener names the pad's link
     * by it; the lengths come from the descriptor with the TV's floor of 1024;
     * the flag, and "hidraw" as the maker, are the TV's. The path is only
     * logged, unless it reads "virtual". */
    ctmb_device_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.vendor_id = (uint16_t)prop_int(g.dev, CFSTR(kIOHIDVendorIDKey));
    caps.product_id = (uint16_t)prop_int(g.dev, CFSTR(kIOHIDProductIDKey));
    caps.bus = 3;
    caps.input_report_len = 1024;
    caps.output_report_len = 1024;
    caps.feature_report_len = 64;
    caps.flags = 1;
    if (desc_len > 0) {
        derive_report_lengths(desc, desc_len, &caps);
        if (caps.input_report_len < 1024) caps.input_report_len = 1024;
        if (caps.output_report_len < 1024) caps.output_report_len = 1024;
    }
    snprintf(caps.path, sizeof(caps.path), "iokit");
    snprintf(caps.serial, sizeof(caps.serial), "%s", mac);
    /* The name Linux gives a USB device, which is what the TV sends: the
     * maker and the product, cut to fit (usbhid_probe, HIDIOCGRAWNAME). */
    strlcpy(caps.product, manufacturer, sizeof(caps.product));
    if (product[0]) {
        if (manufacturer[0]) {
            strlcat(caps.product, " ", sizeof(caps.product));
        }
        strlcat(caps.product, product, sizeof(caps.product));
    }
    if (caps.product[0] == '\0') {
        snprintf(caps.product, sizeof(caps.product), "HID %04x:%04x", caps.vendor_id, caps.product_id);
    }
    snprintf(caps.manufacturer, sizeof(caps.manufacturer), "hidraw");

    char reply[256];
    char cmd[200];
    int rc = agent_command("STATUS", reply, sizeof(reply));
    if (rc == -2) {
        say("DS5-USBIP did not answer at %s port %d: is it running on the PC, and is that the PC's address?",
            g.pc, g.agent_port);
        goto close_pad;
    }
    say("DS5-USBIP: %s", reply[0] ? reply : "(no status)");
    const protocol_compat_t compat = protocol_compat(reply);
    if (compat == PROTOCOL_UPDATE_PC) {
        say("DS5-USBIP on the PC is older than this program's protocol: update it");
        goto close_pad;
    }
    if (compat == PROTOCOL_UPDATE_TV) {
        say("DS5-USBIP on the PC no longer works with this program's protocol: rebuild it from a newer core");
        goto close_pad;
    }

    const char *kind = caps.product_id == DS5_EDGE_PID ? "ds5e_usb" : "ds5_usb";
    char tag[SESSION_RUN_TAG_LEN + 1];
    session_run_tag_from(arc4random(), tag);
    session_name_make(g.busid, sizeof(g.busid), kind, 1, tag);
    snprintf(cmd, sizeof(cmd), "BRIDGE_START %s 0 %s level=%d", kind, g.busid, CTM_PROTOCOL_LEVEL);
    rc = agent_command(cmd, reply, sizeof(reply));
    const int port = rc == 0 ? agent_reply_port(reply) : 0;
    if (port == 0) {
        /* As the TV does: an answer with no port, or no answer, may still
         * have started one, so it is stopped. */
        say("DS5-USBIP did not start a bridge: \"%s\"", reply[0] ? reply : "no answer");
        goto stop_bridge;
    }
    say("bridge %s on port %d", g.busid, port);

    if (ctm_transport_connect_within(&g.xport, g.pc, port, 3000) != 0) {
        say("the bridge's port %d did not answer", port);
        goto stop_bridge;
    }
    if (send_hello(&caps, desc, desc_len) != 0) {
        say("HELLO could not be sent");
        goto stop_bridge;
    }
    ctmb_host_config_t cfg;
    if (wait_host_config(&cfg) != 0) {
        goto stop_bridge;
    }
    ctm_transport_set_ready(&g.xport, 1);
    say("bridged: the PC should now see a DualSense. Ctrl+C releases it.");

    pthread_t net;
    if (pthread_create(&net, NULL, net_main, NULL) != 0) {
        say("the receiving thread could not start");
        goto stop_bridge;
    }
    IOHIDDeviceRegisterInputReportCallback(g.dev, g.in_buf, (CFIndex)sizeof(g.in_buf), on_input, NULL);
    IOHIDDeviceRegisterRemovalCallback(g.dev, on_removed, NULL);
    IOHIDDeviceScheduleWithRunLoop(g.dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.2, 0.2,
                                                   0, 0, watch, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
    CFRunLoopRun();

    say("%s", g.link_lost ? "the link to the PC dropped" : "releasing");
    CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
    CFRelease(timer);
    IOHIDDeviceUnscheduleFromRunLoop(g.dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDDeviceRegisterInputReportCallback(g.dev, g.in_buf, (CFIndex)sizeof(g.in_buf), NULL, NULL);
    IOHIDDeviceRegisterRemovalCallback(g.dev, NULL, NULL);
    g.stop = 1;
    pthread_join(net, NULL);
    say("input reports to the PC: %lu; output reports to the pad: %lu; feature requests: %lu",
        g.inputs, g.outputs, g.features);
    status = g.link_lost ? 1 : 0;

stop_bridge:
    ctm_transport_disconnect(&g.xport);
    snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", g.busid);
    (void)agent_command(cmd, reply, sizeof(reply));
    say("released: %s", reply[0] ? reply : "(no answer)");
close_pad:
    ctm_transport_destroy(&g.xport);
    IOHIDDeviceClose(g.dev, kIOHIDOptionsTypeNone);
    CFRelease(g.dev);
    CFRelease(manager);
    return status;
}
