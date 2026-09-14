/* Which identity a device is known by: what the host auto-links a config on,
 * and what the TV's auto bridge marks.
 *
 * ⭐⭐ THE RULES, decided 2026-09-13 (rhoquinn8217):
 *   1. A DualSense or DualSense Edge is its own MAC, even through a dongle.
 *   2. Every other controller is its serial -- the kernel's uniq, or the USB
 *      serial number where no driver filled uniq -- and a blank or all-zeros
 *      serial is no identity at all.
 *   3. A device that is not a controller has none.
 *
 * ⓘ What that serial holds, measured on the U5s the same day: over Bluetooth
 * the MAC; on a cable the USB serial number, which the HID driver copies into
 * uniq; and for a pad the Xbox driver runs, which fills no uniq at all, the USB
 * device's own serial attribute.
 *
 * ⛔ An Xbox pad has no MAC to give over USB. Its GIP hello carries a fixed ID
 * that is not its Bluetooth MAC (captured 2026-09-13), and only the Xbox driver
 * sees that message anyway.
 *
 * Pure: strings in, strings out, so tests/test_device_identity.c checks it. */

#ifndef DEVICE_IDENTITY_INL
#define DEVICE_IDENTITY_INL

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static inline int identity_is_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* An identity only if some letter or digit is not a zero.
 *
 * ⛔ "00:00:00:00:00:00" would otherwise be shared by every device that
 * reports nothing, and every one of them would link to the same config. */
static inline int identity_usable(const char *s)
{
    if (!s) return 0;
    for (; *s; ++s) {
        if (identity_is_alnum(*s) && *s != '0') return 1;
    }
    return 0;
}

/* Letters lower-cased, digits kept, everything else dropped -- what the
 * listener's normalise_serial() does -- so SDL's "7c-66-ef-82-10-ed" and the
 * kernel's "7c:66:ef:82:10:ed" are one identity. */
static inline void identity_normalise(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    if (!out || out_len == 0) return;
    for (; in && *in && o + 1 < out_len; ++in) {
        char c = *in;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (identity_is_alnum(c)) out[o++] = c;
    }
    out[o] = '\0';
}

/* The same identity however it is punctuated. Two unusable ones are never the
 * same: nothing may be matched on an absence. */
static inline int identity_same(const char *a, const char *b)
{
    char na[128], nb[128];
    if (!identity_usable(a) || !identity_usable(b)) return 0;
    identity_normalise(a, na, sizeof(na));
    identity_normalise(b, nb, sizeof(nb));
    return strcmp(na, nb) == 0;
}

/* Six hex pairs separated by ':' or '-'.
 *
 * ⛔ A DS5dongle's own serial is 17 hex digits with no separators, and it is
 * exactly what must never be taken for the pad's MAC: it follows the dongle
 * across a controller swap. */
static inline int identity_mac_shaped(const char *s)
{
    if (!s || strlen(s) != 17) return 0;
    for (int i = 0; i < 17; ++i) {
        const char c = s[i];
        if (i % 3 == 2) {
            if (c != ':' && c != '-') return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return identity_usable(s);
}

/* Rule 2's serial: uniq when it is one, otherwise the USB serial, otherwise
 * none. */
static inline void identity_pick_serial(const char *uniq, const char *usb_serial,
                                        char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    const char *pick = identity_usable(uniq) ? uniq
                     : identity_usable(usb_serial) ? usb_serial
                     : "";
    snprintf(out, out_len, "%s", pick);
}

/* What a HID interface says it is, from its top-level usage, as a word a person
 * can read: "controller", "keyboard", "mouse", or "" for anything else -- a
 * maker's own control interface, say.
 *
 * ⓘ Media keys (the consumer page) and system keys (sleep, power) count as a
 * keyboard: they are a keyboard's extra keys, and that is where someone looking
 * at the row would expect them. */
static inline const char *identity_type_for_usage(unsigned page, unsigned usage)
{
    if (page == 0x01 && (usage == 0x04 || usage == 0x05 || usage == 0x08)) return "controller";
    if (page == 0x01 && (usage == 0x06 || usage == 0x07 || usage == 0x80)) return "keyboard";
    if (page == 0x0C) return "keyboard";
    if (page == 0x01 && (usage == 0x01 || usage == 0x02)) return "mouse";
    return "";
}

/* The last element of a sysfs link: "../../../bus/usb/drivers/xpad" is "xpad". */
static inline void identity_link_leaf(const char *link, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    const char *slash = link ? strrchr(link, '/') : NULL;
    snprintf(out, out_len, "%s", slash ? slash + 1 : (link ? link : ""));
}

#endif /* DEVICE_IDENTITY_INL */
