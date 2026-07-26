#ifndef CTM_HOSTMOUSE_H
#define CTM_HOSTMOUSE_H

/* TV-pointer -> host mouse synthesizer.
 *
 * Not a hidraw relay: the Magic Remote's LG-vendor HID descriptor means
 * nothing to Windows. Instead the app feeds already-smoothed pointer state
 * (webOS does the gyro math) and this module speaks the normal bridge
 * protocol with its OWN absolute-pointer descriptor (digitizer-style X/Y
 * 0..32767 + 3 buttons + wheel). The agent needs no changes, but the
 * BRIDGE_START kind MUST be "hid": the agent whitelists kinds and only "hid"
 * takes the "auto" dynamic-profile path with the identity map (unknown kinds
 * like "mouse" are rejected), so the host builds the device 1:1 from our
 * descriptor.
 *
 * Threading: one session thread (connect/HELLO/feature replies); feed sends
 * directly from the caller's thread through the thread-safe transport. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the session thread against the agent's data port (the caller already
 * sent BRIDGE_START for this port). Returns 0 on thread start, -1 if already
 * active or thread creation failed. */
int ctm_hostmouse_plug(const char *host, int port);

/* Stop + join the session thread and drop the link. Safe when idle. */
void ctm_hostmouse_unplug(void);

/* True from plug to unplug (regardless of link state). */
bool ctm_hostmouse_active(void);

/* True while the data link is up (handshake done). */
bool ctm_hostmouse_connected(void);

/* Feed pointer state from the app's event loop. x/y in surface coordinates of
 * a w x h surface (scaled to 0..32767 here); buttons bit0=left bit1=right
 * bit2=middle; wheel_delta in detents (+ away from user). Coalesces motion,
 * accumulates wheel. No-op when not connected. */
void ctm_hostmouse_feed(int x, int y, int w, int h, unsigned buttons, int wheel_delta);

/* Feed a keyboard key (HID Keyboard/Keypad usage, e.g. 0x50 LeftArrow) up or
 * down. The device's second report is a standard 6-key-rollover keyboard, so
 * the remote's D-pad/OK arrive on the host as real key strokes. No-op when
 * not connected; pressed set clears on reconnect (no stuck keys). */
void ctm_hostmouse_feed_key(uint8_t hid_usage, bool down);

/* Optional log sink (thread-safe on the caller's side); NULL = stderr only. */
void ctm_hostmouse_set_logger(void (*sink)(const char *line));

#ifdef __cplusplus
}
#endif

#endif /* CTM_HOSTMOUSE_H */
