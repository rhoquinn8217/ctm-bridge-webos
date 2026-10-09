# DualSense Bridge Core for webOS

![platform](https://img.shields.io/badge/platform-LG%20webOS-A50034?logo=lg&logoColor=white)
![language](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)
![fork of CTM-Bridge/ctm-bridge-webos](https://img.shields.io/badge/fork%20of-CTM--Bridge%2Fctm--bridge--webos-lightgrey)

This is a fork of [ciprianmisaila's
ctm-bridge-webos](https://github.com/CTM-Bridge/ctm-bridge-webos). It is the
bridge core [rhoquinn8217/aurora-tv](https://github.com/rhoquinn8217/aurora-tv)
is built with, and it connects DualSense controllers on a webOS TV to
[DS5-USBIP](https://github.com/rhoquinn8217/DS5-USBIP) running on a Windows host,
over the network or the internet.
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

rhoquinn8217/aurora-tv holds this repository as a **submodule**, at
`third_party/ctm-bridge-webos`, and compiles its `ctmbridge` library straight
from these sources rather than linking a prebuilt one. Each aurora-tv commit
records the exact commit of the core it was built with.

See [rhoquinn8217/aurora-tv's build
instructions](https://github.com/rhoquinn8217/aurora-tv#build) for the Docker
build. A clone made with `--recursive` has the core; in any other checkout,
`git submodule update --init --recursive` fetches it.

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
