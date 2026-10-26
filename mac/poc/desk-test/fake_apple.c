/* A pretend macOS for the desk test on Linux: just enough CoreFoundation and
 * IOKit HID behind the stand-in headers to run ds5_mac_bridge against a
 * pretend DualSense on a cable.
 *
 * Environment switches:
 *   FAKE_NO_PAD=1          no DualSense connected
 *   FAKE_DENY_SEIZE=1      opening it exclusively is refused (no Input Monitoring)
 *   FAKE_STRIP_INPUT_ID=1  input reports arrive without their report id
 *   FAKE_UNPLUG_MS=N       the pad is unplugged N ms into the run loop */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { T_STRING = 1, T_NUMBER, T_DATA, T_SET, T_OTHER };

struct __CFString { int tag; char s[256]; };
struct __CFNumber { int tag; int v; };
struct __CFData { int tag; CFIndex len; UInt8 bytes[512]; };
struct __CFSet { int tag; CFIndex count; const void *values[4]; };
struct __CFArray { int tag; };
struct __CFDictionary { int tag; };
struct __CFRunLoop { int tag; };
struct __CFRunLoopTimer { int tag; CFRunLoopTimerCallBack cb; };
struct __IOHIDManager { int tag; };
struct __IOHIDDevice {
    int tag;
    IOHIDReportCallback in_cb;
    void *in_ctx;
    uint8_t *in_buf;
    CFIndex in_len;
    IOHIDCallback rm_cb;
    void *rm_ctx;
    int scheduled;
};

static struct __CFString default_mode = {T_STRING, "kCFRunLoopDefaultMode"};
const CFAllocatorRef kCFAllocatorDefault = NULL;
const CFRunLoopMode kCFRunLoopDefaultMode = &default_mode;
const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
const CFArrayCallBacks kCFTypeArrayCallBacks;

static int env_on(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && v[0] == '1';
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- CoreFoundation ---- */

CFStringRef __CFStringMakeConstantString(const char *cStr)
{
    struct __CFString *s = calloc(1, sizeof(*s));
    s->tag = T_STRING;
    snprintf(s->s, sizeof(s->s), "%s", cStr);
    return s;
}

CFTypeRef CFRetain(CFTypeRef cf) { return cf; }
void CFRelease(CFTypeRef cf) { (void)cf; }
CFTypeID CFGetTypeID(CFTypeRef cf) { return cf ? (CFTypeID)*(const int *)cf : 0; }
CFTypeID CFNumberGetTypeID(void) { return T_NUMBER; }
CFTypeID CFStringGetTypeID(void) { return T_STRING; }
CFTypeID CFDataGetTypeID(void) { return T_DATA; }

CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex c, const CFDictionaryKeyCallBacks *k,
                                                 const CFDictionaryValueCallBacks *v)
{
    (void)a; (void)c; (void)k; (void)v;
    struct __CFDictionary *d = calloc(1, sizeof(*d));
    d->tag = T_OTHER;
    return d;
}

void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) { (void)d; (void)k; (void)v; }

CFNumberRef CFNumberCreate(CFAllocatorRef a, CFNumberType t, const void *p)
{
    (void)a; (void)t;
    struct __CFNumber *n = calloc(1, sizeof(*n));
    n->tag = T_NUMBER;
    n->v = *(const int *)p;
    return n;
}

Boolean CFNumberGetValue(CFNumberRef n, CFNumberType t, void *p)
{
    (void)t;
    *(int *)p = n->v;
    return 1;
}

Boolean CFStringGetCString(CFStringRef s, char *buf, CFIndex size, CFStringEncoding e)
{
    (void)e;
    if ((CFIndex)strlen(s->s) + 1 > size) return 0;
    strcpy(buf, s->s);
    return 1;
}

CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef a, CFIndex c, const CFArrayCallBacks *cb)
{
    (void)a; (void)c; (void)cb;
    struct __CFArray *arr = calloc(1, sizeof(*arr));
    arr->tag = T_OTHER;
    return arr;
}

void CFArrayAppendValue(CFMutableArrayRef a, const void *v) { (void)a; (void)v; }
CFIndex CFSetGetCount(CFSetRef s) { return s->count; }
void CFSetGetValues(CFSetRef s, const void **values) { memcpy(values, s->values, (size_t)s->count * sizeof(*values)); }
const UInt8 *CFDataGetBytePtr(CFDataRef d) { return d->bytes; }
CFIndex CFDataGetLength(CFDataRef d) { return d->len; }

