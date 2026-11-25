# Findings — osos decrypted and analysed

Image: 2012 nano 7G, firmware **1.1.2 / 39A10023**, decrypted on-device (AES engine in DFU via
wInd3x/S5Late). `build/osos.bin`, 9,914,592 bytes, sha256
`c2f90ea2e33d233c58731e0c252c79ba43992f4e1e62084f1c9567de06b497cb`.
Load base `0x08000000`, ARMv7/Thumb-2, raw image.

## G0: what the image says

`strings` on the decrypted image: **42,914 strings**, and the Bluetooth stack is right where
blueth4w said it was:

| Evidence | Meaning |
|---|---|
| `This product contains the BLUEmagic software Bluetooth` | confirms OI BLUEmagic host stack |
| `RTXC` (12×), `BootTask`, `MainAppTask` | Pixo/RTXC RTOS task names |
| `BT ConnectionMgr`, `BT L2CAP TXLoop`, `L2CAP initialization failed`, `L2CAP Test` | an L2CAP module exists and logs |
| `AVCTP`, `AVDTP`, `L2CAP`, `RFCOMM` (packed strings at `0x080fac58`–`0x080fac68`) | protocol name table |
| `OI_AVRCP_RESPONSE_ERROR_INVALID_SCOPE` | BLUEmagic AVRCP API error enum |
| `AVRCP (RemoteClient)::Profile()`, `registerProfile()`, `Failed to register as an AVRCP Target` | Apple's C++ wrapper over the stack |
| `BT::LeDevice *BT::deviceFromConnectionHandle(_OI_HCI_CONNECTION *, bool)` | **LE device layer is present** — relevant to "dual mode" |
| `A2dpStreamConfig`, `OI_AVDTP_*` | A2DP/AVDTP profile layer |

Conclusion: **there is no exported `L2CAP_Connect` symbol**, and no absolute-pointer reference to
the L2CAP name strings either — channels are opened inside the stack, called through function
pointers, not by a public API with a PSM argument. That matches Apple only ever wiring up
AVDTP/AVCTP/RFCOMM.

## Ghidra

- Imported at `0x08000000`, `ARM:LE:32:v7`, analysis **succeeded**: **26,521 functions** recovered
  (`build/functions.txt`), all auto-named `FUN_*` — the image's debug strings are not a symbol table
  as far as Ghidra is concerned.
- **The 12.2 release zip ships decompiler natives for `linux_x86_64` and `win_x86_64` only.**
  On Apple Silicon that means disassembly, xrefs and strings, but no decompiler
  (`ERROR os/mac_arm_64/decompile does not exist`). Fix in progress: install the Homebrew
  `ghidra` formula (12.2, bottled, presumably with `mac_arm_64` natives) and graft its
  `Features/Decompiler/os/mac_arm_64` into the local tree, so the existing project keeps its
  analysis.
- Working scripts: `tools/ghidra_scripts/DumpFunctions.java`, `FindBt.java`, `RefsFromCsv.java`;
  `tools/scan_l2cap_calls.py` (capstone Thumb scan for `bl` into the L2CAP module + PSM constants).

## First anchors

| Address | Evidence | Guess |
|---|---|---|
| `0x080cea74` | references `"BT L2CAP TXLoop"` at `0x080ceadc`; `push {r4,lr}`, sets a 3-word state struct, calls `0x0801ea38`, `0x083b59f8`, `0x083d5c6c` | L2CAP transmit-loop entry / init |
| `0x080cea74` caller `0x080f7b40` | small wrapper, tail-calls `0x084e9e20` | L2CAP init glue |
| `0x0804cfdc` | references `"AVRCP (RemoteClient)::Profile()"` | Apple AVRCP profile object |
| `0x080c8000`–`0x080d4000` | ~91 functions, contains the L2CAP log strings | candidate **L2CAP module** |
| `0x082b0000`–`0x082d0000` | A2DP/AVRCP/A2dpStreamConfig strings; RTP fix lives at `0x082b2bd6` | A2DP + AVRCP upper layers |

No `bl` from outside into `0x080c8000`–`0x080d4000` was found by the capstone scan, which
strengthens the "channels are opened via function pointers / dispatch tables" hypothesis.

## Next questions for Ghidra (with decompiler)

