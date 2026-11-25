# Strategy: stop poking, start recording (2026-11-24)

Goal unchanged: AirPods battery + noise-control modes on the nano 7G over AAP (L2CAP PSM 0x1001).
Blocker: the link drops 15–73 s after the AAP channel opens and the music stops. We do not know who
ends the link. Every earlier test (patch, reboot, connect, wait, disconnect) answered one question
per cycle. The change of approach: **build the observation layer once, then every test answers
many questions**, and decide about code changes from evidence.

## What the firmware already gives us

| Finding | Where |
|---|---|
| Apple's BT layer narrates everything through one logger method: `log(this, module, file, func, line, level, fmt, ...)`. 558 call sites in the decompiled corpus, including "Disconnection reported for device…", every HCI/L2CAP failure, pairing and profile connect/disconnect. | method RAM `0x082bff0c`, vtable `0x087eaa54` slot +8 (RAM data) |
| The method compares `level` with a per-module threshold and returns early, so by default most of the narration is thrown away and nothing reads the rest. | disassembly of `0x082bff0c` |
| The OI layer has its own debug printer. | `FUN_08018ffc` (corpus), RAM `0x08018bfc` |
| HCI driver table: `{flag, name, init, 8 callbacks…}`. The image only holds the placeholder "NULL Transport"; the real UART driver registers itself at run time. | RAM `0x0895c100` (pointer at `0x0895c134`) |
| `start peek` reads DRAM over USB in any mode (even the home screen), no reboot. | `start peek ADDR [SIZE]` |
| The earpods repo's symbol map (same firmware) is valid for our corpus. | see memory `nano7-untethered-earpods-repo` |

Not a lead (checked): the `HCITraces` preference gates `OI_SNIFF_Start`, not packet logging.
Correction: `FUN_08436138` (corpus; RAM `0x08435d38`) is the *profile-level* "device not connected to
profile" notification, not the HCI disconnect reason.

## Plan

**Tap 1 — logger flight recorder (built, compiles, not yet run on hardware).**
`integration/btlog.c`: swaps the one vtable data slot for a wrapper that records
(uptime, module, level, line, fmt, call site, 4 raw args, first `%s` argument) into a 128 KB ring
and then tail-jumps to the stock method. Observation only. Fails safe: installs only if the slot
holds the stock value, `/Apps/Data/btlog.off` disables it, errors always recorded, chatty call
sites are sampled so a per-packet message cannot flush the ring. Read it with
`python3 tools/btlog.py` (no reboot; `--selftest` runs without a device).

**Tap 2 — HCI transport (needs one read-only peek first).**
The Disconnection Complete reason code (0x13 remote user, 0x08 timeout, 0x16 local, 0x15 remote
power-off/low resources) is the single most informative number. The logger may not print it. To
tap the transport we need the live driver's read/write callbacks:
`start peek 0x0895c134 4`, then `start peek <that pointer> 0x40`. No install required.
Output: a btsnoop file we can open in Wireshark.

**Session 1 (baseline, stock behaviour + recorder):** music to the AirPods for 10 minutes, Mac and
iPhone Bluetooth off. Then the same with them on. If the stock stack also drops, AAP is not the
cause and the whole question changes.

**Session 2:** reinstall the AAP transport from `build/nanoapps-aap-backup/` with the recorder
running; one run gives the full timeline around the drop.

## Decision tree (what the evidence selects)

| Evidence | Meaning | Next |
|---|---|---|
| Drops on stock, no AAP | Not our code; AirPods/environment/stock stack | Fix environment (re-pairing, auto-switch off); AAP is innocent |
| Remote-terminated (0x13) with Mac/iPhone BT off | The AirPods end it; something about our AAP session they dislike | Study what a healthy host sends (keep-alive, feature/ack messages); compare to a Mac session |
| Remote-terminated only when Mac/iPhone BT on | User's hypothesis confirmed | Disable auto-switch / unpair the other devices from these buds; app can only warn |
| Timeout (0x08) | RF or stack stall | Look at what the stack was doing (sniff mode, queue); `OI_SNIFF` is a candidate |
| Local (0x16) / stack sends L2CAP disconnect | Our side ends it | Fix the specific cause: channel/idle handling in OI L2CAP |

## Replacing the stack: not now

A second host stack (BTstack, Zephyr) would have to own the HCI UART, which is single-owner, and
re-implement or coexist with everything Apple's BT layer does on the same controller: pairing
store, connection/power managers, the A2DP send thread and SBC path, AVRCP, LE (Nike+ heart-rate)
and the FM radio that shares the chip. It would only pay off if the evidence above pointed inside
OI's L2CAP, and even then a much smaller step exists: a **side-car L2CAP** that injects/consumes
only the PSM 0x1001 frames at the HCI transport boundary on the existing ACL handle and leaves
everything else to the stock stack. Decide after Session 1/2.

## Rollback

`/Apps/Data/btlog.off` (empty file) + reboot disables the recorder. Source of truth for the build:
`git -C <NanoApps checkout> checkout system/silver_resident` restores the stock resident.

## First hardware run (2026-11-24) — findings

Recorder installed with `./start install --resident-only` (backup of the previous resident:
scratchpad `prebtlog/resident.stock-before-btlog.hbapp`; catalog still 14 apps). Slot, method entry bytes
and the `H4BC` HCI driver name were verified live before installing. 244 records after the
first experiments; `hooked=1`; no crash.

1. **USB disk mode powers Bluetooth off.** Log sequence: `Entering disk mode.` →
   `Chipset::writeDefaultPowerState : 0 - OFF` → `Beginning asynchronous power off of module Bluetooth`
   → `StackManager shutting down` → `Stack has stopped`. `Leaving disk mode.` → BT powered on again.
