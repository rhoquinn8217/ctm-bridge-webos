# Plan: bridge the LG Magic Remote to the Windows host

Goal: plug the TV's Magic Remote through the CTM bridge so the PC sees it as an input
device (pointer + keys), same usbip path as the controllers. Written 2026-07-24; nothing
below is implemented yet.

## Phase 0 — probe (facts before code; nothing here is assumed)

All on-TV via `ares-novacom -d tv --run`, remote in hand, pressing buttons between reads:

1. **Does the remote expose hidraw?** `ls -l /dev/hidraw*` + for each:
   `cat /sys/class/hidraw/hidrawN/device/uevent` (HID_NAME, HID_ID → VID/PID, bus).
   Expectation to verify: the Magic Remote is BT; it may appear as a BT HID device with
   hidraw nodes, or only through LG's RCU stack with evdev nodes and no hidraw.
2. **Map its evdev nodes:** `cat /proc/bus/input/devices` — name, handlers, capabilities
   (REL/ABS for the pointer, KEY bitmap, and whether the wheel/mic are separate nodes).
3. **Capture the report descriptor** (if hidraw exists):
   `cat /sys/class/hidraw/hidrawN/device/report_descriptor | hexdump -C`.
   Decision input: standard HID usage pages (mouse/keyboard/consumer) vs LG vendor pages.
4. **Sample live reports:** `cat /dev/hidrawN | hexdump -C` while moving/pressing.
   Note report IDs, lengths, pointer encoding (relative vs absolute).
5. **Grab semantics:** small test — EVIOCGRAB the remote's evdev node for 5 s: does the
   TV cursor freeze (grab works, TV loses remote) and does releasing restore it cleanly?
6. **Jail check from the app context:** the standalone app already opens hidraw for
   controllers; confirm the remote's nodes carry the same group (root:jailer 0660) or not.

Phase 0 output: a short FACTS section appended to this file. Go/no-go per path:
- hidraw + standard HID pages → cheapest path (generic relay, Windows drivers native).
- hidraw + vendor pages → relay + agent-side descriptor profile (translate to mouse/kbd).
- no hidraw at all → evdev-source path (new: synthesize HID reports from evdev events).

## Phase 1 — classification + opt-in plug

- New kind `"magic"` in `bridge_kind_for_item()` matched on the probed VID (LG) + name.
- **Never auto-plugged.** The remote is the TV's own controller; losing it mid-session
  locks the user out of the TV. The auto-plug pass must skip `"magic"` explicitly (today
  it already skips non-recognized kinds, keep that invariant when the kind is added).