1. What is the signature of the function that opens an AVDTP channel (PSM 0x0019), and where does
   the PSM immediate come from — a literal, or a registration-table field?
2. Is there a profile/PSM registration table (name + PSM + callbacks) we can extend, rather than a
   hard-coded open call?
3. Where do L2CAP receive callbacks get dispatched, and on which task?

## Address translation (verified live over SCSI)

The decrypted `osos` file carries a **0x400-byte IMG1 payload header that is not loaded**:

```
RAM address = 0x08000000 + (file_offset - 0x400)
```

Verified by peeking live DRAM from the Mac (`ipod_sg_raw` + SCSI `c6 98 02`, read-only):

| file offset | predicted RAM | peeked | content |
|---|---|---|---|
| `0x0ceadc` | `0x080ce6dc` | match | `BT L2CAP TXLoop` |
| `0x4e6d00` | `0x084e6900` | match | `L2CAP initialization failed\n` + `LE GAP initializa…` |
| `0x2b2fd6` | `0x082b2bd6` | match | NanoBtFix's published patch address — cross-validates both |

And the BT RTP patch is confirmed **live on the device** at `0x082b2bd6`:

```
live:  25 49 | 48 68 00 eb cc 10 48 60 | 00 bf …      <- patched
file:        | 01 ee 10 ca 91 ed 01 0a …             <- stock
```

i.e. NanoApps' resident ran `hb_bluetooth_fix()`. This is also an independent confirmation that our
decrypted image *is* the firmware this iPod runs.

**Consequence:** the first Ghidra project (base `0x08000000`) has every address off by `+0x400`.
`tools/ghidra-import.sh` now maps at `0x07ffffc00` so labels in the decompiled C are real RAM
addresses. All addresses in earlier notes must be shifted by `-0x400` to be used with `peek`/patches.

## Live memory access from the Mac (no DFU, no Linux, no sudo)

`NanoApps/tools/macos/ipod_sg_raw.c` compiled locally gives `sg_raw`-compatible SCSI from macOS:

```sh
clang -O2 -o ipod_sg_raw ipod_sg_raw.c -framework IOKit -framework CoreFoundation
./ipod_sg_raw -o out.bin -r 512 auto c6 98 02 08 0c e6 dc   # peek 512 bytes at 0x080ce6dc
```

Works with the iPod on the Home screen, connected normally.

## L2CAP internals (from the decompiled corpus, verified live where noted)

### The channel table — 24 slots of 0x88 bytes (verified live)

`FUN_080f7b40` (L2CAP init) reads config key `0x2f` via `FUN_08417708` and allocates:

```c
count_links = *(u16*)cfg0x2f;        // = 0x0a (10)
iVar = FUN_0840ec40(count_links << 3);   // 10 * 8-byte link entries
*DAT_084e9eb0 = iVar;                    // live: 0x0896d92c
...
FUN_080f2430();   // count_channels = *(u16*)(cfg0x2f+2) = 0x18 (24)
                  // -> FUN_0840ec40(24 * 0x88)  = CCB array, live: 0x08d02fe4
```

State block live at `0x0895c080`: `{u16 in_use=1, u16 channels=24, u32 ccb_array=0x08d02fe4}`.

### CCB layout (0x88 bytes)

| offset | type | meaning | source |
|---|---|---|---|
| `+0x00` | u8 | state: 0 free, 2 open/connected, 3 allocating | `FUN_080f2128` treats 0 as free |
| `+0x02` | u16 | **profile id supplied by the caller** (5 and 6 observed live) | `FUN_080f21c0` |
| `+0x04` | u16 | cid | observed |
| `+0x24` | u32/bytes | next/order pointer + flags | `FUN_080f7da0` |
| `+0x28` | u16 | **MTU** — allocator writes 672 (`0x2a0`); AVRCP caller overwrites with `0x17` | `FUN_080f21c0`, `FUN_080eb4b0` |
| `+0x2a` | u16 | flush timeout (`0xffff`) | same |
| `+0x2e` | 6 bytes | remote BD_ADDR | `FUN_080f2274`, `FUN_080f2324` match on it |
| `+0x34` | u8 | connection sub-state | `FUN_080f7da0` |
| `+0x39` | u8 | "ready" flag | `FUN_080f2324` |
| `+0x3c` | u16 | id handed to the callback | `FUN_080f21c0` |
| `+0x40`..`+0x48` | u16 | mtu/flush/latency/timeouts | `FUN_080f2128` for state 2 |