static struct __CFRunLoop the_loop = {T_OTHER};
static volatile int loop_stopped;
static CFRunLoopTimerRef the_timer;
static struct __IOHIDDevice the_pad = {T_OTHER, NULL, NULL, NULL, 0, NULL, NULL, 0};

CFRunLoopRef CFRunLoopGetCurrent(void) { return &the_loop; }
void CFRunLoopStop(CFRunLoopRef rl) { (void)rl; loop_stopped = 1; }
CFAbsoluteTime CFAbsoluteTimeGetCurrent(void) { return now_s(); }

CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef a, CFAbsoluteTime fire, CFTimeInterval interval,
                                       CFOptionFlags f, CFIndex o, CFRunLoopTimerCallBack cb,
                                       CFRunLoopTimerContext *ctx)
{
    (void)a; (void)fire; (void)interval; (void)f; (void)o; (void)ctx;
    struct __CFRunLoopTimer *t = calloc(1, sizeof(*t));
    t->tag = T_OTHER;
    t->cb = cb;
    return t;
}

void CFRunLoopAddTimer(CFRunLoopRef rl, CFRunLoopTimerRef t, CFRunLoopMode m) { (void)rl; (void)m; the_timer = t; }
void CFRunLoopRemoveTimer(CFRunLoopRef rl, CFRunLoopTimerRef t, CFRunLoopMode m) { (void)rl; (void)t; (void)m; the_timer = NULL; }

/* A pad on USB reports every 4 ms; the timer fires every 200 ms. */
void CFRunLoopRun(void)
{
    loop_stopped = 0;
    const char *unplug = getenv("FAKE_UNPLUG_MS");
    const double unplug_at = unplug ? now_s() + atoi(unplug) / 1000.0 : 0;
    int unplugged = 0;
    unsigned tick = 0;
    uint8_t report[64];
    while (!loop_stopped) {
        usleep(4000);
        ++tick;
        if (unplug && !unplugged && now_s() >= unplug_at) {
            unplugged = 1;
            fprintf(stderr, "fake: the pad is unplugged\n");
            if (the_pad.rm_cb) the_pad.rm_cb(the_pad.rm_ctx, kIOReturnSuccess, &the_pad);
        }
        if (!unplugged && the_pad.scheduled && the_pad.in_cb) {
            memset(report, 0, sizeof(report));
            report[0] = 0x01;
            report[1] = report[2] = report[3] = report[4] = 0x80;   /* sticks at rest */
            report[7] = (uint8_t)tick;
            if (env_on("FAKE_STRIP_INPUT_ID")) {
                memcpy(the_pad.in_buf, report + 1, 63);
                the_pad.in_cb(the_pad.in_ctx, kIOReturnSuccess, &the_pad, kIOHIDReportTypeInput, 0x01,
                              the_pad.in_buf, 63);
            } else {
                memcpy(the_pad.in_buf, report, 64);
                the_pad.in_cb(the_pad.in_ctx, kIOReturnSuccess, &the_pad, kIOHIDReportTypeInput, 0x01,
                              the_pad.in_buf, 64);
            }
        }
        if (tick % 50 == 0 && the_timer) the_timer->cb(the_timer, NULL);
    }
}

/* ---- IOKit HID ---- */

static const UInt8 descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
    0x85, 0x01, 0x75, 0x08, 0x95, 0x3f, 0x81, 0x02,   /* input 0x01, 63 bytes after the id */
    0x85, 0x02, 0x95, 0x2f, 0x91, 0x02,               /* output 0x02, 47 */
    0x85, 0x05, 0x95, 0x28, 0xb1, 0x02,               /* feature 0x05, 40 */
    0x85, 0x09, 0x95, 0x13, 0xb1, 0x02,               /* feature 0x09, 19 */
    0x85, 0x20, 0x95, 0x3f, 0xb1, 0x02,               /* feature 0x20, 63 */
    0xc0
};

IOHIDManagerRef IOHIDManagerCreate(CFAllocatorRef a, IOOptionBits o)
{
    (void)a; (void)o;
    struct __IOHIDManager *m = calloc(1, sizeof(*m));
    m->tag = T_OTHER;
    return m;
}

void IOHIDManagerSetDeviceMatchingMultiple(IOHIDManagerRef m, CFArrayRef a) { (void)m; (void)a; }

CFSetRef IOHIDManagerCopyDevices(IOHIDManagerRef m)
{
    (void)m;
    struct __CFSet *s = calloc(1, sizeof(*s));
    s->tag = T_SET;
    if (!env_on("FAKE_NO_PAD")) {
        s->count = 1;
        s->values[0] = &the_pad;
    }
    return s;
}

