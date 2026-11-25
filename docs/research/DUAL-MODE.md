# "Dual mode": what parity with iOS/macOS means here

You asked whether we can make the nano behave like a modern Apple device. Split that into
what it actually does, and what we can reach.

## How a modern Apple device talks to AirPods Pro 2

| Link | Transport | Carries |
|---|---|---|
| Audio | Classic BR/EDR — A2DP/AVDTP, SBC (or AAC) | the music |
| Transport control | Classic — AVRCP | play/pause/skip, volume |
| **Control plane** | **Classic L2CAP, PSM 0x1001 (AAP)** | noise modes, battery, ear detection, conversation awareness, renames |
| Proximity / Find My / OTA | BLE (GATT + Apple's auth) | pairing magic, firmware updates |
| LE Audio (Pro 2 only) | BLE Isochronous, LC3 | lower latency/lossless-ish audio |

The nano already has the first two — A2DP and AVRCP work (once the RTP timestamp patch is on).
**AAP is the missing third link**, and it's the one that unlocks the whole control surface.
So "dual mode" for our purposes really means: *add the control plane.*

## Achievable, in order of confidence

1. **Noise control modes** — ANC / Transparency / Off, plus Adaptive if not gated. One 11-byte
   packet. This is the headline.
2. **Battery per bud + case** — comes free once subscribed (`0x04` notifications). The nano's own
   battery icon is a fixed glyph; we'd draw ours.
3. **Ear detection → auto-pause** — subscribe to `0x06`; when both buds leave, pause the stock
   music player (its transport is already drivable — NanoApps' Remote app proves the API).
   This is the feature that makes it feel modern rather than like a novelty.
4. **Current mode display** — mirror stem presses in the UI, since the AirPods notify state
   changes regardless of who caused them.
5. **Conversational awareness state / adaptive audio** — likely needs the `4D 00 FF` unlock
   packet; plausible but unverified on this host.

## Realistically out of reach

- **LE Audio / LC3.** The nano's BT 4.0 stack predates LE Audio entirely. No amount of patching
  makes a 2012 Broadcom stack speak ISO channels.
- **Firmware updates for the AirPods.** Apple-signed, attestation-gated. Nothing we do changes that.
- **Find My / proximity pairing UX**, **head tracking / spatial audio** — both need Apple's
  attestation chain or a motion pipeline that doesn't exist here.
- **True multipoint.** AirPods handle multipoint themselves; the nano can be one of the two hosts,
  but the nano can't *manage* the other. Nothing to write.

## So: yes, with one honest asterisk

If G1 passes, the nano ends up with the same control plane a Mac has, minus the Apple-cloud
features — i.e. it will genuinely feel like a modern host: your buds tell the nano their battery,
you switch ANC from a touchscreen, music pauses when you pull a bud out.

The asterisk is that all of it rides on one unproven link: opening PSM 0x1001 from BLUEmagic.
Every other piece is ordinary app work on top of that.
