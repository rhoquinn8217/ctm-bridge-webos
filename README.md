# CTM Bridge Test

> **A fork of [CTM Bridge](https://github.com/CTM-Bridge/ctm-bridge-webos) by
> Ciprian Teodor Misaila.**
>
> What CTM Bridge is, how the television and the Windows host divide the work,
> the TCP protocol between them and the design philosophy behind it are all his.
> They are documented in
> [the upstream README](https://github.com/CTM-Bridge/ctm-bridge-webos#readme)
> and this page does not repeat them. Read that first.

## What this fork is for

This fork exists to serve one thing: the
[rhoquinn8217/aurora-tv](https://github.com/rhoquinn8217/aurora-tv) beside it. That app
compiles its `ctmbridge` library straight from these sources rather than linking a
prebuilt one, so the two move together and a change here reaches the app on its
next build.

To build the app, clone this repo **as a sibling directory** — that is where its
CMake looks — or point `-DCTM_BRIDGE_DIR=<path>` at wherever you put it.

The standalone webOS app in this repo is `ctm_bridge_lvgl_ui`, installed as
**CTM Device Bridge**. It carries a DualSense or DualSense Edge from the
television to a Windows host.

## Build

The `ctmbridge` core is **not built on its own**: whatever embeds it compiles
these sources, so building the core means building the app that carries it.
➡️ **See the [rhoquinn8217/aurora-tv's build instructions](https://github.com/rhoquinn8217/aurora-tv#build)**,
which cover the Docker build and the sibling checkout this repo has to sit in.

### The standalone app

⚠️ **Last verified July 2026.** The app's own sources have moved on since —
`CMakeLists.txt` and the UI both changed in September — while these scripts have
not, so treat them as a starting point rather than a guarantee. The core is built
through rhoquinn8217/aurora-tv above, which is the path in daily use.

`ctm_bridge_lvgl_ui` is packaged with the webOS SDK scripts here:

```sh
scripts/build_ipk_macos.sh
```

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_ipk_windows.ps1
```

The Windows script installs missing `cmake`/`ninja` with Chocolatey unless
`-NoInstallPrereqs` is passed, and auto-detects a Beanviser-bundled
`ares-package.cmd` when Beanviser sits beside this project. Pass `-AresPackage`
and `-ToolchainFile` if the SDK/NDK is somewhere unusual, or
`-WebOsSdkInstaller` to let the script run an installer and re-detect.

Host tools: `cmake`, `cpack`, `ares-package`, SDL2, SDL2_ttf and the webOS
SDK/NDK toolchain, plus `rsync` and `pkg-config` on macOS and Linux and `ninja`
on Windows. Beanviser ships the Ares packaging CLI but not the compiler
toolchain.

## License

[GNU General Public License v3.0](LICENSE) (GPL-3.0-or-later).
Copyright (C) 2026 Ciprian Teodor Misaila.

Not a license term, just a friendly ask from upstream: if you integrate CTM
Bridge into your own app or fork, please overlay the CTM Bridge badge on your
app's icon — the way the
[aurora-tv](https://github.com/CTM-Bridge/aurora-tv) and
[moonlight-tv](https://github.com/CTM-Bridge/moonlight-tv) forks do:

<a href="https://github.com/CTM-Bridge/ctm-bridge-webos/blob/main/icon_extra_large.png"><img src="https://raw.githubusercontent.com/CTM-Bridge/ctm-bridge-webos/main/icon_extra_large.png" width="96" alt="CTM Bridge badge"></a>&nbsp;&nbsp;→&nbsp;&nbsp;<img src="https://raw.githubusercontent.com/CTM-Bridge/aurora-tv/main/deploy/webos/icon.png" width="96" alt="Aurora icon with the badge">&nbsp;<img src="https://raw.githubusercontent.com/CTM-Bridge/moonlight-tv/main/deploy/webos/icon.png" width="96" alt="Moonlight TV icon with the badge">
