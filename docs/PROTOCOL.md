# AAP on AirPods Pro 2 (USB-C, firmware 7A305)

Protocol facts below are transcribed from [LibrePods](https://github.com/kavishdevar/librepods)
`docs/AAP Definitions.md`, `docs/control_commands.md`, `docs/opcodes.md` — captured by kavishdevar
with PacketLogger on macOS, cross-checked against their working Android and Linux implementations.
Facts about a wire protocol; no code copied.

## Transport

**Classic Bluetooth L2CAP, PSM `0x1001` (4097)**, secured + authenticated.
Confirmed by LibrePods Android `BluetoothConnectionManager.kt`:

```kotlin
fun createBluetoothSocket(adapter, device, uuid: ParcelUuid, psm: Int): BluetoothSocket {
    val type = 3 // L2CAP
    // ...reflects a BluetoothSocket ctor with (…, psm, uuid)
}
// called as: createBluetoothSocket(adapter, device, uuid("74ec2172-…"), 4097)
```

SDP service UUID the AirPods advertise: `74ec2172-0bad-4d01-8f77-997b2be0722a`.
That UUID is how a host *finds* the service; the actual channel is L2CAP PSM 0x1001.

> **This is the whole feasibility question.** If we cannot open PSM 0x1001 from the nano's
> BLUEmagic stack, there is no AAP, and no ANC/battery/ear-detection app. See [research/FEASIBILITY.md](research/FEASIBILITY.md).

## Framing

```
04 00 04 00 | [opcode LE16] | [payload…]
└─ header ─┘
```

The 4-byte header `04 00 04 00` prefixes everything, in both directions.

**Control commands are fixed at 11 bytes:** 7 bytes after the header
(`identifier, data1, data2, data3, data4`). Unused bytes are zero. Sending a longer frame
desyncs the accessory's parse of the following frame — this defect existed in the first
attempt (it sent 12) and is now enforced by `harness/test_packets.c`.

## Handshake (mandatory — otherwise the AirPods ignore you)

```
00 00 04 00 01 00 02 00 00 00 00 00 00 00 00 00
```

## Subscribe to notifications

```
04 00 04 00 0F 00 FF FF FF FF      # or FF FF FE FF
```

After this we receive battery, ear detection, and noise-control state changes unsolicited.

## The three packets that matter

**Set noise control mode** — opcode `0x0009` (control command), identifier `0x0D`:

```
04 00 04 00 09 00 0D [mode] 00 00 00
```

| mode | meaning |
|---|---|
| `01` | Off |
| `02` | Noise Cancellation |
| `03` | Transparency |
| `04` | Adaptive Transparency |

**Noise control notification** (accessory → host) — same frame shape, so parsing is symmetric.

**Battery report** — opcode `0x0004`:

```
04 00 04 00 04 00 [count] ( [component] 01 [level] [status] 01 ) × count
component: 08 case · 04 left · 02 right
status:    00 unknown · 01 charging · 02 discharging · 04 disconnected
```

**Ear detection** — opcode `0x0006`:

```
04 00 04 00 06 00 [primary] [secondary]
status: 00 in ear · 01 out of ear · 02 in case
```

## Feature gating

Some features are gated behind Apple OS version checks. LibrePods captured this packet
(Intel Mac, macOS 15.0.1) which enables conversational awareness during playback and
Adaptive Transparency:

```
04 00 04 00 4D 00 FF 00 00 00 00 00 00 00
```

Noise control and battery are **not** gated — they work on Android/Linux without it.

> If Adaptive Transparency is *not* enabled first, the AirPods answer a request for mode 4
> with a different mode (commonly `0x02`). A “mode ignored” symptom usually means this
> packet was missing, not that the frame was wrong.

## Configuration commands that decide what noise control can do

All are opcode `0x0009`, 11 bytes. Identifiers from LibrePods `control_commands.md`.

| Identifier | Meaning | Values | Why it matters here |
|---|---|---|---|
| `0x1A` | ListeningModeConfigs | bitmask: Off `01`, ANC `02`, Trans `04`, Adaptive `08` | modes not in the mask are refused; `0x0F` enables all four |
| `0x1B` | OneBudANCMode | `01` enabled, `02` disabled | **a single AirPod cannot do ANC unless enabled** |
| `0x1F` | Chime Volume | 0..100 | the mode-switch chime volume |
| `0x0A` | Ear Detection | `01` enabled, `02` disabled | gate for the auto-pause feature |
| `0x28` | Conversational Awareness | `01` on, `02` off | also sent once by the AirPods after subscribe |
| `0x2E` | AutoANC Strength | 0..100 | only meaningful in Adaptive |

### Notifications worth decoding

| Opcode | Frame | Meaning |
|---|---|---|
| `0x0004` | `04 00 04 00 04 00 [n] ([comp] 01 [lvl] [stat] 01)×n` | battery |
| `0x0006` | `04 00 04 00 06 00 [primary] [secondary]` | ear detection |
| `0x0009` `0x0D` | `04 00 04 00 09 00 0D [mode] 00 00 00` | noise control changed |
| `0x0009` `0x28` | `04 00 04 00 09 00 28 [status] 00 00 00` | conversational awareness state, sent once after subscribe |
| `0x001D` | null-terminated strings | name, model, firmware, serials |
| `0x004B` | `04 00 04 00 4B 00 02 00 01 [level]` | CA speaking event (level 01/02 reduce volume, 03 restore) |

### Opcodes seen on this device that are *not* control

`0x000C`, `0x000E`, `0x002E` are the audio-source and connected-devices exchanges and carry
often-encrypted payloads. They are **not** required for noise control, battery or ear
detection; the first attempt mistook them for a mandatory unlock step. Ignore them.

## Out of scope

- Firmware updates (Apple-signed OTA; no keys, no path).
- Find My / proximity keys (needs Apple's attestation chain for the key exchange).
- Head tracking / spatial audio (needs the motion pipeline and, for head-tracked output,
  Apple's own renderer).