2. **The first host SCSI access (`start peek`, `start trace`, the install tools) forces disk mode.**
   Controlled run: eject at uptime 187.554 s, 23 s idle with the cable attached → nothing logged;
   first `start peek` at +23.6 s → `Entering disk mode` at 211.151 s, BT off 1.1 s later. A second
   observation (boot capture) shows the same pattern. Cable attached alone does nothing.
3. Consequence: **every earlier AirPods test in which the Mac talked to the nano over USB while the
   AirPods were connected is confounded** (autopilot taps, trace/peek readouts, installs). Those
   results cannot be used to blame AAP, the stack, or the Mac/iPhone.
4. The stack logs the disconnect reason itself: `Received disconnection result for "%s" profile on
   device %s - result was %d - reason was %d`; also `Sending 'device disconnected' notification…`,
   `%s is disconnecting device %s` (local initiation), `Disconnected from device "%s"`. The logger tap
   therefore already captures what Tap 2 (HCI) was meant to; Tap 2 is deferred.
5. Paired devices known to the stack: AirPods Pro (`xx:xx:xx:xx:xx:xx`) and one record with an empty
   link key (`00:0A:45:19:42:..`). Core stack build: May 7 2016.

### Test protocol (replaces all earlier ones)

Never touch USB data while a Bluetooth test runs. Unplug the nano, run the test, plug in, read.
The ring survives (DRAM); the readout itself powers BT off, but only after the data was recorded.
Stack start/stop, disk mode, pairing and profile events carry timestamps, so the readout moment is
visible in the log and cannot be mistaken for a drop.

- A: AirPods connected, music 10 min, Mac and iPhone Bluetooth **off**.
- B: same, Mac and iPhone Bluetooth **on**.
- C (only if A or B is clean): reintroduce the AAP transport with the recorder running.

## Results (2026-11-24)

**Baseline A (stock stack + recorder, no AAP, Mac/iPhone BT off, unplugged):** AirPods linked at 519.7 s
uptime, audio from 542.5 s, no disconnect messages until the deliberate readout at 1026 s (≈ 8.4 min).

