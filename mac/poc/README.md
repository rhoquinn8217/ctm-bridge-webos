# A DualSense on a Mac, bridged to DS5-USBIP

A proof of concept. `ds5-mac-bridge` reads a DualSense on the Mac's USB cable
and lends it to DS5-USBIP on the gaming PC, the way the TV does, so Windows on
the PC sees a DualSense of its own. It speaks the TV's protocol through this
repository's transport (`src/shared/ctm_transport.c`). Nothing on the Mac
speaks USB/IP: DS5-USBIP does that, on the PC.

Not in it yet: Bluetooth, the pad's speaker, headphone jack and microphone,
reconnecting after a dropped link, and play over the internet (DS5-USBIP asks a
caller from the internet for a signature, and this program has none). It is
for the home network.

## Build

On a Mac with Apple's command line tools (`xcode-select --install`), from the
root of this repository:

    sh mac/poc/build.sh

## Run

1. Start DS5-USBIP on the PC.
2. Connect the DualSense to the Mac with a USB cable.
3. Run `mac/poc/ds5-mac-bridge <the PC's address>`.

Ctrl+C releases the pad. The program holds the pad for itself alone, so
macOS and games on the Mac leave it alone while it is bridged. macOS may ask
for Input Monitoring for the terminal the first time (System Settings, Privacy
& Security, Input Monitoring). If it cannot hold the pad, it says why and
opens it shared instead.

| Option | What it does |
|---|---|
| `--agent-port N` | DS5-USBIP's agent port, if it is not 48054 |
| `--no-seize` | Opens the pad shared from the start |
| `--strip-id` | Hands each output report to macOS without its report id byte. Try it if the PC's rumble and light never reach the pad |
