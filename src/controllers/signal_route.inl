/* Which way a DualSense's signal goes: through its sound card, as Bluetooth
 * reports, or as the light alone.
 *
 * ⛔⛔ WHY (code review, 2026-10-05). "No speaker device open" was read as
 * "Bluetooth", and on a cable it is not:
 * - A cabled pad has no speaker device until the card match opens one, which
 *   runs after the plug-time speaker wake. So every cabled bridge was sent a
 *   398-byte Bluetooth wake report, retried up to five times.
 * - A cabled pad whose sound card could not be opened was sent its whole
 *   signal as Bluetooth reports, 120 to 240 of them, in place of the tone.
 *
 * ➡️ The bus decides Bluetooth. The speaker decides only which wired way: the
 * tone and the felt pulse both travel as audio and need the card, while the
 * light is a HID report and does not.
 *
 * ⓘ Its own file so a test can pin it. */

#include <string.h>

typedef enum {
    SIGNAL_ROUTE_SPEAKER = 0,   /* a cable with its sound card: tone, pulse and light */
    SIGNAL_ROUTE_BLUETOOTH,     /* Bluetooth: reports carry all of it */
    SIGNAL_ROUTE_LIGHT_ONLY     /* a cable without its sound card: the light alone */
} signal_route_t;

static inline int signal_bus_is_bluetooth(const char *bus)
{
    return bus && strcmp(bus, "BT") == 0;
}

static inline signal_route_t signal_route_for(const char *bus, int has_speaker)
{
    if (signal_bus_is_bluetooth(bus)) return SIGNAL_ROUTE_BLUETOOTH;
    return has_speaker ? SIGNAL_ROUTE_SPEAKER : SIGNAL_ROUTE_LIGHT_ONLY;
}