**Test C (AAP transport + AirPods app + recorder, Mac/iPhone BT off, unplugged):** user report: rock
solid, survives screen lock, app came up already connected without pressing Connect, buds' battery
updates almost instantly as buds are moved, noise-control modes (off / ANC / transparency) switch with
the chime. Recorder: stack link up 160.3 s → 537.4 s (≈ 6.3 min), AAP channel opened 172.7 s (before
audio started) and stayed up to the readout; **no disconnect message of any kind** in the window; link
survived pause/resume (`paused` at 485 s, `playing` 494 s, …). 12 battery packets, 23 ear-detection,
73 control (0x09) notifications, one 2582-byte packet at open. (The user's estimate was 10 min; the
recorder's clock says 6.3 min of link. A timed run would settle it; nothing suggests the clock is off.)

Conclusion so far: the old 15–73 s drops were **not** the AirPods, the stock stack, or the AAP channel
itself. Prime suspect is our own USB activity during those tests (see finding 1–3 above). The
Mac/iPhone hypothesis is still untested with them *on*.

### Open items
1. **Case battery never shown.** All 12 battery packets carry the case entry as component 0x08,
   status 4 ("disconnected"), level dropped by the parser (status 4 → -1). Raw 5-byte entries are now
   recorded as `AAPBR` marks (installed 21:26) to see whether a level byte is present. Needs a bud in an
   open case for ≥ 15 s while the other bud stays connected.
2. **Mac/iPhone ON test (user's hypothesis).** Same protocol as C with their Bluetooth on and idle.
   The log will give the disconnect reason code if the buds go elsewhere.
3. UI: no in-ear/charging indication ("-" for a bud in a closed case); ear-detection (op 0x0006) is
   decoded but unused; modes beyond off/ANC/transparency/adaptive untested.

## Test D (2026-11-24, Mac and iPhone Bluetooth ON, unplugged)

User: works well; played and paused on the Mac, went back to the nano, no disconnect. Recorder (window
142.6 s → 478.4 s): stack link 171.4 s → end, AAP open 176.7 s, **no disconnect or stream-loss message
of any kind**; the nano's playback states (pause/play) are the user's and the buds' in-ear detection.
So the Mac/iPhone hypothesis is not what ended the old sessions: with them on and idle, and with the
Mac taking the audio briefly, the nano keeps its A2DP + AAP links.

**Case battery, raw entries (AAPBR):**

| t (s) | LEFT | RIGHT | CASE |
|---|---|---|---|
| 179.5 | 62% disch. | 00 **disconnected** (right bud in case at start) | 00, status 4 |
| 281.7 | 62% disch. | 92% disch. | 00, status 4 |
| 302.3 | 62% | **92% charging** (bud docked) | **level 0xff, status 4** |
| 307.6 | 62% | 00 status 4 | 00, status 4 |
| 400.4 | 61% | 00 status 4 | 00, status 4 |

So for the ~5 s the right bud was docked and still connected, the case entry flipped from `00` to `ff`
(present, level unknown) and never received a real level; 5.3 s later (preceded by ops 0x000c and
0x0008) the bud dropped to status 4. AirPods Pro apparently do not hand an AAP client the case level the
way iOS gets it (iOS also hears the case's own BLE advertisements). Levels and `charging` are decoded
correctly; there is simply no case level in the AAP stream in these conditions.

Changes installed afterwards (21:39): raw payload of every short AAP message (AAPM/AAPM2 marks), app
shows charging (`92%+`) and last-known levels for buds in a closed case (`~92%`) via ABI 8 status fields.

Plan B for the case level: listen for the buds'/case's BLE proximity advertisements (Apple company
0x004C, type 0x07: left/right/case battery nibbles + charging flags + lid state). The stack has an LE
layer (`BT::LeDeviceManager`, used for heart-rate monitors), so a scan hook is conceivable; it is a
separate project from AAP.

## LibrePods source review (kavishdevar/librepods @ 89356c0, 2026-10-16)

Read: `docs/AAP Definitions.md`, `docs/opcodes.md`, `android/.../BLEManager.kt`, `AACPManager.kt`,
`linux/ble/blemanager.cpp`, `linux/battery.hpp`, READMEs. Findings that matter here:

1. **Case battery comes from BLE advertisements.** LibrePods scans for Apple manufacturer data
   (company 0x004C) with data[0]=0x07 (proximity pairing), data[1]=0x19 (25 bytes). Plain layout
   (after the company id): [2] paired flag (0 = pairing mode, skip) · [3..4] model id (BE; 0x0E20 AirPods
   Pro, 0x1420 Pro 2, 0x2420 Pro 2 USB-C, …) · [5] status: bit5 primary=left, bit6 *this pod is in the
   case*, bit4 one pod in case, bit2 both in case, bits 1/3 in-ear · [6] pod batteries, low nibble /
   high nibble (swap when right is primary), value×10 %, 0xA–0xE = 100 %, 0xF = unknown · [7] high nibble
   flags (bit0/1 pod charging, bit2 case charging), **low nibble = case level ×10 %** · [8] lid: bit3 =
   closed, bits 0–2 open counter · [9] colour · [10] connection state (4 idle, 5 music, 6 call) ·
   [11..26] AES-128-ECB encrypted block: exact levels (left/right/case bytes, bit7 = charging,
   0x7F/0xFF = unknown). Linux only trusts the case level while *this pod is in the case*.
2. **Keys:** over AAP, request opcode `0x30` with type `01` (IRK) or `04` (ENC_KEY); response `0x31`.
   IRK verifies the advertiser's resolvable private address; ENC_KEY decrypts the block. Not needed for
   the 10 %-step plain values.
3. **AirPods hold two hosts at once** ("Bluetooth multipoint, 2 devices"), explaining test D.
4. Opcodes seen at docking: `0x0E` = audio source response, `0x0C` = undocumented sibling (maybe
   audio source 2), `0x08` unknown; none carries case data. `0x1D` device information (model number as
   2nd string) was received at open (238 bytes) but not stored; model of the user's AirPods not yet known.
5. LibrePods' docs show the AAP battery packet *can* include the case level (AirPods Pro 2 USB-C sample:
   case 17 %, right pod charging), so behaviour is model/firmware dependent; ours reports status 4.
6. Their current Android code sends feature flags `0xD7` (docs say `0xFF`); handshake and `0F` subscribe
   bytes are identical to ours. Linux note: AirPods may drop the link "expecting information from an Apple
   device" only when the host spoofs Apple's Vendor ID (not our case).

### LE path on the nano (not built)

The stack has an LE central role (`LeCentral` scan interval/window "levels", `LeDeviceManager`, GATT,
HRM data source) but only scans on demand (heart-rate monitor search). Needed: (a) start a low-duty LE
scan from the resident (stack API or HCI LE scan commands via the H4BC transport; driver table live at
`0x0896dd3c`, name string `H4BC`), (b) tap received LE advertising reports (HCI event 0x3E / subevent
0x02) before the stack's HRM filter, (c) parse type 0x07 and publish case/bud nibbles through the shared
state, (d) verify scanning does not disturb A2DP. Observation-first plan: step 1 run the stock HRM
search with the recorder on to see which LE messages and call sites appear; step 2 locate the HCI
RX path; step 3 prototype the tap read-only; only then scanning.

## LE advertisement listening — proven on hardware (2026-11-24, 22:xx)

**Tap:** `FUN_08439318` (RAM `0x08438f18`), the LE central's per-advertisement handler
`adv(LeCentral*, LeDevice*, int type, Report*)`; `Report+0x12` RSSI (s8), `+0x14` raw length (u32), `+0x18`
raw advertising bytes inline. Hooked with the SDK's absolute-jump helper (8 stock bytes
`2d e9 ff 4f 06 46 95 b0`, verified live), read-only, original always runs afterwards. Installed by
`btlog.c`; header flag `le-tap=1`. Records are `fmt = 0xFFFFFFxx` (RSSI in the low byte), decoded by
`tools/btlog.py` (Apple 0x07 proximity layout from LibrePods).

**Trigger (for now):** the stock heart-rate-monitor search (Fitness → Settings → Heart Rate Monitor).
Lifecycle observed: ~12 s of classic "Nike+" inquiry scans, then `power on of module LE` → LE scan
**5.2 s** (`Received 'stop scan' request from session "BTLESession"`) → `power off of module LE`.
The LE side is a separate Nordic module (`OI_Nordic_StartScan`).

**Result, three searches:** the AirPods were heard in every 5.2 s window (2, 2 and 1 advertisements; each
pod advertises with its own random address):

| State | Decoded advertisement |
|---|---|
| both buds in case, lid open | **AirPods Pro 2 (USB-C)** · L 80 %+ R 100 %+ **CASE 60 %** · lid open · both in case |
| R in ear, L in case, lid open | L 80 %+ · R 90 % · **CASE 60 %** · R in ear · one in case |
| both in ears, case closed | L 80 % · R 90 % · CASE unknown (`?`) |

(`+` = charging; nibbles are 10 % steps.) The environment also showed lots of other Apple advertisers
(types 0x09 AirPlay, 0x10 Nearby Info, 0x12 Find My), so the real implementation must pick *our* AirPods:
(a) cross-check the buds' levels against the AAP values, (b) strongest RSSI, (c) later: verify the
resolvable private address with the IRK (AAP opcode 0x30, type 1).

**Open questions / next steps:**
1. Self-triggered scan. `LeCentral::startScan(this, session, UUIDs*, allowDuplicates, flag)` is
   `FUN_0843ae00` (RAM `0x0843aa00`); sessions come from `FUN_080249a4("BTLESession", &handle)`
   (registerApplicationSession, used by the HRM data source constructor `FUN_0845a118`); stop request
   logs at call site `0x0843afbc`. Step 1: tap `startScan`'s arguments while the stock search runs
   (`this`, session handle, UUID vector, flags). Step 2: call it from the resident on an app-button press.
2. In-resident parse of type 0x07 and publish via `hb_aap_state_t` (case %, charging, lid, in-ear, age).
3. A2DP interaction while scanning (needs a test with music playing).
4. 1 % precision: ENC_KEY (AAP opcode 0x30 type 4) + AES-128-ECB over the last 16 advertisement bytes.

## Session 2026-11-24 (evening): scan control, keys, and the settings page

**Case battery over AAP works** when a bud sits in the open case for more than a few seconds: raw entries
`LEFT charging 99 %`, `CASE discharging 57 %` (earlier "disconnected/ff" was the 5 s docking window).
The app keeps the last valid case level (`~57%`, dimmed).

**RAM is larger than assumed.** The LE central lives at `0x0b355e10`; heap objects reach at least `0x0b4xxxxx`.
All range checks now use `0x08000000..0x0c000000`. (A too-small bound made every self-triggered scan refuse
itself: `LESTART … running=0xff`, so scan variants 1–3 are still **untested on hardware** after the fix.)

**LE scan API (all addresses Thumb, RAM):** `LeCentral::instance()` `0x083df961` (confirmed = observed `this`
`0x0b355e10`), `startScan` `0x0843aa01`, `stopScan` `0x0843aef9`, `registerApplicationSession` `0x080245a5`.
`session` for startScan/stopScan is only a map key plus a name read at `session+0x20` (COW string); the
session manager returns 0 safely for unknown keys. We pass our own static record. Stock HRM search call:
UUID vector of one entry, `allowDuplicates=0`, flag=1, ~5.0 s window.

**Key (step 4):** the AirPods answer the opcode-0x30 request with their ENC_KEY (`AAPKEYS 1 key`). The key is
RAM-only (never in the ring); AES-128 decrypt is `integration/aes128.c` (tested against two NIST vectors).

**Host tests (no device):** `tools/test_aes128.c`, `tools/test_aap_le.c` (real captured advertisements +
synthetic encrypted one), `tools/ui_preview/` (headless LVGL render of the actual page code; `render.sh`;
previews in `docs/ui-preview/`).

**Settings page** (`app/aap_ctl/aap_ui.c`, iOS 16/17 grouped style): hero with buds/case/battery glyphs, Name,
NOISE CONTROL segmented card (icons; active mode named below), PRESS AND HOLD, toggles (ear detection,
conversation awareness, one-bud NC) with footnotes, ABOUT (model/serial/firmware from AAP opcode 0x1D),
debug block (Connect / Scan). `aap_models.c` is the model table: rows a model lacks are hidden. Art is drawn
from shapes; real icons/PNGs to be added later. ABI 10 adds device info, `ctl_val[]`/`ctl_val2[]`, `req_ctl`.

## Test of the first settings-page build (2026-11-24, 23:xx)

**Connect regression, fixed.** After the channel open the firmware's write path answered `0x194` to every
frame for 1.8 s (its lookup `FUN_083b0c50(cid)` finds no channel object until the remote accepts the
connection); the sender counted those as failures and spent all ten tries in the first 90 ms, so the handshake
never went out and the page sat on "Connecting…" (open channel, `rx_count` 0). `0x194` is now "not ready":
retry after 250 ms, no budget spent (limit ~20 s).

**Self-triggered LE scan: accepted, but radio silent.** `startScan` with our own session record returned 0 and
`stopScan` 0 after 5.2 s, yet `raw=0` (no advertisement reached the handler) and none of the stock side effects
appeared (`power on of module LE`, `Stopping scan - no remaining scan agents permitted to scan`). Reading the code:
 - the stack only lets a session scan if it is in the session manager's map (`FUN_082b3910(mgr, key, p3, flag)`
   inserts it, with policy bytes at node+0x58/+0x59 from a policy object keyed by the session *name*); the
   stock heart-rate source reaches it via a callback thunk (`0x084595d1`) that calls `session->vtable[0xb0]`,
   so the stock session is a real C++ object we cannot cheaply fake;
 - the LE module (a separate Nordic part) is powered by something in an undecompiled region;
 - an agent with an empty UUID filter matches every advertisement and would call the session's delegate
   (`node+0x14`), which a dummy session does not have: a crash risk once scanning actually works.
