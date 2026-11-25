# Feasibility: can the nano open L2CAP PSM 0x1001?

## What we know (with sources)

| Fact | Source |
|---|---|
| Nano 7G boots a Pixo/RTXC RTOS, `osos` load base `0x08000000`, encrypted IMG1 fmt 3 | blueth4w `docs/HOW_IT_WORKS.md` |
| BT host stack is **Open Interface BLUEmagic** + Apple C++ layer (`BT::A2dpStreamConfig`, `OI_AVDTP_*`) | same |
| `osos` has **rich debug strings and C++ symbols** | same |
| `wInd3x decrypt` works on the nano 7G, on macOS, RAM-only, via S5Late | wInd3x README (Nano 7G: decrypt = YES) |
| AAP = classic L2CAP PSM 0x1001, secured | LibrePods `BluetoothConnectionManager.kt` (`type = 3`, `psm = 4097`) |
| Code pages are writable in RAM and patches survive until reboot | NanoBtFix / blueth4w |

## What we do *not* know

Whether BLUEmagic exposes **connect-to-arbitrary-PSM** to callers. Apple only ever wired up
A2DP/AVDTP, AVRCP, and HFP/SCO. PSM 0x1001 was never used on this device.

## The good news, and the actual attack

The firmware *does* open L2CAP channels by PSM — for its own profiles:

| Profile | L2CAP PSM |
|---|---|
| SDP | 0x0001 |
| RFCOMM (HFP) | 0x0003 |
| AVRCP (control) | 0x0017 |
| AVDTP (A2DP signalling) | 0x0019 |

So there is a generic `l2cap_connect(psm, …)` somewhere under the AVDTP/AVRCP open path. Two
strategies, in order of cheapness:

**Strategy A — find the constant.** Locate the code that opens AVDTP's channel (PSM 0x0019,
immediate `0x19` appears as an argument), and clone that call path with `0x1001`. This is the
same technique the scene already proved out: find the function, understand the args struct,
patch bytes. No new API needed.

**Strategy B — find the API.** If a symbol like `OI_L2CAP_*` / `L2CAP_Connect` exists and is
callable, use it directly: cleaner, but depends on its contract (callbacks, MTU, event pump).

**Strategy C — dead end.** L2CAP channel setup turns out to be entangled with profile registration
in a way that can't be driven from outside a profile task. Then this project stops, and the fallback
is: ANC via the AirPods' own stem squeeze (already works today, no firmware needed).

## Decision gates

1. **G0 — symbols.** Does `build/symbols.txt` contain an L2CAP connect entry point, or does the
   AVDTP open path lead to a generic PSM argument? → go / no-go.
2. **G1 — RAM probe.** From the resident action-hook context, open PSM 0x1001, send the AAP
   handshake, and get any reply. Trace ring shows the bytes. → go / no-go.
3. **G2 — stability.** Does running the channel alongside live A2DP audio leave the link up for
   ≥10 min, through a track change and a reconnect? → ship / don't ship.

## Risks

- **Crash exposure.** Calling stack internals from the wrong task context can take down the BT
  link or reboot the nano. Mitigation: RAM-only, every step reboot-recoverable; run from the same
  context the resident already uses for its patches.
- **Threading / event pump.** L2CAP callbacks likely fire from the BT task. Our app's UI runs
  elsewhere, so state must be handed over safely (single-writer flag + trace ring, not LVGL calls
  from the BT task).
- **Memory.** ~4 MB per app, and the resident's policy avoids reusing executable memory. A new
  L2CAP channel costs stack buffers (MTU-sized), which the firmware may try to allocate from a
  pool that isn't sized for it.
- **Callback tables.** Registering a receive handler may mean patching a dispatch array rather
  than passing a function pointer — more surface, more risk.
- **AirPods side.** Pro 2 USB-C may require the `4D 00 FF` unlock packet for some features, and
  may refuse a channel from a device that doesn't look like Apple anything. Noise control is
  reported to work on Android/Linux, so this is unlikely to block the core feature.

## Non-goals

Firmware updates, Find My, head tracking, LE Audio. See DUAL-MODE.md.
