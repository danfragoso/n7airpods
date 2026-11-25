# How it works

n7airpods adds two things to a NanoApps iPod nano: an **app** (the page you see) and a set of **resident
modules** (the Bluetooth work). They share one small, versioned block of memory. The reasons for this split are
the most important thing to understand about the project, because the obvious design (the app talks Bluetooth
itself) crashed the device.

```
 AirPods ──── classic BT (A2DP audio, AAP control) ───┐
         ──── BLE advertisements (case, lid, ears) ───┤
                                                      ▼
                         nano firmware Bluetooth stack (Open Interface BLUEmagic + Apple C++ layer)
                                                      ▲  a few verified hooks
 ┌────────────────────────────────────────────────────┴───────────────────────────────┐
 │ NanoApps resident (always loaded)             integration/                           │
 │   aap_transport.c  owns the AAP channel (L2CAP PSM 0x1001), decodes notifications    │
 │   aap_le.c         BLE advertisement listener and scan control (case battery, lid)   │
 │   aes128.c         decrypts the advertisements' exact battery levels                  │
 │   btlog.c          flight recorder for the stack's own log (observation only)        │
 │   did_spoof.c      Device-ID record experiment (off by default)                      │
 └──────────────────────────────────┬─────────────────────────────────────────────────┘
                 hb_aap_state_t: shared memory, magic + ABI + size checked
 ┌──────────────────────────────────┴─────────────────────────────────────────────────┐
 │ aap_ctl app (LVGL)                            app/aap_ctl/                           │
 │   aap_ui.c      the page, the caches, the animations                                 │
 │   aap_models.c  which AirPods can do what                                            │
 │   aap_ctl.c     glue: reads the state, records intent                                │
 └────────────────────────────────────────────────────────────────────────────────────┘
```

## Why the resident owns Bluetooth

The firmware stores pointers to the receive callback and its buffers inside the L2CAP channel's control block,
and NanoApps unloads an app's image (and its heap) when the app closes. Opening the channel from the app returned
success and then killed the device, twice. The same call made from the resident's dispatch hook works and keeps
working. So:

- The resident opens, keeps and decodes the channel. Its state lives in its own `.bss`.
- The app **only reads** `hb_aap_state_t` and **records intent** in it: the mode it wants (`req_mode`), a setting
  to change (`req_ctl`), "try to connect" (`want_open`). A tap never touches Bluetooth.