### Registration and teardown

```c
/* FUN_080f21c0 (RAM 0x080f1dc0) — allocate a channel slot for a profile */
int reg(int id, int arg1, int arg2, CCB **out, int cb_id)
{
    for (i = 0; i < n_channels; i++)          // reject duplicate id
        if (ccb[i].state && ccb[i].id == id) return 0x192;
    find free slot; memset(ccb, 0, 0x88);
    ccb->id = id; ccb->state = 3;
    ccb->cb_id = cb_id;
    ccb->mtu = 0x2a0;      /* 672 */
    ccb->flush = 0xffff;
    *out = ccb;
    FUN_083d5c6c(ccb->id, arg1, arg2);
}
```

Callers (only two): `FUN_080eb4b0` (id 6, then sets `mtu = 0x17`) and `FUN_080ebb28` (id 5).
Both live in the Apple profile-registration layer at `0x080e8-0x080ee`.

`FUN_080f7da0` (RAM `0x080f79a0`) tears a channel down by link handle, calls
`thunk_FUN_080ceaec(cid)`, zeros `+0x28..+0x2b`, re-resolves the peer by BD_ADDR
via `FUN_080f7cf0`, and dispatches `(**(code**)(ccb+0x30))(...)` to a callback.

**The PSM never appears as an immediate anywhere near this code** — the profile id at
`+0x02` is the only caller-supplied discriminator, so the mapping id → PSM has to be
resolved deeper in the signalling layer.

### The message-based API

`FUN_0841b0f4(op, a1, a2, a3, a4)` is the stack's internal command dispatcher; the L2CAP
API wrappers are trivial forwarders, e.g.

```c
FUN_080f7608(a)                  -> FUN_0841b0f4(0x94, a)
FUN_080f76fc(a, b)               -> FUN_0841b0f4(0x68, a, b)
FUN_080f7706(a, b, c, d, e)      -> FUN_0841b0f4(0x37, a, b, c, d, e)
FUN_080f771e(a, b, c)            -> FUN_0841b0f4(0xb4, 0, a, b, c)
```

30 call sites across the stack use 29 distinct opcodes (`0x0b`..`0xb4`). The opcode→handler
dispatch is not visible as a comparison chain in the L2CAP module, so it is either a jump
table or resolved by the message payload's class byte.

### Live observation

`tools/capture-l2.py` snapshots both tables over SCSI. With the nano idle:
**0 live channels of 24**, and the three stale slots carry ids 5/6 and MTU 0x17 —
i.e. AVRCP registrations from an earlier session, torn down cleanly.

## Open question (the last one before G0 closes)

Which call opens an **outgoing** channel to a given PSM, and where does the PSM come from?

Options, in order of preference:
1. Find the L2CAP API object (the C++ interface used by A2DP: `piVar1 = FUN_08410bf0()`,
   `(**(code**)(*piVar1 + 0xNN))(...)`) and call its connect method with `0x1001`.
2. Hook `FUN_080f21c0` with our own code and observe the arguments AVDTP passes when it
   connects, then replicate the call with a different PSM.
3. Live-capture the CCB table while A2DP is streaming to see how an AVDTP channel appears
   (state/id/mtu), which narrows which field carries the PSM.

## G0 ANSWERED: there is a connect-by-PSM entry point, and it accepts 0x1001

### Correction to the CCB layout above

The PSM is **not** at `+0x28` (that is a u32 pointer). It lives at **`+0x04`** as a u16. Confirmed
by live capture with A2DP streaming:

| ch | state | `+0x02` (cid) | `+0x04` (PSM) | peer BD_ADDR |
|---|---|---|---|---|
| 0 | 3 (listener) | 0x0005 | 0 | — |
| 1 | 3 (listener) | 0x0004 | 0 | — |
| 2 | 3 (listener) | 0x0006 | 0 | — |
| 3 | 2 (open) | 0x0203 | **0x0019 AVDTP** | xx:xx:xx:xx:xx:xx |
| 4 | 2 (open) | 0x0304 | **0x0017 AVRCP** | xx:xx:xx:xx:xx:xx |
| 5 | 2 (open) | 0x0605 | **0x0019 AVDTP** | xx:xx:xx:xx:xx:xx |

