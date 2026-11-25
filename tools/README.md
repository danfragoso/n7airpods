# tools/

Helpers for working on the app. None are needed to install it.

| Tool | What it does | Needs |
|---|---|---|
| `run_host_tests.sh` | unit tests that run on the desktop: AES-128 vectors, the BLE advertisement parser (captured AirPods Pro 2 advertisements in `fixtures/`) | a C compiler, a patched NanoApps checkout (`NANOAPPS=...`, default `./.nanoapps`) |
| `ui_preview/` | renders the real page code on the desktop with mock AirPods states (13 scenarios: every model, the empty and connecting states), for design review without a device | `build_host_lvgl.sh` once, then `render.sh`; `make_gallery.py` composes the README screenshots |
| `btlog.py` | reads the Bluetooth flight recorder from the running iPod over USB and prints it as a log (`-g text` to filter, `--save`/`--file` to keep a capture, `--selftest` to check the decoder without a device) | NanoApps' `start` (`NANOAPPS=...`), and the decrypted firmware image at `build/osos.bin` to resolve message text, see `research/README.md` |
| `make_icons.py` | draws the four noise-control icons (head, shoulders and a halo) at 16x and writes `app/aap_ctl/aap_icons.c` | Pillow |
| `make_fonts.sh` | regenerates the three Inter fonts (`aap_font_*.c`) | node/npm, network |
| `make_art.py`, `make_icon.py` | build the AirPods pictures and the home-screen icon art from a folder of AirPods renders | Pillow, your own renders, see [../docs/ASSETS.md](../docs/ASSETS.md) |
| `host_shim/` | a tiny `hb_sdk.h` and the test key so `integration/aap_le.c` compiles on the desktop | |

## UI preview

```sh
NANOAPPS=/path/to/NanoApps tools/ui_preview/build_host_lvgl.sh    # compiles NanoApps' pinned LVGL for the host, once
NANOAPPS=/path/to/NanoApps tools/ui_preview/render.sh             # build/hostlv/out/s<N>.png (2x), c<N>.png
python3 tools/ui_preview/make_gallery.py                          # docs/screenshots/*.png
```

`NANOAPPS` can be the `.nanoapps` checkout that `./install.sh --build-only` makes. The page is the same code that
runs on the nano. `preview.c` also has test modes switched on by environment variables (`SCROLLTEST`, `ANIMTEST`, `FLINGTEST`,
`EDGETEST`) that were used for the performance work; each is described by a comment where it is handled.
