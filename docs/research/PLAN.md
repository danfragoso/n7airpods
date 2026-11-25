# Rebuild plan — AAP noise control on the iPod nano 7G

Written after the 2026-11-23 incident. The previous attempt corrupted the loader,
duplicated work between app and resident, and left Bluetooth audio silent. This plan
replaces guesswork with **gates**: each gate has one question, one artifact, and one
rollback that is known to work.

## 0. The one thing we now know for certain

Deleting `Apps/`, `Data/` and `Library/` (i.e. **no NanoApps resident and no apps**) on the
patched firmware restores working Bluetooth audio. That is the proven-good baseline, and it
is also the rollback for every gate below. Anything we add must be removable by deleting
those directories.

Corollary: we cannot assume the resident is harmless. The resident ran while audio was
silent. Whether the resident, its boot-time patching, or the broken AAP hooks were
responsible is unresolved — so the first gate re-establishes the resident *alone*.

## 1. Invariants (violating any of these is what broke the device)

1. **Never write outside memory you own.**
   The boot report is 512 bytes at `0x086c9b84`. The deferred stage's executable
   `action_match_helper` begins at `0x086c9d84` = report + 0x200. The old "mailbox" sat
   exactly there and overwrote it on every tap. Any diagnostic block must live in the
   resident's own allocation, published via a magic-checked pointer in `hb_shared_t`.
2. **Keep diagnostic records from overlapping.**
   The old receive ring used report slots 8..31 while status words used 16..21 — earlier
   evidence was partly garbage. One field, one offset.
3. **Control frames are exactly 11 bytes.** `04 00 04 00 | 09 00 | id | data1 | 00 00 00`.
   The abandoned build sent 12. Extra bytes desync the accessory's parse of the *next* frame.
4. **No AAP work in the touch hook.** Touch runs on a hot path with a tight stack; the
   abandoned build opened channels and transmitted from there. Do BT work only from the
   UI/action context, once per event, behind a re-entrancy guard.
5. **A return code is not an acknowledgement.** `rc=0` only means the local stack accepted a
   buffer. Truth is the accessory's notification (mode `09 00 0D …`) arriving in our sink.
6. **One link-changing action per gate.** Open, then observe. Do not open + handshake +
   subscribe + set mode in one pass; that made failures unattributable.
7. **Bluetooth audio must survive every gate.** If music stops, the gate failed regardless of
   what the protocol did.

## 2. Required reading for the implementation

- `docs/PROTOCOL.md` — frames, lengths, and the identifiers we need (authoritative, LibrePods).
- `harness/aap_packets.h` — builders/parsers, unit-tested (`harness/test_packets.c`).
- `docs/FINDINGS.md` — what was verified on this device, including the L2CAP ABI facts.

## 3. Gates

Each gate: build → install → observe → record → rollback if it fails.

### G0 — Baseline (proven 2026-11-23)

- State: patched firmware, **no** `Apps/`, `Data/`, `Library/`.
- Pass: Bluetooth audio plays.
- This is the reference. Re-run it any time something looks damaged.

### G1 — Resident only, no AAP code, no boot patching

- Install the stock NanoApps resident with `aap_hook` **removed from `SRCS`** and the
  Bluetooth/dock fix calls **disabled** for this gate.
- Pass: audio still plays after a reboot, resident reports itself live, apps launch.
- Question answered: **does the resident alone disturb audio?**
- If it fails: the resident/hook set is the culprit, and AAP work stops until that is
  understood. Do not proceed to G2.

### G2 — Resident with its normal fixes (as shipped)

- Re-enable `hb_bluetooth_fix()` / `hb_dock_fix()`.
- Pass: audio still plays. If audio breaks here, the boot-time Bluetooth patching is
  incompatible with the patched firmware (which may already contain the RTP fix), and the
  resident must ship with those fixes disabled on this firmware.
- Question answered: **is the resident's own patching safe on patched firmware?**

### G3 — Open the AAP channel, transmit nothing

