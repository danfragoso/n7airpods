# Notices

## Not affiliated with Apple

n7airpods is an independent hobby project. It is not made, endorsed, sponsored or approved by Apple Inc.
Apple, AirPods, AirPods Pro, AirPods Max, iPod, iPod nano, iPhone, iOS, macOS and SF Symbols are trademarks of
Apple Inc. They are used here only to say which products this software works with.

## Apple artwork

`app/aap_ctl/aap_art.c` / `aap_art.h` contain small renders of AirPods and their cases (about 70 px wide,
converted to RGB565), and `app/aap_ctl/icon_art.png` is the render the home-screen icon is built from (one AirPods
Pro bud, 467 x 545 px). All of it is derived from Apple's product imagery, which is Apple's copyright. It is in this
repository only so that the app shows the AirPods it is controlling, **it is not covered by the MIT license**, and
it is not offered for reuse.

- To build without them, replace the files with your own artwork (see [docs/ASSETS.md](docs/ASSETS.md)).
- If you are a rights holder and want them removed, open an issue and they will be.

## Components used

| Component | License | How it is used |
|---|---|---|
| [NanoApps](https://github.com/nfzerox/NanoApps) (nfzerox, Merquice, Raphmatt, Nikita Shimkus) | MIT | The platform this runs on. Not copied into this repository. `integration/nanoapps-aap.patch` is a patch against it and keeps its license, see its `LICENSE`. |
| [LVGL](https://github.com/lvgl/lvgl) (c) 2025 LVGL Kft | MIT | The UI toolkit NanoApps links. `app/aap_ctl/aap_snap.c` is adapted from LVGL's `lv_snapshot.c`. |
| [Inter](https://github.com/rsms/inter) (c) 2016 The Inter Project Authors | SIL Open Font License 1.1 | `app/aap_ctl/aap_font_*.c` are subsets of Inter converted to LVGL bitmap fonts. The license is in [docs/fonts/Inter-OFL.txt](docs/fonts/Inter-OFL.txt). |
| [LibrePods](https://github.com/kavishdevar/librepods) (kavishdevar) | GPL-3.0 | **No code is used.** Its documentation of the AirPods protocol and its model ids were read to learn the protocol; see [docs/PROTOCOL.md](docs/PROTOCOL.md). |

The reverse-engineering notes in `docs/research/` describe Apple's firmware (function addresses, structures). The
repository contains no Apple firmware and none of its code.
