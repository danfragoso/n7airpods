# Changelog

## 0.1.0 - first public release

- AirPods page for the iPod nano 7G (NanoApps app `aap_ctl`): battery for left bud, right bud and case with
  charging state; name, model, serial and firmware; listening mode (Off, Transparency, Adaptive, Noise
  Cancellation, per model); Conversation Awareness and Noise Cancellation with One AirPod.
- Case battery and lid state from BLE advertisements, exact levels with the AirPods' proximity key.
- Resident modules for NanoApps: the AAP L2CAP channel, the LE listener, a Bluetooth flight recorder.
- iOS-style UI with the Inter typeface, 60-fps-class scrolling (cached page, direct RGB565 present), animated
  selection and switches, press feedback.
- `install.sh` builds everything from source against a pinned NanoApps commit and installs it;
  `--build-only`, `--no-apple-art` and `--restore-stock` options.
- Tested on an iPod nano 7G (A1446), firmware 1.1.2, with AirPods Pro 2 (USB-C). Other models are in the capability
  table but unverified.