IOReturn IOHIDDeviceOpen(IOHIDDeviceRef d, IOOptionBits o)
{
    (void)d;
    if ((o & kIOHIDOptionsTypeSeizeDevice) && env_on("FAKE_DENY_SEIZE")) {
        fprintf(stderr, "fake: open exclusively REFUSED\n");
        return kIOReturnNotPrivileged;
    }
    fprintf(stderr, "fake: opened %s\n", (o & kIOHIDOptionsTypeSeizeDevice) ? "exclusively" : "shared");
    return kIOReturnSuccess;
}

IOReturn IOHIDDeviceClose(IOHIDDeviceRef d, IOOptionBits o)
{
    (void)d; (void)o;
    fprintf(stderr, "fake: closed\n");
    return kIOReturnSuccess;
}

CFTypeRef IOHIDDeviceGetProperty(IOHIDDeviceRef d, CFStringRef key)
{
    (void)d;
    static struct __CFString transport = {T_STRING, "USB"};
    static struct __CFString maker = {T_STRING, "Sony Interactive Entertainment"};
    static struct __CFString product = {T_STRING, "DualSense Wireless Controller"};
    static struct __CFNumber vid = {T_NUMBER, 0x054c};
    static struct __CFNumber pid = {T_NUMBER, 0x0ce6};
    static struct __CFData desc = {T_DATA, 0, {0}};
    if (desc.len == 0) {
        desc.len = (CFIndex)sizeof(descriptor);
        memcpy(desc.bytes, descriptor, sizeof(descriptor));
    }
    if (strcmp(key->s, "Transport") == 0) return &transport;
    if (strcmp(key->s, "Manufacturer") == 0) return &maker;
    if (strcmp(key->s, "Product") == 0) return &product;
    if (strcmp(key->s, "VendorID") == 0) return &vid;
    if (strcmp(key->s, "ProductID") == 0) return &pid;
    if (strcmp(key->s, "ReportDescriptor") == 0) return &desc;
    return NULL;   /* no serial number, as a cabled DualSense on macOS may have none */
}

IOReturn IOHIDDeviceSetReport(IOHIDDeviceRef d, IOHIDReportType type, CFIndex id, const uint8_t *report, CFIndex len)
{
    (void)d;
    fprintf(stderr, "fake: SetReport %s id=0x%02lx %ld bytes, the first 0x%02x\n",
            type == kIOHIDReportTypeOutput ? "output" : "feature", (long)id, (long)len, len > 0 ? report[0] : 0);
    return kIOReturnSuccess;
}

IOReturn IOHIDDeviceGetReport(IOHIDDeviceRef d, IOHIDReportType type, CFIndex id, uint8_t *report, CFIndex *len)
{
    (void)d; (void)type;
    if (id == 0x09) {
        static const uint8_t pairing[20] = {0x09, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x08, 0x25, 0x00};
        const CFIndex n = *len < 20 ? *len : 20;
        memcpy(report, pairing, (size_t)n);
        *len = n;
    } else {
        report[0] = (uint8_t)id;
        for (CFIndex i = 1; i < *len; ++i) report[i] = (uint8_t)i;
    }
    fprintf(stderr, "fake: GetReport feature id=0x%02lx %ld bytes\n", (long)id, (long)*len);
    return kIOReturnSuccess;
}

void IOHIDDeviceRegisterInputReportCallback(IOHIDDeviceRef d, uint8_t *buf, CFIndex len, IOHIDReportCallback cb, void *ctx)
{
    d->in_buf = buf;
    d->in_len = len;
    d->in_cb = cb;
    d->in_ctx = ctx;
}

void IOHIDDeviceRegisterRemovalCallback(IOHIDDeviceRef d, IOHIDCallback cb, void *ctx)
{
    d->rm_cb = cb;
    d->rm_ctx = ctx;
}

void IOHIDDeviceScheduleWithRunLoop(IOHIDDeviceRef d, CFRunLoopRef rl, CFStringRef m) { (void)rl; (void)m; d->scheduled = 1; }
void IOHIDDeviceUnscheduleFromRunLoop(IOHIDDeviceRef d, CFRunLoopRef rl, CFStringRef m) { (void)rl; (void)m; d->scheduled = 0; }

/* ---- macOS's libc ---- */

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t n = strlen(src);
    if (size > 0) {
        const size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    const size_t used = strnlen(dst, size);
    if (used == size) return size + strlen(src);
    return used + strlcpy(dst + used, src, size - used);
}