Two AVDTP channels (signalling + transport) and one AVRCP channel, exactly as A2DP requires.
`state 3` entries are listeners registered via `FUN_080f21c0`; `state 2` entries are live
connections created through the routine below.

### The open routine: `FUN_083d8e60` (RAM `0x083d8a60`)

```c
int l2cap_open(void *cb1, void *cb2, void *cb3, uint psm,
               void *bdaddr6, void *cfg, uint16_t *out_cid, void *ctx)
{
    if (!cb1 || !ctx || !cb2 || !cb3 || ((psm & 0x101) != 1) || !out_cid) return 0x65;
    if (FUN_0810be18(cfg) == 0 && FUN_080f2128(2 /*state=open*/, &ccb) == 0) {
        ccb->bdaddr = *(u32*)bdaddr6;  ccb->bdaddr_hi = *(u16*)(bdaddr6+1);
        *(u16*)(ccb + 4) = (u16)psm;            /* <-- the PSM */
        ccb->+0x20 = ctx; ccb->+0x14 = cb3; ccb->+0x7c = cb2; ccb->+0x80 = cb1;
        ...
        *out_cid = *(u16*)(ccb + 2);
    }
}
```

**The argument check `(psm & 0x101) == 1` passes for `0x1001`** (`0x1001 & 0x101 == 0x0001`).
It also passes for AVDTP `0x19` and AVRCP `0x17`, so this is the same path the firmware uses.

Call sites (both AVRCP, from `FUN_080ef880` / `FUN_080ef908`):

```c
l2cap_open(DAT_080ef904, DAT_080ef900, DAT_080ef8fc, 0x1b, peer, DAT_080ef8f8+0x18, &cid, DAT_080ef8f8);
l2cap_open(DAT_080ef9c8, DAT_080ef9c4, DAT_080ef9c0, 0x17, peer, param_3,           &cid, DAT_080ef9bc);
```

The `DAT_` values are literal-pool words; their real contents (read from the image, verified
against a live peek at RAM `0x080ef504`) are Thumb callback pointers `0x080e33a1`, `0x080e34c5`,
`0x080e3531` and context block `0x08782800` — i.e. RAM `0x080e2fa1`, `0x080e30c5`, `0x080e3131`,
`0x08782400`.

### Probe harness

`harness/aap_open.c` — finds the live AirPods link (scans CCBs for state 2 with PSM 0x19/0x17,
copies the BD_ADDR), calls `l2cap_open` with **PSM 0x1001**, and reports rc/cid to the screen and
the trace ring (`AOPENRC`). Drop it into `NanoApps/harnesses/aap_open/` and
`./start run aap_open`. RAM only; reboot undoes it.

### Remaining work after G0

1. Verify the open returns 0 and a cid (device test).
2. Find the data-write path for an open cid (the L2CAP send used by A2DP), to emit the AAP
   handshake and control packets.
3. Replace the borrowed callbacks with our own receive handler that logs/forwards to the app.
4. Build the LVGL app on top (docs in `app/README.md`).

## Transport: SCSI exec is unreliable on this dock — install as an app instead

`C6/98 03` (exec) returns SCSI status `0x04` and the device leaves the bus. `C6/98 01`
(upload) works when sent in **512-byte chunks** with the address advancing per chunk
(a single unaligned transfer returns `realized=0`). Even with eject-then-upload-then-exec
the payload often never starts.

So the probe is built as a normal **RAW_SURFACE NanoApps app** (`apps/aap_open/`, icon "AAP")
and installed over the mounted FAT volume, which is reliable:

```sh
./start build aap_open && ./start install aap_open
```

Then tap the AAP icon on the Home Screen. The app runs `probe()` at init and on every tap,
and draws `rc` / `cid` / channel counts / the peer BD_ADDR it found.

---

# Consolidated findings after the first build (2026-11-23)

Written after Bluetooth audio was lost and recovered. Read this before touching the device.

## What is proven to work on this hardware