Options if this is ever resumed: (a) bootstrap: tap the stock HRM `startScan` once per boot, copy its session
pointer and UUID entry, reuse them; (b) find the module-power call and the session's delegate. Neither is needed
for the settings page: the case level arrives over AAP whenever a bud is in the case.

## Page v2: one fixed screen (2026-11-24, 23:20)

Test of the first settings-page build: connect fixed and working (battery, case, info, listening modes OK).
**The three toggles do nothing.** The AirPods announce `0x28` (conversation awareness) = 2 and `0x1b` (one-bud
NC) = 2 at connect, and no ear-detection setting (`0x0a`) at all; our `09 00 id 01` commands were accepted by the
link (rc 0) but **never echoed** by the AirPods (the listening-mode command *is* echoed), i.e. ignored from this
host. Removed, along with rename, press-and-hold and the BLE scan UI.
**Sluggish scrolling** came from a tall scrolling page with clipped corners, opacity layers and switches. The page
is now one fixed 240x432 screen with absolute positions and flat shapes (`app/aap_ctl/aap_ui.c`): hero (buds/case
with batteries), NOISE CONTROL, ABOUT (model, serial, firmware). A frame probe logs `UIFRM frames-per-2s max-gap-ms`
to the trace ring (`./start trace`). Previews: `docs/ui-preview/`.