- The resident acts on those on its next poll. While the app is in the foreground the app's frame callback drives
  the poll (the home screen's dispatch is quiet then); otherwise the resident's own dispatch hook does.

## Opening the channel

AirPods connect to the nano for audio as usual (A2DP). The resident waits for that link, then opens the AAP channel
once per link. If the channel does not come up it backs off and tries again, and after three failed attempts it
waits until the link changes or you tap **Try Again**. Once the channel is open it runs the handshake, subscribes to
notifications and reads the device information.

## What the page shows, and where it comes from

| On the page | Source |
|---|---|
| Name, model, serial, firmware | AAP device-information message (opcode `0x1D`) |
| Left / right battery, charging | AAP battery notifications (opcode `0x04`); the last known level is kept and dimmed when a bud is in a closed case |
| Case battery, charging, lid | **BLE advertisements** (Apple manufacturer data, type `0x07`). The case level is not sent over AAP. Levels are 10 % steps; with the proximity key the AirPods hand over on the AAP channel (opcode `0x30`/`0x31`), `aes128.c` decrypts the advertisement's tail for exact levels |
| Listening mode | AAP listening-mode notification (opcode `0x0D`), so a stem squeeze on the buds moves the highlight too |
| Settings | AAP control commands (`09 00 <id> <value> 00 00 00`) |
| Which rows exist | `aap_models.c`: identified by the BLE model id, then the model number, then the name; unknown models get only the basics |

BLE side: the resident taps the stack's per-advertisement handler (read-only; the original always runs afterwards)
and starts its own short scans through the stack's LE central. Several Apple devices advertise nearby, so an
advertisement is only accepted as *yours* if its levels agree with what AAP says, or if it decrypts with your key.

**Settings are write-only on the wire.** The AirPods announce their settings once, at connect, and never echo a
write; only the listening mode is echoed. So the resident records a sent write as the current value, the page
shows that, and if the write could not be sent at all the switch snaps back with a note. A change made on another
device while connected is not seen until the next connect.

## Safety rules the code follows

These come from the failures that preceded this design (the history is in [research/](research/)):

1. Never write outside memory you own; state is published through `hb_shared` behind a magic guard.
2. Patches are applied only after the surrounding stock bytes match, and otherwise leave everything alone
   (`btlog.c` is a single aligned store into a vtable data slot).
3. Control frames are exactly 11 bytes; an extra byte desynchronises the next frame.
4. No Bluetooth work in the touch path. Touch handlers record intent.
5. A return code is not an acknowledgement. The page shows what the AirPods report back.
6. One bounded open per link; a failure backs off and eventually waits for the user.
7. Bluetooth audio must survive everything. If music stops, that is a bug.

`btlog.c` is a flight recorder: it copies every record the stack logs (and our own breadcrumbs) into a ring in
RAM that `tools/btlog.py` reads over USB while the device keeps running, which replaced guessing and rebooting.
It can be switched off by creating `/Apps/Data/btlog.off` on the iPod. `did_spoof.c` can present the nano to the
AirPods with Apple's Bluetooth-SIG vendor id (as LibrePods does); it made no difference for what the page uses,
so it is **off** unless `/Apps/Data/did.on` exists.

## Changes to NanoApps

`integration/nanoapps-aap.patch` (against the commit pinned in `install.sh`) makes five small changes:

- `sdk/hb_shared.h`: two fields appended to the resident/app shared block (`aap_magic`, `aap_state`). The block's
  version is unchanged, so other apps ignore them.
- `system/silver_resident/silver_resident.c` and `Makefile`: call `hb_aap_init()` at start and `hb_aap_poll()`
  from the dispatch hook; build the new modules; install the flight recorder and the Device-ID hook once per boot.
- `system/silver_resident/compat_fixes.h`: declarations for the new modules.
- `sdk/hb_lv_surface.c`: an opt-in (`HB_LV_BACKBUF`) cached back buffer and a fast RGB565 presenter, described
  below. Other apps do not enable it and draw as before (the patch also reads a timer around the first refresh).

The new files are in `integration/` (`*.c`, `aes128.h`, `hb_aap_shared.h`), and `install.sh` copies them in.

## Making the page fast

The nano's CPU is slow and the framebuffer is uncached, so how the page draws matters more than what it draws.
Measured on the device:

- LVGL's renderer reads and writes every pixel it blends. Doing that on the framebuffer is slow, so the page
  renders into a **cached back buffer** and only the dirty rectangles are copied out (5.9 ms for a full screen).
- A full live redraw of the page costs 30-34 ms, too slow for scrolling. So when the page is idle it **snapshots
  itself** as one RGB565 picture, and while you scroll, a small routine (`hb_lv_present565`) copies a window of that
  picture straight to the display, bypassing LVGL's generic (and slow, about 190 ns per pixel) image blend. A scroll
  frame costs about 8.4 ms, so scrolling runs at roughly 50-60 fps. When the finger lifts, LVGL takes over again
  and redraws live.
- Colours are quantised the same way in both paths, so the picture and the live page agree to the pixel.
- Taps repaint only what changed: setters compare against the current style first, because LVGL repaints on every
  set call even when the value is the same. The selection pill and the switch knob animate over 100-120 ms.
- The typeface is Inter (an open, screen-first face close to the iOS system font), subset to ASCII.

If you build on this: `UIFRM`, `UIPRES`, `UIREN`, `UIFLUSH` and `UISNAP` lines in `./start trace` report the
frame rate, present cost, render cost and snapshot state.
