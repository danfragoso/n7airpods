# Status — rebuild in progress

Hardware: iPod nano 7G A1446, firmware 1.1.2 / 39A10023 (patched IPSW with the
Bluetooth RTP fix), NanoApps resident and apps as shipped. One AirPod only.

## Current state (2026-11-23, after the incident)

- **Working baseline:** the patched firmware with its stock `Apps/`, `Data/`,
  `Library/` plays Bluetooth audio. Backed up in full to `build/working-backup/`
  (19 MB, 164 files, resident sha256 `22a4772a…234fb0`). That directory is the
  restore point; the earlier "delete everything" rollback is no longer needed.
- **The resident was not the fault.** Audio works with the shipped resident
  installed, so the damage came from the abandoned AAP build (loader mailbox at
  report+0x200 overwriting `action_match_helper`, 12-byte control frames, BT work
  in the touch hook), not from NanoApps itself.
- **New app installed: `hb.aap_ctl` ("AirPods")**, 274848 bytes, LVGL surface
  (`LV_SURFACE := 1`) so it renders through the framework instead of drawing over
  the framebuffer. Self-contained: no resident change, no shared mailbox, all
  state in its own `.bss`. The stock resident is byte-identical (verified).

## What the new app does

One step per LVGL frame tick (400 ms apart), each logged to `/Data/AAP/aap.txt`
**before** the call, so a crash still names the step that caused it:

```
open → hello → subscribe → features(0x4D) → allowed modes(0x1A=0x0F)
     → one-bud ANC(0x1B=0x01) → chime(0x1F=0x3C) → ready
```

Then a mode tap sends `04 00 04 00 09 00 0D <mode> 00 00 00` (11 bytes). The UI
only claims a mode after the AirPods' own `09 00 0D` notification arrives.
Touch handlers only record intent; all Bluetooth work happens in the frame
callback (UI task), never in the touch path.

Byte-level contract enforced by `harness/test_packets.c` (`-Wall -Wextra -Werror`).

## Architecture that shipped (v2, resident-owns-transport)

Two crashes at the same operation settled the division of labour:

| Attempt | Result |
|---|---|
| app opens channel (8-byte cfg) | crash before the call returned |
| app opens channel (24-byte cfg struct) | `rc=0`, cid `0x706`, then the device rebooted |
| resident opens channel (dispatch hook) | `rc=0`, channel stays open, notifications arrive |

The L2CAP control block keeps pointers to the receive callback and its buffers, and
app images are unloaded with their heap arena. So:

- `system/silver_resident/aap_transport.c` — owns the channel, its sink, and the
  notification decoding. One bounded open per link (max 4 attempts), called from the
  dispatch hook only. State published via `hb_shared` (`aap_magic` + `aap_state`).
- `apps/aap_ctl/` — LVGL app: all UI, the protocol sequence, and logging. Sends the
  frames itself through the firmware writer, using the resident's cid.

Resident version live: `1795433097`, 23116 bytes, `failmask 0`. Stock resident is
preserved at `build/working-backup/Library/resident.hbapp`.

## Next gate

Tap the AirPods icon, then a mode chip, and read `/Data/AAP/aap.txt`:

- `open:reuse` / `open:end a=0` ⇒ channel up
- `hello:end a=0`, `sub:end a=0`, … ⇒ frames accepted locally
- `rx` counter rising and `mode:begin` followed by an echoing `09 00 0D` ⇒
  the AirPods accepted the request (the only real proof)
- `open:no-avdtp` ⇒ AirPods not connected to the nano

See `docs/PLAN.md` for the gate ladder and rollback, `docs/PROTOCOL.md` for the
corrected frames.

## Historical detail (kept for the record)

The sections below document the failed first attempt, the defects found, and the
rollback forensics. They are the reason every rule in `docs/PLAN.md` exists.


## Actual milestone

**Transport stability is not proven. Noise-control operation is not proven.**
A PSM 0x1001 channel has opened and received packets, but tapping the app has
repeatedly disrupted Bluetooth. A zero firmware return code is not evidence
that the peer accepted a handshake or changed mode.

## Verified defects in the previous experiment

1. **Out-of-bounds mailbox overwrote loader instructions.**
   Report base = `0x086c9b84`, report size = `0x200` bytes.
   The old mailbox started at `base + 0x200 = 0x086c9d84` and wrote 16 bytes.
   The deferred stage's executable `action_match_helper` starts at exactly that
   address (`CAVE + 0x5a0`, CAVE = `0x086c97e4`). The assembly and linker layout
   explicitly place code after the report. Live RAM contains `REPR`, mode and
   sequence over its first instructions, followed by the remaining Thumb code.
   This is proven corruption, though not proven to be the sole disconnect cause.