| Finding | How it was proven |
|---|---|
| AAP channel on PSM `0x1001` opens from the resident's UI context | live CCB: `state=2 cid=0x0606 psm=0x1001 +0x34=4` |
| The open routine needs the peer's accepted config, not a composed one | copying `+0x40` from a live A2DP CCB gave `rc=0`; composing gave `rc=0x68` + reboots |
| AAP frames can be transmitted through the firmware writer | `FUN_08415ac0(cb, cid, 0, bitstream, 0)`; it frames the PDU itself using remote cid at `+0x2c` |
| AirPods do send notifications to the nano | 39–47 packets observed: `0x0004` battery, `0x0006` ear detection, `0x0009 0x0D` mode, `0x0017`, `0x0008`, `0x001D` metadata |
| Battery bytes decode correctly | component `0x04` level `0x57`/`0x55` → 87%/85%, matching the LibrePods layout |
| Mode changes were audibly observed once | while the AAP build was live, the user heard the noise-control change on tapping |
| The patched firmware alone plays Bluetooth audio | deleting `Apps/`, `Data/`, `Library/` restored audio, with the RTP patch present |

## What broke, exactly

1. **Mailbox outside the report block overwrote loader code.**
   Report = `0x086c9b84`, size `0x200`. The mailbox was `base+0x200 = 0x086c9d84`, which is
   the deferred stage's `action_match_helper` (`CAVE+0x5a0`). Every tap wrote 16 bytes of
   `REPR`/mode/sequence over its first instructions. Live RAM confirmed the corruption; the
   on-disk stage was intact, so a reboot cleared it.
2. **Diagnostic overlap.** RX ring occupied report slots 8..31, status words 16..21. Some
   recorded values were not what they were labelled.
3. **Frame length defect.** Control frames were sent as 12 bytes; the protocol's control
   frame is 11. `0x1A`/`0x1B`/`0x4D` gating was never sent, so Adaptive could not have worked
   and single-bud ANC was never enabled.
4. **Wrong execution context.** Touch hooks opened channels and transmitted. That path is hot
   and stack-tight; BT work belongs in the UI/action context, once per event.
5. **`rc=0` treated as success.** It only means the local stack accepted the buffer. The
   accessory's `09 00 0D` notification is the only real acknowledgement, and we never waited
   for it before declaring progress.
6. **Bluetooth audio was lost while the resident ran, and the cause remains unattributed.**
   This is the reason the new plan starts with a resident-only gate (G1) rather than
   assuming the resident is safe.

## Misreadings we have now corrected

- `0x000C`, `0x000E`, `0x002E` are audio-source / connected-device exchanges, not a mandatory
  encrypted unlock. Noise control, battery and ear detection are plaintext.
- The bitstream descriptor (`FUN_0841097e`) takes **12 bytes per chunk as a descriptor**, not
  a 12-byte payload limit; `FUN_080e0bc4` stores ≤8 payload bytes inline and allocates
  otherwise. The "split 16-byte hello into 12+4" workaround was based on a wrong reading.
- `rrc=65535` seen in one log was a locally invented "waiting" marker, not a firmware code.

## Facts about this specific device setup

- One AirPod only. Single-bud ANC therefore depends on `0x1B OneBudANCMode`.
- Mac↔nano over a DELL DA300 dock. macOS will take the AirPods as its own active output
  whenever they connect to the Mac; that alone produces "nano connected but silent".
  `blueutil --disconnect xx-xx-xx-xx-xx-xx` releases them.
- SCSI peeks unmount the volume and interrupt playback. Do not instrument while the user is
  listening to something that matters.
- Trace ring is evicted by `APPADD` catalog spam on every app launch: use files for durable
  logs, and only overwrite files that already exist.

## App-context L2CAP open: the real crash cause (2026-11-23)

The self-contained LVGL app crashed on the first mode tap. The log stopped after
`open:begin`, i.e. inside the open call. This was **not** an execution-context
limit — it was an argument-size bug:

`FUN_083d8e60(sink, sink, sink, psm, bd, cfgstruct, &cid, ctx)`

- argument 6 is a **config structure**, not the 8-byte L2CAP config:
  it calls `FUN_0810be18(param_6)` and then reads `+0x06`, `+0x07`, `+0x08`,
  `+0x0c`, and **dereferences `*(void**)(param_6 + 0x10)`**.
- the 8-byte config the stack itself accepted (from a live A2DP CCB `+0x40`)
  belongs at the **front** of that structure.