## Page v3 and the reconnect bug (2026-11-24, 23:50)

**"Tap to connect did nothing / the app looked dead."** The frame probe showed the UI loop healthy (~119 frames
per 2 s, worst gap 18 ms). The cause was in the resident: after the AAP session ended (AirPods went away,
`AAPEND`, A2DP down) and A2DP came back, the reopen gate (`s_need_new_link`) demanded a *different* AVDTP cid
than the last session's, but the firmware reuses the same cid after a reconnect, so the gate never opened.
Now "A2DP link down" (`link_avdtp == 0`) clears the gate, and `want_open = 2` (the page's "Try Again") clears every
back-off. `link_avdtp` and per-control echo counters (`ctl_seq[]`) are published (ABI 11).

**Page.** Apple-style empty state ("Connect Your AirPods") when there is no link, "Connecting..." when A2DP is up
but AAP is not, and the scrolling page when connected (hero, NOISE CONTROL, SETTINGS rows a model has, ABOUT).
Taps are optimistic: the segment/switch moves at once; the AirPods' echo (`ctl_seq` changes) confirms it; no echo
within 2 s snaps it back and shows "The AirPods did not accept that change." Feature flags changed to `0xD7`
(current LibrePods) in case the AirPods gate settings on the host's declared capabilities.

## Page v4: real art, notice slot, bolt (2026-11-24, 23:50)

Test of v3 on the device (user): empty/connecting states good, connecting is fast, the three mode buttons
switch correctly. The three Settings toggles still snap back with the amber "did not accept" note, which the
user accepts for now. Two layout bugs: the note wrapped to two lines but only one was reserved, so it sat
on top of the ABOUT rows (Serial); and at 100 % while charging the charge bolt touched the "100%" text.

- Note: a fixed two-line slot under the settings group. Bolt: moved beside the battery outline (clear of the
  number); percentage label centred on the 72 px column (it was 8 px off-centre).
- Real art: `tools/make_art.py` converts Apple's renders from a local folder (`ASSET_DIR`)
  (left, right, closed case per generation; Max is one picture) into `app/aap_ctl/aap_art.[ch]`.
  Format RGB565, pre-composited on the black page background, error-diffused. Reason: the device LVGL
  (sdk/lv_conf.h) has `LV_DRAW_SW_SUPPORT_ARGB8888 0`, so alpha images cannot be drawn, RGB565 can.
  The LED baked into the case renders is found, painted out, and redrawn live (green / amber), position
  exported per set. 9 sets, 125 KB of pixels; app 249 KB -> 377 KB. A model without a set (unknown) keeps the
  drawn stand-ins; the empty state shows the Pro 2 set (or the last known model's).
- Model table gained an `art` id and AirPods Pro 3 (BLE id 0x2720 = PID 0x2027 byte-swapped, the pattern
  every other id follows; model numbers unconfirmed so it only matches by BLE id).
- Previews: docs/ui-preview/{connected,settings-and-notice,models-and-states}.png (`tools/ui_preview/render.sh`).

## Device-ID spoof (2026-11-25, 00:10)

The nano registers its Bluetooth Device-ID (SDP DI) record in `BT::StackManager::startupStack` through
`OI_DI_Register` (corpus FUN_080f38c0, RAM 0x080f34c0). The record it passes: vendor 0x05AC, product 0x1266
(iPod nano 7G, USB id), version 0, primary = 1, **vendor id source 2 (USB-IF)**. LibrePods' "VendorID
spoofing" (android/app/src/main/cpp/l2c_fcr_hook.cpp, `fake_BTA_DmSetLocalDiRecord`) sets vendor 0x004C and
source 1 (Bluetooth SIG) and nothing else; AirPods then treat the host as an Apple device.

`integration/did_spoof.c` taps `OI_DI_Register` with the stock-bytes trampoline pattern (8 prologue bytes:
`push.w {r4-fp,lr}; mov.w r7,#7`), edits rec[0] and rec[6], and runs the original. Recorder marks:
`DIREG` (as built) and `DIREG2` (as registered). Verified on the device at boot, 0.465 s, i.e. the hook
wins the race against the first stack start by a hair, and on every later stack restart:
`DIREG a=0x5ac0002 b=0x12660000` -> `DIREG2 a=0x4c0001 b=0x12660000`.
Opt out: `/Apps/Data/did.off` (then it only logs). The remote may cache the record from pairing: forget and
re-pair the AirPods once if nothing changes.

Expectation, from LibrePods' README: the features gated by the spoof are Loud Sound Reduction, Hearing Aid,
Transparency customization and multipoint. Conversation Awareness and one-bud NC are listed as working
without it, so the spoof is not expected to fix our three toggles; this test is mainly to see whether the
AirPods announce more settings after connect (compare the 0x09 identifiers in the recorder with ring-v13)
and whether the link stays up ("AirPods may drop the link expecting information from an Apple device").

## Page v5: proportional buds, SF Symbol icon, render benchmark (2026-11-25)

- **Bud size.** Apple's renders of one generation share a pixel scale (bud height / case width in the files
  matches the real hardware: AirPods 3 0.57, Pro 0.52, 1st/2nd gen 0.78 of the case height). `make_art.py` used
  to fit every picture into its own box, which blew the buds up to 64 px next to a 51 px case. Now one scale
  per set (case fits 68 x 60), buds follow it: a Pro 2 bud is 26 x 35 px next to a 68 x 51 case. Pro 3 uses
  `left-small/right-small@2x` (the plain `left@2x` is a different, larger render). Pixel data 125 KB -> 90 KB.
- **App icon.** `tools/make_icon.py` turns the SF Symbol `airpods.pro` (airpods-assets/mockup/symbols/hero) into
  `app/aap_ctl/icon_art.png` (white, cropped to its outline). NanoApps' `build_apps.py` already fits an
  `apps/<name>/icon_art.png` inside the glossy disc instead of a FontAwesome glyph. Disc colour unchanged (#1b9e77).
- **Speed, desktop numbers (relative only; x86 -O1, DIRECT mode like the device).** Full-page refresh 558 us:
  boxes (noise-control icons, settings rows, about) 65 %, all text 29 %, the bare fill+flush floor 7 %,
  pictures 2 % (RGB565 vs XRGB8888 makes no difference). A tap repaint of one row is 95 us (17 % of a frame).
  So scrolling costs a full frame per step; taps are cheap. The device's real numbers are needed:
  `AAP_BENCH=1` (Makefile default for this test build) makes the app sweep the page once after launch under
  six variants and log `BFULL BNOIMG BNOTXT BNOBOX BNONE BTAP` (avg us, max us) plus `BDONE` to `./start trace`;
  always on: `UIFRM` (frames, longest gap ms), `UIPOLL` (avg, max us in the resident poll), `UIREN`/`UIRENX`
  (frames that drew, avg us; longest). Options once numbers exist: poll throttling, LVGL at -O2 (separate
  archive), a cached RGB565 snapshot of the page used only while dragging, fewer objects in the icons.

## The toggles worked all along (2026-11-25, 00:20)

User test with the Device-ID spoof active: link stable through two sessions (A2DP drop and reopen), the AirPods
announced exactly the same control ids at connect as without it (0x0d 0x18 0x17 0x25 0x23 0x1f 0x24 0x28 0x26
0x29 0x2c 0x2f 0x33 0x2e 0x1b 0x35 0x3e), so the spoof unlocks nothing the page uses. The page still said
"did not accept that change".

The recorder (ring-v13, ring-did2) shows why that note was wrong. The AirPods announce 0x28 (conversation
awareness) and 0x1b (one-bud NC) once, at connect, and send no 0x09 after a write, in any session. But the
value announced at the NEXT connect is always the last value we wrote:

| write | next connect announces |
|---|---|
| 0x28 = 1 (v13, t=375) | 0x28 = 1 (t=1418) |
| 0x28 = 2 (t=1418) | 0x28 = 2 (t=1568) |
| 0x1b = 2 (t=381, 409) | 0x1b = 2 (t=1418, 1568) |
| 0x1b = 1 (t=1621) | 0x1b = 1 (did2 t=317, 406) |
| 0x28 = 2 (did2 t=320, 344) | 0x28 = 2 (t=406) |

So the writes are applied; "no echo" was misread as "ignored". Listening mode (0x0d) is the one setting the
AirPods do echo. Fixes: the resident records a sent write as the current value (`ctl_val[id]`, `ctl_seq[id]++`,
mark `AAPCTLW`); the AirPods' next announcement overwrites it with the truth. The page no longer reverts a
switch; its note now only means "could not send". Automatic Ear Detection (0x0a) is dropped from the page: the
AirPods never announce it, so its state cannot be shown or verified. The Device-ID hook stays installed but is
OFF by default (`/Apps/Data/did.on` enables it).
Lesson: absence of an echo is not evidence of rejection; check the state announced at the next connect.

## The benchmark build was unusable; what it did tell us (2026-11-25, 00:35)

The test build with the on-launch benchmark (`AAP_BENCH`) broke the app: it swept the page ~100 times right
after launch, and the app's frame callback does the Bluetooth poll, so for that whole time the poll did not
run. Ring evidence: A2DP came up at t=133.3 s, AAPOPEN only at 145.5 s (normal is 3-5 s: ring-did2 311.9 -> 315.9,
400.5 -> 404.6), i.e. the sweep took ~12 s; the user saw an auto-scrolling fake page and an app that looked
dead. Lesson: nothing in the user's test path may block the frame callback; measure passively (probes, one-shot
timings), never with a scripted sweep. `AAP_BENCH` now defaults to 0 and says why in the Makefile.

Number it left behind: ~100 steps in ~12 s means a full-page refresh costs on the order of 100 ms on the
device, versus 0.56 ms on the desktop (a 25x CPU gap would give ~15-20 ms). Idle is fine (UIFRM: 119 fps,
longest gap 19 ms; UIPOLL: poll avg 25 us, max 250 us, so the poll is not the problem). Hypothesis: LVGL renders
straight into the display framebuffer (DIRECT mode, `sdk/hb_lv_surface.c`), which is not cached, so every
blend's read-modify-write is a slow bus access. Fix being tested: `HB_LV_BACKBUF` (opt-in, set in the app's
Makefile): LVGL renders into a 240x432x4 static back buffer in the app's .bss (cached heap memory) and the
flush copies only dirty rectangles to the framebuffer (it already did a per-pixel pass there to force alpha).
Measured by the always-on probes: `LVFIRST` (first full refresh, us, logged once at launch) and `UIREN`/`UIRENX`
(frames that drew, avg/max us). Success looks like a full refresh well under 30 ms.

