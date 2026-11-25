# research/

Historical material from the reverse-engineering phase. **None of it is needed to install or use the app**, and
**some of it crashed or rebooted the iPod**, so do not run things from here without reading the notes first.

| Folder | What it is |
|---|---|
| `harness/` | the first probes (`aap_probe.c`, `aap_open.c`) that opened the L2CAP channel from SCSI-exec'd or app code. `aap_open.c` is the raw firmware-call harness that killed the device; the working design moved this work into the resident (see `docs/ARCHITECTURE.md`). `test_packets.c` is a host test of the early frame builders. |
| `aap_open/` | the first "safety baseline" NanoApps app (memory-only diagnostic, no Bluetooth). Superseded by `app/aap_ctl`. |
| `tools/` | the firmware-analysis scripts: decrypt the firmware image on a Mac with wInd3x, survey its symbols, drive Ghidra headless, read and scan the device's RAM over USB (`peek.sh`, `rd.py`, `scan_dram.py`), live L2CAP table capture. |

## Working with the firmware

Nothing from Apple's firmware is in this repository, and the scripts assume you obtain it yourself:

1. Download the iPod nano 7th generation 1.1.2 IPSW from Apple's update servers.
2. `research/tools/decrypt.sh your.ipsw` extracts and decrypts the `osos` image using [wInd3x](https://github.com/freemyipod/wInd3x)
   (the nano has to be in DFU mode; the decryption runs on the nano's own AES engine and writes nothing to it).
   Put the `wInd3x` binary in `tooling/bin/`.
3. `research/tools/survey.sh` and `research/tools/ghidra-import.sh` work on the result in `build/`
   (Ghidra goes in `tooling/ghidra_*`).

`tools/btlog.py` (the Bluetooth flight-recorder reader) also resolves log format strings against that decrypted
image, at `build/osos.bin`.

Scripts that talk to the device use NanoApps' `ipod_sg_raw` helper; set `NANOAPPS` to your NanoApps checkout
(default `~/NanoApps`).