2. **Status and receive-ring records overlapped.**
   `aap_report(8..31)` held eight RX entries while mode/subscription/request
   diagnostics wrote `aap_report(16..21)`. Earlier diagnostic values cannot be
   trusted as independent records.
3. **Bitstream interpretation was wrong.**
   `FUN_0841097e` appends a 12-byte *descriptor*, not a payload limited to
   12 bytes. `FUN_080e0bc4` stores up to 8 payload bytes inline and allocates
   backing storage for larger payloads. Splitting a 16-byte handshake 12+4
   was not established as necessary and did not prove a valid handshake.
4. The resident mode command was 12 bytes; the tested protocol builder emits
   **11**. Do not reuse the resident's old literal packet.
5. Touch hooks performed opens/transmits, selected stale AVDTP slots without
   checking allocation state, and could issue several opens while one was still
   pending. Moving work into a resident does not prove its execution context safe.

## Most recent evidence before rollback

Saved locally in `build/disconnect-evidence/`:

- `trace.txt`: three AAPMOD attempts followed by requests with no corresponding
  send; confirms input/mailbox activity, not successful AirPod control.
- `result.txt`: `request cid=2819 mode=1 ... rmode=1 rrc=65535 runs=7`.
  `65535` is the previous build's locally invented waiting marker, not a
  firmware return code.
- Live channel snapshot: AAP CID `0x0b03`, state 2, ready 4; AVDTP slot was
  state 0. This snapshot cannot reconstruct the precise disconnect timeline.
- Raw report and channel bytes plus the unsafe source snapshots retained for
  analysis. Received opcode 0x0009 alone is not a mode confirmation: payload
  must identify control 0x0d and the requested mode.

## Safety rollback

The replacement app and resident deliberately make **zero AAP Bluetooth calls**.
They do not claim to change ANC/Transparency/Off/Adaptive.

- Resident owns `hb_aap_state_t` in its allocation.
- App obtains it through a self-identifying extension to `hb_shared_t`.
- No AAP writes to firmware/report addresses.
- No AAP work in the touch hook; the existing UI timer observes requests using
  memory operations only.
- UI explicitly says "Bluetooth writes OFF" and "Modes are NOT sent".
- No writes to the report's reset scratch, so future boots do not reserve it
  accidentally for AAP diagnostics.

Host tests compile the actual resident implementation with AddressSanitizer and
UndefinedBehaviorSanitizer; cover 100 requests, invalid modes, duplicate polls,
sequence wraparound, null shared state and memory canaries. Both ARM artifacts
build. Static checks exclude old firmware addresses and touch-hook polling.
These checks do **not** establish on-device stability.

## Follow-up: apparent sound changes but no songs

Live inspection found the old resident still active (version `1795405907`),
old AAPCTL/AAPREQ/AAPMOD traces, an open AAP control channel, and inactive
AVDTP/AVRCP entries. The files on disk had also reverted to the old artifacts;
the earlier immediate readback did not prove persistence across USB handoff.
The cause of that reversion has not been established.

Reinstalled through the normal `start install aap_open` and
`start install --resident-only` paths. Automatic restart still stopped on
occupied scratch, but the standard filesystem handoff completed. Remounted
and verified both artifacts byte-for-byte **after** that handoff, then ejected
successfully. Persisted safety resident version is `1795406796` (22656 bytes),
app 8820 bytes. Receipt: `build/disconnect-evidence/safe-install-handoff.json`.
A real reboot remains required; screen sleep or app relaunch does not replace
the running resident. Re-establish audible song playback before testing AAP.
The new UI must say `AAP safe diagnostics` / `Bluetooth writes OFF`.

## Follow-up: silent song playback after reboot

The subsequent live inspection confirms safety resident `1795406796` is now
running. Its resident-owned state has the expected `AAP2` magic, ABI 1, size
36, and Bluetooth calls disabled. The loader matcher starts with instructions
again, not the old mailbox. The Bluetooth RTP timestamp patch is present and
its application result is 1. The channel-open entry is stock/unhooked.

At the snapshot, every previously used AVDTP (0x0019) and AVRCP (0x0017) slot
has allocation state 0 and ready 0. No AAP channel is allocated. Thus there
was no open stereo-audio channel at inspection; the song's player UI alone
is not proof of an audio connection. The Mac lists this AirPod as not connected.
These observations do not identify why the connection closed. Next recovery
step is a normal Bluetooth reconnect from nano Settings, without opening AAP,
while ruling out iPhone auto-connection. No further AAP build is needed for that.
Raw report, CCB and trace saved as `no-playback-*` in the evidence directory.

## Rollback completed and verified

Restored the **exact original pre-AAP resident** from the first backup:
`build/install-backups/20261123-000510-.../Library/resident.hbapp`, version
`1794120442`, 22672 bytes, sha256 `22a4772a…234fb0`. Retired `hb.aap_open`.