Icon: the two-pod `airpods.pro` symbol read as tall and stretched in the round disc; now the single-pod
`airpods.pro.left` (SF Symbols "parts" set).

## Icons v2 (2026-11-25)

The user, after the benchmark build was removed: "the ui is better, icons still don't look good, nowhere near stock apps".
Two sets of icons were rough, both fixed:

- **Home-screen icon.** The flat SF Symbol silhouette (`airpods.pro.left`, white on muted teal) was lopsided; the two-pod
  symbol was tall. Now ONE real Apple AirPods Pro bud render (`airpods-pro-2/left@3x`), tilted 32 degrees so it fills the
  disc diagonally like the stock glyphs, with a soft shadow, on a vivid green (`#34c759`, no other app uses it).
  `tools/make_icon.py` -> `app/aap_ctl/icon_art.png`. (build_apps.py scales the art to the disc at 4x supersampling.)
- **Listening-mode icons.** They were assembled from LVGL rectangles, circles and 3x3 dots - crude at 30 px. Now
  anti-aliased A8 images drawn at 16x supersampling by `tools/make_icons.py` -> `aap_icons.[ch]` (about 1 KB each), tinted
  white by LVGL (the device build supports A8). Off = thin ring, transparency = ring of dots, adaptive = dots that swell
  around the ring, noise cancellation = heavy ring; a person (head and shoulders) inside.