- Resident finds the A2DP entry, copies peer address and the accepted config, calls the
  L2CAP open once, and records `rc`/`cid` in its own state. **No** send, **no** sink traffic.
- Pass: `rc = 0`, a cid, audio unaffected, and the channel visible in the CCB table.
- Rollback: reboot.

### G4 — Handshake, once per channel

- Send `00 00 04 00 01 00 02 00 00×8` (16 bytes) through the firmware's data write.
- Pass: audio unaffected **and** the first notifications arrive (the AirPods start talking).
- Note: 16 bytes exceeds the 8-byte inline payload of the bitstream descriptor
  (`FUN_080e0bc4` allocates for larger payloads). Verify the descriptor's ownership and
  lifetime against a stock caller before trusting it; do not chunk blindly.
- Fail mode to expect: nothing arrives → handshake never left, or the sink is not attached.

### G5 — Subscribe, then read notifications

- Send `04 00 04 00 0F 00 FF FF FF FF`.
- Pass: sink receives `0x04` battery, `0x09 0x0D` mode, `0x06` ear detection, `0x1D` metadata.
- Record every opcode with its full payload in a fixed 16-entry table, each entry in its own
  reserved report slots (no overlap with status words).

### G6 — Configuration (why ANC/Adaptive may have been refused)

Send, in order, waiting for the echo/next notification between each:

1. `0x4D FF 00 00 00 00 00 00 00` — set features. **Without this, Adaptive degrades to
   another mode.** 14 bytes.
2. `09 00 1A <mask>` — allow the modes in the cycle. Off `0x01`, ANC `0x02`, Trans `0x04`,
   Adaptive `0x08`; mask `0x0F` enables all four.
3. `09 00 1B 01` — **OneBudANCMode**. With a single AirPod, ANC is refused unless enabled.
4. `09 00 1F <0..100>` — chime volume; this is the missing mode-switch chime.

- Pass: each command produces a notification or an observable state change.

### G7 — Set the mode, verified by notification

- `04 00 04 00 09 00 0D <mode>` (11 bytes; Off 1, ANC 2, Transparency 3, Adaptive 4).
- Pass: the sink receives `09 00 0D <mode>` back, **and** audio keeps playing, **and** ANC is
  audible.
- If we get a `09 00 0D <different mode>` back, the AirPods refused the request: go back to
  G6 — usually `0x1A`/`0x1B`/`0x4D` was not applied.

### G8 — UI

Only after G7: mode chips, battery from `0x04`, ear detection from `0x06`, and auto-pause on
ear-out. See `app/README.md`.

## 4. Instrumentation rules (learned the hard way)

- **Files, not the trace ring**, for durable logs: `APPADD` catalog spam evicts the ring on
  every launch. Overwriting an *existing* file works; creating a new name silently failed
  once — create the file inside a directory that already exists and verify with a read-back.
- **One write per event**, so a crash leaves the last line on disk describing what was about
  to happen. Log *before* the risky call.
- Keep the on-device counters small and monotonic (open rc, cid, sent count, rx count, last
  rx opcode + first 8 payload bytes).

## 5. What must be verified before claiming success

| Claim | Proof required |
|---|---|
| Channel open | `rc = 0`, cid present, CCB state 2, audio unaffected |
| Handshake accepted | notifications from the AirPods start arriving |
| Subscribe worked | battery/ear/mode notifications decoded |
| Config applied | `1A`/`1B`/`4D` echoed or reflected in later notifications |
| Mode changed | `09 00 0D <requested>` received back, ANC audible, music uninterrupted |
| Stable | 10 min of playback plus one reconnect with the channel alive |

## 6. Efficiency notes

- Build the app UI only after G7; protocol work needs no UI beyond one tap target.
- Use `harness/test_packets.c` as the byte-level gate for every frame change
  (`cc -Wall -Werror harness/test_packets.c`); it already covers lengths and the parsers.
- Keep the resident's AAP code in one file with no fixed-address constants; the only allowed
  fixed addresses are the firmware entry points we must call, each named and documented.