Verified after the standard filesystem handoff, then a fresh boot was
confirmed by the restart routine:

| Check | Result |
|---|---|
| resident version live | `1794120442` (original) |
| apps registered | 45 (AAP removed) |
| loader `action_match_helper` | real instructions, mailbox garbage gone |
| BT RTP timestamp patch | live at `0x082b2bd6` |
| dock fix | applied (1) |
| L2CAP open entry | stock, unhooked |

Receipt: `build/disconnect-evidence/original-rollback.json`. Music, other apps
and Bluetooth pairing data were not touched.

**Playback was not audible at the last gate and is not yet re-confirmed.** The
next test is plain AirPods connection + song playback on this restored stock
resident, with no NanoApps AAP code involved at all.

## Next gates, in order

1. **Safety baseline:** after a power-cycle restores loader code, music remains
   connected while opening the safe app and tapping; requested/observed sequence
   counters advance. ANC mode is deliberately unchanged.
2. **Open only:** reintroduce one channel-open from a verified safe execution
   context with correctly typed callbacks; no TX, no duplicate pending opens,
   no use of stale AVDTP entries. Capture disconnect events/reasons.
3. **Handshake only:** verify bitstream ownership/lifetime against stock callers,
   then a single handshake. Separate local return codes from peer responses.
4. **Subscribe, then one 11-byte mode command:** validate received header and
   control identifier 0x0d, and correlate requested/reported mode. Account for
   single-bud ANC settings; no assumption that ANC is enabled with one bud.
5. Ten-minute playback/reconnect stability gate, then battery/ear UI and auto-pause.

Do not ask for another combined open+handshake+mode test before these gates.

---

# Verdict (2026-11-23, end of session): AAP channel open is not stable on this firmware

## Where the device is now

- Resident restored to **stock** (version `1794120442`, sha256 `22a4772a…234fb0`,
  byte-identical to `build/working-backup/Library/resident.hbapp`).
- `hb.aap_ctl` removed; catalog back to 45 apps. Music, data and pairing untouched.
- Bluetooth RTP fix still applied (`0x082b2bd6` patched), as in the working baseline.

## What is proven to work

| Fact | Evidence |
|---|---|
| AAP runs on PSM `0x1001` and the nano can reach it | channel reached `state=2`, `+0x34=4` |
| Frames can be built and transmitted by the firmware writer | `rc=0` on send |
| The AirPods do talk back | 39–47 notifications: `0x0004` battery (85–87% matched the bud), `0x0006` ear detection, `0x0009 0x0D` mode, `0x001D` metadata |
| A mode change was audibly observed once | user report while the AAP build was live |
| The four missing commands that gate ANC/Adaptive | `0x4D` features, `0x1A` mode mask, `0x1B` one-bud ANC, `0x1F` chime (LibrePods) |
| App-context open is wrong; so is surface-timer context | crash at `open:begin`, and again after `open:end rc=0 cid=0x706` |
| Config argument must be a >=0x18-byte zeroed struct | data abort from an 8-byte buffer |

## What blocks it

Opening the channel succeeded **once** out of six attempts. The other five reset
the device, clustered on dispatches that coincided with Music app navigation or
playback:

| # | Context | Outcome |
|---|---|---|
| 1 | resident dispatch, link idle | `rc=0`, channel stayed open, notifications flowed |
| 2 | app code, 8-byte cfg | reset inside the call |
| 3 | app code, 24-byte cfg | reset immediately after `rc=0` |
| 4 | resident surface timer | reset after ~2 s of "Connecting…" |
| 5 | dispatch while navigating to Music | reset |
| 6 | dispatch with `want_open` pending | reset |

**Conclusion:** this firmware's Bluetooth stack cannot reliably host a second
L2CAP channel to the AirPods while A2DP is active or being set up. The one success
happened on a quiescent link. This is a stability limit of the stack, not of the
AAP frames — the frames were verified correct and the AirPods answered.

## Untried, in order of promise

1. **Open once, at boot, before any app runs** — from the resident's
   `payload_entry` while the BT stack is quiet, and never again. Needs proof the
   stack is initialised that early.
2. **Open once after a settle delay on a stopped link** (`media_state == 2`), no
   dispatch churn, then never retry that boot. Our retries may themselves have
   caused resets.
3. **Abandon AAP control** and ship only what AVRCP already gives us (transport,
   volume, track info). No ANC control, but zero stability risk.

## Process lessons worth keeping

- `hb_fs_write` is **not crash-durable**: cached FAT writes are lost on reset. Use
  the RAM trace ring for anything that must survive a crash.
- SCSI peeks unmount the volume, which silently breaks app file logging.
- Verify the install landed (version + bytes) *before* asking for a device test.
  Two of the last tests ran against a build I had already identified as unsafe.