## Page v6: quieter, more iOS (2026-11-25)

User: settings apply now; the green icon is too contrasty (want gray); drop the green "Connected" line; more speed and
more modern-iOS polish.

- Icon disc is graphite `#646469` (Info.plist HBIconColor), the bud keeps its own colours.
- "Connected" line removed (the page showing at all means connected; the empty/connecting states already say otherwise).
  Hero row and everything below moved up 10-12 px (`HERO_Y`, `NC_Y`).
- Touch feedback: segments and rows highlight while pressed (white at 10-20 %), like iOS table rows.
- Two tiny animations: the noise-control selection is ONE pill that slides (160 ms ease-out) instead of four backgrounds
  that switch; a switch knob rolls (130 ms). Both repaint only a few small rectangles.
- Measured on the device (trace, back-buffer build): a frame that really draws (scrolling) costs 26-37 ms (UIREN/UIRENX),
  idle frames are free (UIPOLL avg ~17 us). `LVFIRST` logged 0 (the timer read around lv_refr_now does not tick as
  assumed; do not trust it). Two 320-340 ms gaps between frames appeared near AAP traffic (mode change echo); not
  inside our poll (max 77 us) nor in a long render, cause not found.
- Not done, candidates: an SF-like typeface (Inter, OFL, generated with lv_font_conv; the biggest "iOS" cue left),
  a cached bitmap of the page during scroll drags (only if 26-37 ms/frame is felt as sluggish).

## Page v7: iOS icons, Inter, page cache (2026-11-25)

User: the noise-control icons "are more like this" (a screenshot of iOS Settings > AirPods), and "build the scroll
optimisation and the typeface too".

- **Mode icons** redrawn to match iOS (`tools/make_icons.py`): ONE solid person (head + shoulders); the halo around the
  HEAD says the mode - thin faint ring (off), ring of dots (transparency), thick ring (noise cancellation), no ring but two
  sparkles (adaptive). The shoulders cut the halo with a small gap. Dots sit on whole pixels (2x2 blocks) so they read
  equally bright at 1x: the previous version, with sub-pixel dots, looked uneven on the device.
- **Typeface**: Inter (SIL OFL 1.1, `docs/fonts/Inter-OFL.txt`) at 14/16 regular and 20 semibold, ASCII only, 4 bpp,
  generated by `tools/make_fonts.sh` (lv_font_conv from npm + @fontsource/inter) into `aap_font_{14,16,20}.c`
  (~40 KB of source each, a few KB of data). The charging bolt keeps Montserrat (Font Awesome glyph).
- **Page cache** (`aap_snap.[ch]`, `aap_ui.c` "page cache"): all page widgets now live in one tall `content` object.
  Once the page has been still for 400 ms (no tap, no animation, no finger down) it is drawn off screen into an RGB565 picture
  (240 x ~620 = ~300 KB from the LVGL pool; the routine is LVGL's lv_snapshot.c vendored, because the shared LVGL
  archive has LV_USE_SNAPSHOT 0 and changing the shared config for one app is not worth it). While a drag or fling is in
  progress (`LV_EVENT_SCROLL_BEGIN` .. `SCROLL_END`) the picture is shown instead of the live widgets, so each step is one
  image blit instead of a full re-render (26-37 ms measured before). At scroll end the live widgets return (same pixels:
  the card greys are defined through `Q565()` so they survive the RGB565 round trip exactly). If the picture is not ready
  or memory is short (3 failures and it stops trying) the page scrolls live as before.
  Desktop test with a simulated finger (`SCROLLTEST=1 preview out.ppm 0 432`): picture taken, on screen during the drag,
  gone after SCROLL_END, picture vs live frame at the same scroll: largest channel difference 10 (text edges only).
- **Instrumentation** (passive, 2 s): `UIFLUSH us px` (time copying dirty rectangles to the display, pixels copied; from
  `sdk/hb_lv_surface.c`, HB_LV_BACKBUF only) and `UISNAP pictures<<16|failures, last_ms<<24|biggest_free_block`.
  What to read after a scroll test: `UIREN` average (was 26-33 ms) against `UIFLUSH` (is the copy itself the floor?),
  and `UISNAP` (did the picture fit in memory, how long it took to draw, how much pool is left).

## Bud spacing; caption clip (2026-11-25)

User: first "the pods for some models face away from each other", then, after checking Apple's reference material,
"the direction is fine, the weird thing is the distance between them"; also "the g of 'Right' is cut by a few pixels".
- Orientation: Apple's left/right renders, left render in the left slot, are correct for every model. A swap for
  AirPods 1/2 and Pro 1/2/3 was tried and reverted (make_art.py keeps a note not to do it).
- Spacing: each bud was centred in its own 72 px column, so the pair sat 72 px apart, far more than a pair looks in
  Apple's artwork. Each bud now moves `BUD_PULL` (12 px) toward the other inside its column (aap_ui.c `slot_art`), the
  case and the battery columns stay put; the Max (one picture) is not moved. Compared 0/10/14/18 on the preview: at 18 the
  Pro buds touch, 10-14 read as a pair. One constant to tune.
