/* Tests for which way a DualSense's signal goes.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). "No speaker device open" was
 * read as "Bluetooth", so every cabled bridge was sent a Bluetooth wake report,
 * and a cabled pad whose sound card could not be opened was sent its whole
 * signal as Bluetooth reports. These pin src/controllers/signal_route.inl: the
 * bus decides Bluetooth, and the speaker decides only which wired way.
 *
 * ➡️ Build and run:  cc tests/test_signal_route.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>

#include "../src/controllers/signal_route.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

int main(void)
{
    printf("\nBluetooth: the reports carry the signal\n");
    ok(signal_route_for("BT", 0) == SIGNAL_ROUTE_BLUETOOTH, "no speaker device, as always on Bluetooth");
    ok(signal_route_for("BT", 1) == SIGNAL_ROUTE_BLUETOOTH, "the bus decides even if a card were open");

    printf("\na cable with its sound card: tone, pulse and light\n");
    ok(signal_route_for("USB", 1) == SIGNAL_ROUTE_SPEAKER, "the speaker route");

    printf("\na cable WITHOUT its sound card: the light alone, never Bluetooth\n");
    ok(signal_route_for("USB", 0) == SIGNAL_ROUTE_LIGHT_ONLY, "light only (was the Bluetooth signal)");
    ok(signal_route_for("USB", 0) != SIGNAL_ROUTE_BLUETOOTH,
       "so the plug-time wake, which asks for Bluetooth, skips it");

    printf("\nno bus known is not Bluetooth\n");
    ok(!signal_bus_is_bluetooth(""), "empty");
    ok(!signal_bus_is_bluetooth(NULL), "NULL");
    ok(!signal_bus_is_bluetooth("BTX"), "a near miss");
    ok(signal_route_for("", 0) == SIGNAL_ROUTE_LIGHT_ONLY, "and no card: the light alone");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