- argument 8 is the channel context.

The rebuilt app passed an 8-byte buffer, so `+0x10` landed in neighbouring `.bss`
(the BD_ADDR bytes) and was dereferenced as a pointer → data abort. The earlier
resident passed a 24-byte zeroed buffer, which is why the open worked there and
appeared to be a "context" problem.

**Rule:** `static uint8_t cfg[24] __attribute__((aligned(8)))`, zeroed, first 8 bytes
copied from the AVDTP entry's `+0x40`, passed as *both* argument 6 and argument 8.

This also means a self-contained app **can** open the channel — no resident
transport is required.

---

# Boot-sequence + connection-event analysis (2026-11-23, offline only)

Written to answer: *if we open PSM 0x1001 once at `payload_entry`, is the stack
ready?* Grounded entirely in the decompiled corpus (`build/decompiled-all.c`)
and the resident source at `system/silver_resident/` of the NanoApps checkout.
No device activity.

## BT stack init runs in BootTask, strictly before the resident loads

Verified call chain (addresses are RAM):

```
FUN_080fa248   "OI_StackWrapper_Init_Custom"   decomp line 214406
  └─ FUN_080f1da8  "BT CoreStack"               line 207703
      └─ FUN_084e6bfc                           line 889502
          └─ FUN_080f7b40  L2CAP init          line 212301
              └─ allocates CCB table (24 × 0x88 B) at *(void**)0x080f2058
```

The wrapper logs the strings `"OI_StackWrapper_Init_Custom"`,
`"BT CoreStack"`, `"Initializi…"` and `"Stack Init failed"` — all present in
the image. `FUN_084e6bfc` sequences Policy Mgr → Security Mgr → L2CAP → LE-GAP
→ SDP, i.e. L2CAP is up before any profile layer runs.

The NanoApps resident's `payload_entry` (`silver_resident.c:1614`) hooks the
HOME-screen vtable (`HB_SILVER_HOME_VTABLE`), which only exists after
MainAppTask has instantiated the UI — this is strictly after BootTask finishes
BT initialisation. Therefore:

> **At `payload_entry`, `*(void**)0x080f2058` is a valid pointer to a 10-link /
> 24-channel L2CAP state block with the listener slots (id 5, id 6) already in
> state 3.**

Live evidence consistent with this: previous CCB captures at idle have shown
the state block at `0x0895c080` with `channels=24, ccb_array=0x08d02fe4` and no
AVDTP/AVRCP slots in state 2 yet.

## What is NOT ready at `payload_entry`

The AirPods have not reconnected. There is no AVDTP CCB in state 2, no
`+0x2e` BD_ADDR to copy, no `+0x40` accepted config. `aap_transport.c:118`'s
`find_chan(AVDTP_PSM, 1)` returns NULL at that point, so a boot-time open from
`payload_entry` *as literally described in STATUS.md* cannot succeed — it has
no peer to open to.

**Consequence:** the "open once at boot, before any apps run" plan does not
work as stated. We need an event — "AVDTP has just reached state 2 to the
AirPods, media not yet streaming" — not a point in time.

## Candidate hook: `FUN_080e8780` / `FUN_080e8884` connection-event dispatcher

`FUN_080e8780(cb)` (RAM `0x080e8780`, decomp line 201116) appends `cb` to a
callback array at `DAT_080e87c4` (max 8 entries), guarded by a count at
`DAT_080e87c0 + 6`. Two existing registrants:

- **AVRCP profile init** (`FUN_080eb4b0`, line 202902) — after it registers its
  L2CAP listener (`FUN_080f21c0(6, …)` → id 6, MTU 0x17), it calls
  `FUN_080e8780(DAT_080eb570)` to subscribe to connection events.
- **`DAT_08439a60`** (line 801218) — a second subscriber registered deeper in
  the stack (near the link-key path, line ~801218).

The dispatcher `FUN_080e8884(peer_hdr, ccb_or_null, status)` (line 201146) is
invoked from `FUN_080e8388`:
- Line 200945 `FUN_080e8884(param_4, puVar3, 0)` — success path:
  `puVar3 = FUN_080f74c4(param_4)` looks up the CCB by peer BD_ADDR, then
  fires the dispatcher with `status=0`.