- UI: normal Plug in/out row; label makes the stakes clear ("bridges the TV remote to
  the PC — TV loses it while plugged").

## Decision 2026-07-24 (rev 2) — mouse semantics, SERVICE-ONLY transport

Reference: moonlight's non-CTM path treats the remote as an ABSOLUTE mouse
(webOS SDL emits the smoothed pointer; session_mouse.c sends
`LiSendMousePositionEvent(x, y, w, h)`; OK = left click, wheel = scroll).
That confirms the semantics — but the SDL/moonlight-protocol route is
**dropped from scope**. Everything goes through OUR service instead:

- One invariant: in CTM mode nothing rides the moonlight protocol; ALL host
  input is a real HID device via usbip. No view_only carve-outs (that
  asymmetry is the bug class we just finished fixing).
- Real device on Windows (vhci), not Sunshine SendInput injection — same
  reason the pads use the bridge; also works outside a streaming session,
  and scales to whatever device we add next.
- Build: new core synthesizer kind (not hidraw relay — the remote's
  LG-vendor descriptor means nothing to Windows): fixed ABSOLUTE-pointer
  descriptor (digitizer-style X/Y 0..32767 + left button + wheel), agent
  gets it as a static profile (puck composite proved this machinery).
- Input source: the apps feed webOS-smoothed pointer coordinates from their
  OWN SDL/LVGL event loop into the core via a small feed API
  (`ctm_bridge_feed_pointer(x, y, w, h, buttons, wheel)` — works identically
  in the standalone and both forks while the app is foreground; kills the
  raw-gyro risk for the primary use case. Phase 0 evdev probes stay relevant
  ONLY for optional background capture later.

## Phase 2 — transport

- Preferred: reuse the generic hidraw relay (`controller_generic.c`) — forward reports
  as-is over the existing bridge protocol; agent builds a dynamic profile from the
  forwarded descriptor (the `auto` profile path already exists on the agent).
  **WHITELIST CAVEAT (learned 2026-07-24): the agent's BRIDGE_START handler whitelists
  kinds {ds4,ds5,hid,puck,xbox} and REJECTS anything else — only kind `hid` reaches the
  auto path. New kinds ("kbd", "mouse", …) require an agent whitelist addition; until the
  agent rework ships, everything synthetic/generic must ride kind `hid`.**
- If reports are vendor-specific: keep the relay as-is on the TV side; do the translation
  on the agent (descriptor profile mapping vendor usages → mouse/consumer), so the TV
  code stays dumb and portable.
- Mic/voice endpoints: excluded outright in v1 (filter by report ID).

## Phase 3 — safety rails (the remote must never be lost)

- **Back long-press escape (BINDING)**: holding Back ~1.2 s while the pointer
  is bridged unplugs it (temporarily) and restores TV control — same reflex as
  moonlight/aurora's Back. Back is not part of the pointer descriptor, so it
  never reaches the PC; the long-press detector is app-local (key down/up
  timing in the app's input path). Short-press behavior while bridged: stays
  local (no app exit). Re-plug via the panel/row.

- Grab only while plugged; release on: unplug, app exit, session teardown (all already
  release grabs), plus a dead-man rule — if the bridge link to the agent drops, release
  the grab immediately (don't wait for process exit).
- A no-grab mode as a setting (forward without EVIOCGRAB; TV also reacts — acceptable
  inside a fullscreen stream, and strictly safer). Default decided after Phase 0 item 5.

## Portability rule (applies to this and all future bridge features)

Two consumers build the bridge core today: the standalone app (this repo) and the
moonlight/aurora embeds (via `ctmbridge/CMakeLists.txt` compiling core files from
`CTM_BRIDGE_DIR`). To keep every new feature drop-in portable:

1. **New logic goes in new, UI-free core files** — e.g. `src/shared/ctm_autoplug.c`
   (move the auto-plug policy out of `ui_app.c` into a reusable helper the UI calls),
   `src/controllers/controller_magic.c` (or the classification bits in the existing
   shared tables). No LVGL, no SDL, no app globals beyond ctm_state.
2. **Consumers stay one-line adopters:** standalone wires the call in its UI; the forks
   add the file to `ctmbridge/CMakeLists.txt` and call it from the glue. Nothing else.
3. **One core, eventually:** the forks currently compile the OLD core copy at
   `D:\Work\CMU\ctm-bridge-test-webos`; this repo has diverged ahead of it. Target state:
   point the forks' `CTM_BRIDGE_DIR` at THIS repo and retire the CMU copy — needs one
   compile/verify pass because the glue lib compiles a fixed file list. Until then, any
   core change lands here first and is cherry-copied to the CMU tree when the forks
   need it.

## Also queued: generic keyboards / mice on the TV (USB or 2.4G dongle)

User direction 2026-07-24; details TBD ("more tweaks" coming). Unlike the Magic
Remote these are REAL HID devices with standard descriptors, so the transport is
the EXISTING hidraw relay (generic ops -> agent auto dynamic profile + identity
map) — likely already functional via manual plug today. Actual work:

1. **Classify** — new kinds "kbd" / "mouse" from the HID report descriptor's
   top-level usage (keyboard 0x06 / mouse 0x02) instead of lumping into "hid";
   gives proper labels, per-kind policy, and auto-plug eligibility decisions.
2. **Grab** — for these, EVIOCGRAB must be ON while plugged (generic ops today
   don't grab): otherwise the TV reacts to every keystroke in parallel with the
   PC (double input). Grab-on-plug/release-on-unplug + the usual safety rails;
   an escape gesture TBD (a keyboard has no Back long-press — maybe a hotkey
   combo, or panel-only unplug).
3. **Dongles** — a 2.4G receiver is one USB device with several HID interfaces
   (kbd + mouse + consumer): the puck COMPOSITE machinery (forwarded
   enumeration, per-interface bridging) is the template; may work as-is.
4. Auto-plug: keyboards/mice stay OPT-IN like the remote (a TV keyboard may be
   someone's webOS input), decision per user tweaks.

## Also queued (fork-side)

- **"Use CTM Bridge" enabled by default in moonlight/aurora**: flip the setting's
  default in `app_settings.c` (both forks) so fresh installs get the bridge path
  (view-only input gating + bridge start with the stream) without visiting
  settings. Must not override an explicit user choice already saved in conf, and
  the ctm-bridge glue's ensure-core keeps panel plugs working when it's off.

## Order of work (rev 2 — core-first, universal by construction)

1. Refactor auto-plug into `ctm_autoplug.c` (portability rule, no behavior change).
2. Core synthesizer: `src/shared/ctm_hostmouse.c` — fixed absolute-pointer HID
   descriptor + report packing + session lifecycle (BRIDGE_START kind "hid" —
   see the whitelist caveat in Phase 2), plus the feed API
   `ctm_bridge_feed_pointer(x, y, w, h, buttons, wheel)`. UI-free.
3. Agent: NO change needed (kind "hid" → auto dynamic profile + identity map
   builds the device from our HELLO descriptor).
4. Standalone = first consumer: wire its LVGL/SDL pointer events into the feed
   API + a Plug row (never auto-plugged; grab/safety rails per Phase 3).
5. Forks adopt: add the core file to ctmbridge/CMakeLists.txt, call the feed
   hook from the SDL event path, panel row appears like any other device.
6. Phase 0 evdev probes only if/when background capture (app not foreground)
   is wanted.