- The caption "Right" lost its descender because the 128 px column clipped its child (LVGL clips children to the
  parent) and Inter's line is 18 px tall (Montserrat's was 16). Column height 128 -> 138.
- **Result: the closer spacing looked weird on the device and was reverted** (`BUD_PULL` is 0; each bud is centred in its
  own column again). What the user meant by "the distance between them" is still open. Candidates: the gap between
  the buds and the case, or the gap between the two buds relative to each bud's size.
- Preview scenarios 11 (AirPods 2) and 12 (AirPods 4) added; `docs/ui-preview/` refreshed.

## Toward 60 fps: what the device measured, and the direct presenter (2026-11-25)

User: revert the bud spacing to what it was before the last build (done: `BUD_PULL` 0, centred, same as before); "any more
performance we can extract so we can get 60fps?"

**Measured on the device (page-cache build, trace):**
- The cache works: 4 pictures taken, each 28-32 ms, 322 KB of pool still free after a 296 KB picture (`UISNAP`).
- But a scroll frame still cost 25.0 ms on average (UIREN, three windows within 0.1 ms of each other), the same as before the
  cache (26 ms). The copy to the display (`UIFLUSH`) is 57 ns per pixel = 5.9 ms for a full screen, so ~19 ms was LVGL's own
  work even when it only blitted one picture: its RGB565 -> XRGB8888 image blend is a per-pixel loop with three
  multiplies and three byte stores (`lv_draw_sw_blend_to_rgb888.c`, `rgb565_image_blend`), ~190 ns per pixel on this CPU.
  A live full-page render was up to 70 ms (UIRENX at the end of a scroll, the swap back to live widgets).
- Touch is sampled by LVGL's indev timer every 30 ms by default, so a drag could only move 33 times a second.

**Changes:**
1. `hb_lv_present565` (sdk/hb_lv_surface.c, HB_LV_BACKBUF apps): writes rows of an RGB565 picture straight to the
   display, two pixels per word read, three small tables for the conversion with LVGL's own rounding
   ((r5 * 2106) >> 8, (g6 * 1037) >> 8), so presented and LVGL-drawn frames agree to the pixel. Cost ~ writing display
   memory (about 6 ms for the full screen at the measured 57 ns/px, plus the conversion).
2. While a scroll is in progress (`SCROLL_BEGIN`..`SCROLL_END`): `lv_display_enable_invalidation(false)` keeps LVGL from
   drawing, and the app's post-render hook calls `aap_ui_present_tick()` each frame, which presents the cached picture at the
   current scroll position (only when it changed). At `SCROLL_END`: invalidation back on and the whole screen invalidated, so
   the live page is drawn once. A 2 s watchdog releases the display if `SCROLL_END` were ever missed. Without a registered
   presenter (the desktop preview) the earlier path is used: an lv_image of the picture over the hidden live widgets.
3. Touch read period 30 -> 8 ms and fling slow-down 10 % -> 3 % per read, so a drag follows every frame and a fling travels
   about as far as before (the slow-down is per read, so a shorter period needs a smaller percentage).
4. Palette greys use LVGL's own RGB565 rounding (`Q565`), so the live page and the picture match exactly on flat areas.

**Desktop test** (`SCROLLTEST=1 [FASTPATH=1] preview out.ppm 0 432`, simulated finger): direct present shown during the drag,
LVGL leaves it untouched, live frame at the same scroll differs by at most 10 levels (text edges only).

**What to read on the device after a scroll test (`./start trace`):**
`UIPRES n avg_us` (cost of one scroll frame in fast mode: goal < 10 ms), `UIPRESX longest_us, ms<<16|frames` (frames/ms = rate,
goal >= 60 per second), `UIFLUSH` and `UIREN` (should now only show the one live frame per scroll end), `UISNAP`.
If `UIPRES` is near the flush figure the floor is the display write; a burst (ldm/stm) copy loop is the next lever.

## After the direct presenter: "scroll is fast but the bounce animation is slow" (2026-11-25)

**Measured (trace of the user's run):** a direct present costs 8.4 ms (`UIPRES` 8360-8514 us, longest 10.6 ms), 42-66 presents
a second while scrolling (the frame loop itself runs ~118 Hz; a present + the next tick makes the cadence ~17 ms, i.e.
~60 at best), and the live redraw at scroll end is now 30-34 ms (it was up to 70). The 8.4 ms is the display write
(57 ns/px) plus the conversion; the next lever would be a burst (ldm/stm) copy.

**"Bounce":** the page has no overscroll bounce (elastic is off; checked with a simulated drag past the top: scroll_y stays
0), so the only animations are the mode-selection pill (160 ms) and the switch knob (130 ms). Taken as those. Two causes of
"slow" found and fixed:
1. Every page update called dozens of LVGL setters, and LVGL repaints on each call even for an unchanged value, so a tap
   repainted ~15 areas (header, all three columns, icons, 40k px) BEFORE the pill moved; the first frame of the animation was
   the expensive one. `set_hidden/set_text/set_bg/set_tc/set_bgopa/set_xx/set_yy/set_ww/set_hh/set_pos` now touch LVGL only
   when the value differs (they compare configured STYLE values: `lv_obj_get_x/y` read layout coordinates, which are stale
   before the first layout pass and wrongly skipped a move: the one-AirPod row vanished in the Pro 1 preview until fixed). A mode
   switch now repaints the caption row (226 x 30) and then 60 x 46 per animation frame (measured on the desktop with `ANIMTEST=1`).
2. Durations 160 -> 120 ms (pill) and 130 -> 100 ms (knob).
Fling physics checked on the desktop (`FLINGTEST="period throw"`): with the 8 ms touch period the integer slow-down decays by
1 px per read (3 % and 1 % behave the same), which gives a smooth linear deceleration and flings about as long as before.