- Line 200950 `FUN_080e8884(param_4, 0, param_1)` — failure path: null CCB,
  non-zero status.

Dispatcher body iterates the listener array and calls each `cb(ccb, status)`.
This is *a* connection-event hook — but the corpus alone does not fully settle
whether it fires at **L2CAP channel open complete** or at **ACL link
established**. `FUN_080e8388` processes a header with `*param_4 == 1` and
`param_4[1] >> 6 == 1`, which pattern-matches HCI/L2CAP connection-command
parsing. The successful-path CCB lookup is by BD_ADDR (not by cid), which is
weak evidence it fires per *link*, not per *channel* — but it may fire
multiple times as separate channels (AVDTP signalling, AVDTP media, AVRCP)
reach state 2 under the same link. **Live verification needed before trusting
this.**

## The failure table (STATUS.md) reinterpreted

| # | Context | Outcome | What was actually wrong |
|---|---|---|---|
| 1 | resident dispatch, link idle | **success**, notifications flowed | the only truly quiescent case |
| 2 | app code, 8-byte cfg | reset | 8-byte cfg bug (data abort at `+0x10` deref) — orthogonal |
| 3 | app code, 24-byte cfg | reset after rc=0 | app-arena lifetime issue (sink lived in unloadable arena) |
| 4 | resident surface timer | reset after ~2 s | wrong task context |
| 5 | dispatch while navigating to Music | reset | **A2DP being torn up/down** — not a quiescent link |
| 6 | dispatch with `want_open` pending | reset | re-entry: two opens on one link |

Only cases 5 and 6 are relevant evidence against "open while AVDTP is up but
media is stopped". Case 5 was not that scenario; case 6 is a re-entry defect.
**There is zero evidence against opening once per boot, exactly when AVDTP
reaches state 2, with media_state==2, and never retrying.** That is the gap
the next device test should probe.

## Verdict for Step 1

1. **"Open from `payload_entry`" as literally stated is unworkable** — no
   peer exists yet.
2. **"Open once at the right event" is well-supported by the corpus**, with
   `FUN_080e8780`/`FUN_080e8884` as the strongest candidate hook; its exact
   trigger semantics need one live confirmation but no risky write.
3. **The existing `aap_transport.c` poll path is salvageable** if its "up to
   4 opens" allowance is reduced to exactly one per boot *and* a stopped-media
   precondition is added. On the current evidence, the quiescent-link open is
   the only case that has ever worked and has not been disproven.

## Proposed next sub-steps (no device flashing)

1. **Decide on approach A (event hook) vs B (gated poll)**. A is cleaner; B is
   smaller and reuses working code. Recommend B first — fewer unknowns.
2. **Edit `aap_transport.c` only**: change `s_aap.opens < 4` to `< 1`; add a
   gate that rejects the open unless media is stopped. No new hooks, no new
   addresses. Diff before build; build before install; verify install landed.
3. **Separate, offline**: identify a firmware word that reports media_state so
   the gate has something to read. Previously observed as `media_state == 2`
   means stopped — need the address. `grep` for A2DP state constants and the
   stream-start/stop callers.
4. Only *after* 1–3 are concluded on paper, schedule one device test: boot
   with safe build, verify audio, then (user initiates) connect AirPods and
   watch for one `open rc=0 cid=<x>` log line. No mode command on this gate.

## Facts that still need live confirmation (none required for the writeup above)

- Address of the A2DP media-state variable.
- Whether `FUN_080e8884` fires per L2CAP state=2 transition or only once per
  ACL link.
- The actual number of listeners currently registered at
  `DAT_080e87c0 + 6` on this device (sanity: should be at least 2).

## Link drops during AAP sessions (2026-11-23, project paused)

Once the handshake worked (HELLO, SUB, FEATURES sent from the resident), the AirPods
streamed notifications and battery decoded correctly. The link then dropped
15–73 s after the channel opened, with no fixed timeout, and the music stopped.

Suspected cause (the user's): the Mac and iPhone paired with the same AirPods were
asking them to reconnect, so the AirPods switched away from the nano. Not verified.
First retest: Bluetooth off on the other devices before changing code.

State: the device runs the stock resident (only `bluetooth_fix.c`). The AAP code,
the app and a diff are in `build/nanoapps-aap-backup/`.
