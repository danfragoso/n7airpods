# integration/

What gets added to [NanoApps](https://github.com/nfzerox/NanoApps) to give it the AirPods transport. You normally
do not touch this by hand: `../install.sh` does it. This folder is the source of truth.

| File | Goes to (in NanoApps) | What it is |
|---|---|---|
| `nanoapps-aap.patch` | applied with `git apply` | five small changes to existing NanoApps files, see below |
| `hb_aap_shared.h` | `sdk/` | the shared state block between resident and app (`hb_aap_state_t`), with a magic, an ABI number and a size |
| `aap_transport.c` | `system/silver_resident/` | owns the AAP L2CAP channel; decodes notifications; takes the app's intents |
| `aap_le.c` | `system/silver_resident/` | BLE advertisement parser and scan control |
| `aes128.c`, `aes128.h` | `system/silver_resident/` | AES-128 decryption for the advertisements' exact levels |
| `btlog.c` | `system/silver_resident/` | Bluetooth flight recorder (observation only) |
| `did_spoof.c` | `system/silver_resident/` | Device-ID record experiment (off by default) |

The patch is made against NanoApps commit `0c2bd815217b964f364edd527ae07c72b4217b94` (the one `install.sh`
pins). It changes `sdk/hb_shared.h`, `sdk/hb_lv_surface.c`, `system/silver_resident/Makefile`,
`system/silver_resident/compat_fixes.h` and `system/silver_resident/silver_resident.c`. What each change is for is
in [../docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md#changes-to-nanoapps).

## By hand

```sh
git clone https://github.com/nfzerox/NanoApps && cd NanoApps
git checkout 0c2bd815217b964f364edd527ae07c72b4217b94
git apply /path/to/n7airpods/integration/nanoapps-aap.patch
cp /path/to/n7airpods/integration/{aap_le.c,aap_transport.c,aes128.c,aes128.h,btlog.c,did_spoof.c} system/silver_resident/
cp /path/to/n7airpods/integration/hb_aap_shared.h sdk/
cp -R /path/to/n7airpods/app/aap_ctl apps/aap_ctl
./start build aap_ctl && make -C system/silver_resident
./start install --resident-only        # the resident first: the iPod restarts
./start install aap_ctl
```

The resident and the app are a pair. If the app finds a resident without the AirPods state it shows
"Resident update needed" instead of guessing.

## Other NanoApps versions

The patch is exact for the pinned commit. For a newer NanoApps, `git apply --3way` usually resolves it; the
resident hooks only depend on firmware addresses that do not change (firmware 1.1.2 is the only one NanoApps
supports). Please open an issue if you port it.
