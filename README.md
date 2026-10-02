# ctm-bridge-webos - DualSense Bridge Core for webOS

![platform](https://img.shields.io/badge/platform-LG%20webOS-A50034?logo=lg&logoColor=white)
![language](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)
![fork of CTM-Bridge/ctm-bridge-webos](https://img.shields.io/badge/fork%20of-CTM--Bridge%2Fctm--bridge--webos-lightgrey)

This is a fork of [ciprianmisaila's
ctm-bridge-webos](https://github.com/CTM-Bridge/ctm-bridge-webos). It is the
bridge core [rhoquinn8217/aurora-tv](https://github.com/rhoquinn8217/aurora-tv)
is built with, and it connects DualSense controllers on a webOS TV to
[DS5-USBIP](https://github.com/rhoquinn8217/CTM-USBIP) running on a Windows host.
The core is a static library and has been expanded to include DualSense specific
features.

This fork of ctm-bridge-webos expects DS5-USBIP running on your host PC to
bridge controllers.

---

## What lives here

**Existing, from ciprianmisaila's core**

- DualSense audio over Bluetooth
- DualSense output report translation

**Added by this fork**

- DualSense audio over the TV's USB port
- Microphone over the TV's USB port
- DualSense Edge support
- DualSense bridge gestures and confirmation signals

---

## Build

The core is **not built on its own**: whatever embeds it compiles these sources,
so building the core means building the app that carries it.

See [rhoquinn8217/aurora-tv's build
instructions](https://github.com/rhoquinn8217/aurora-tv#build), which cover the
Docker build and the sibling checkout this repository has to sit in. Clone it as a
**sibling directory**, which is where that CMake looks, or point
`-DCTM_BRIDGE_DIR=<path>` at wherever you put it.

The core carries its own test suite, run by `tests/run-tests.sh`.

---

## Clean-room

**Load-bearing, not a formality.** All controller protocol here is derived from
this project's own observation (sysfs reads and on-wire captures), **not** from
third-party or kernel driver sources. ciprianmisaila's ctm-bridge-webos holds the
same line, and this fork continues it.

---

## Acknowledgements

- **[ciprianmisaila](https://github.com/ciprianmisaila)**: ctm-bridge-webos and
  [CTM-USBIP](https://github.com/CTM-Bridge/CTM-USBIP). The bridge itself, the
  map-driven translation pipeline, the TCP protocol between the television and the
  host, and the DualSense audio work over Bluetooth are all ciprianmisaila's.
- **[LVGL](https://github.com/lvgl/lvgl)** (MIT): the UI toolkit, bundled here.
- **[ENet](https://github.com/lsalzman/enet)** (MIT): the optional UDP transport,
  bundled here.

---

## License

[GNU General Public License v3.0](LICENSE) (GPL-3.0-or-later).

Copyright (C) 2026 Ciprian Teodor Misaila. Fork additions copyright (C) 2026
rhoquinn8217, under the same license.

Not a license term, an ask from ciprianmisaila's CTM Bridge that this fork
honours: if you integrate CTM Bridge into your own app or fork, overlay the CTM
Bridge badge on your app's icon, the way the
[aurora-tv](https://github.com/CTM-Bridge/aurora-tv) and
[moonlight-tv](https://github.com/CTM-Bridge/moonlight-tv) forks do.

<a
href="https://github.com/CTM-Bridge/ctm-bridge-webos/blob/main/icon_extra_large.png"><img
src="https://raw.githubusercontent.com/CTM-Bridge/ctm-bridge-webos/main/icon_extra_large.png"
width="96" alt="CTM Bridge badge"></a>
