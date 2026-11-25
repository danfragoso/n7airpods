#!/usr/bin/env bash
# Verify the nano is in DFU (USB 05ac:1234 = 4660 decimal) and watch for it.
#
#   tools/dfu.sh            -> check once
#   tools/dfu.sh --watch    -> poll for 60s while you do the button dance
#   tools/dfu.sh --resume   -> SIGCONT the Apple daemons when you're done
#
# Stopping Apple's device daemons first matters: they grab a DFU nano within
# seconds. This script cannot sudo for you, so it prints the commands.
set -uo pipefail
DFU_PID=4660   # 0x1234

find_dfu() {
    ioreg -p IOUSB -l -w0 2>/dev/null | grep -q "\"idProduct\" = $DFU_PID" && return 0
    system_profiler SPUSBDataType 2>/dev/null | grep -A4 -i "iPod" | grep -q "0x1234" && return 0
    return 1
}

case "${1:-}" in
  --resume)
    sudo pkill -CONT -f MobileDeviceUpdater || true
    sudo pkill -CONT -f AMPDevicesAgent || true
    sudo pkill -CONT -f AMPDeviceDiscoveryAgent || true
    echo "daemons resumed"
    exit 0
    ;;
esac

echo "Before you start, stop the daemons that steal the DFU device:"
echo
echo "  sudo pkill -STOP -f MobileDeviceUpdater"
echo "  sudo pkill -STOP -f AMPDevicesAgent"
echo "  sudo pkill -STOP -f AMPDeviceDiscoveryAgent"
echo
echo "Also: close Finder windows / Music / Apple Devices, and use a"
echo "USB-A to Lightning cable, plugged straight into the Mac (no hub)."
echo
echo "Then: hold Home + Sleep/Wake together for ~10 s, keep holding until the"
echo "screen goes black and STAYS black, then release both at once."
echo

if [ "${1:-}" = "--watch" ]; then
    printf "waiting for 05ac:1234 "
    for _ in $(seq 1 60); do
        if find_dfu; then
            echo; echo "DFU DETECTED (05ac:1234) — do not touch the nano."
            exit 0
        fi
        printf "."
        sleep 1
    done
    echo; echo "not seen after 60 s. If the Apple logo appeared, it booted normally —"
    echo "that is a failed attempt, not a broken device. Try again with a longer hold."
    exit 1
fi

if find_dfu; then
    echo "DFU DETECTED (05ac:1234)"
    # Topology check: wInd3x decrypt transfers ~10 MB over control/bulk. Through a hub
    # chain the device has been observed to drop off the bus mid-transfer
    # ("libusb: no device" at ~37%). Warn if we are not on a root port.
    depth=$(ioreg -p IOUSB -w0 -l 2>/dev/null \
            | grep -n 'USB DFU Device@' \
            | head -1 | cut -d: -f1)
    if [ -n "${depth:-}" ]; then
        line=$(pure=$(ioreg -p IOUSB -w0 -l 2>/dev/null | sed -n "/USB DFU Device@/p" | head -1); echo "$pure")
        bars=$(printf '%s' "$line" | awk -F'|' '{print NF-1}')
        if [ "${bars:-0}" -ge 2 ]; then
            echo "WARNING: DFU device is behind a hub/dock (depth $bars)."
            echo "         Mid-transfer dropouts are likely. Plug the Lightning cable"
            echo "         directly into the Mac and re-enter DFU."
        else
            echo "topology: attached directly (good)."
        fi
    fi
    exit 0
else
    echo "No DFU device right now (that is expected before the button dance)."
    echo "Re-run with --watch, then do the buttons."
    exit 1
fi
