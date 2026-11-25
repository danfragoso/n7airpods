#!/usr/bin/env bash
# install.sh - add the AirPods app to an iPod nano 7G that runs NanoApps.
#
#   ./install.sh                 build everything from source and install it on the connected iPod nano
#   ./install.sh --build-only    build only; touch nothing on the iPod (a good first run)
#   ./install.sh --restore-stock put NanoApps' own, unpatched resident back on the iPod
#
# What it does, in order:
#   1. gets NanoApps (github.com/nfzerox/NanoApps) at the exact commit this project was made against
#   2. applies integration/nanoapps-aap.patch, adds the resident modules (integration/*.c) and the app
#      (app/aap_ctl) to that checkout
#   3. builds the app and the patched resident with NanoApps' own ./start (which installs missing
#      compilers and tools itself, the first run takes a few minutes)
#   4. installs the resident first (the iPod restarts), then the app
#
# Options:
#   --nanoapps DIR   use DIR for the NanoApps checkout (default: ./.nanoapps, created if missing)
#   --no-apple-art   build without Apple's AirPods pictures and icon art (the page draws generic stand-ins)
#   --yes            do not ask before writing to the iPod
#   -h, --help       this text
set -euo pipefail

NANOAPPS_REPO="https://github.com/nfzerox/NanoApps.git"
NANOAPPS_COMMIT="0c2bd815217b964f364edd527ae07c72b4217b94"    # what integration/nanoapps-aap.patch applies to

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$HERE/.nanoapps"
BUILD_ONLY=0
ASSUME_YES=0
RESTORE=0

say()  { printf '\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "'$1' is required but not installed"; }

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --nanoapps)      [ $# -ge 2 ] || die "--nanoapps needs a directory"; WORK="$2"; shift 2 ;;
        --build-only)    BUILD_ONLY=1; shift ;;
        --restore-stock) RESTORE=1; shift ;;
        --no-apple-art)  export APPLE_ART=0; shift ;;
        --yes|-y)        ASSUME_YES=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)               die "unknown option: $1 (try --help)" ;;
    esac
done

need git
need python3
need make

case "$(uname -s)" in
    Darwin|Linux) ;;
    *) die "NanoApps' installer runs on macOS and Linux only" ;;
esac

# --- a NanoApps checkout at the pinned commit -----------------------------------------------------------
checkout() {   # checkout DIR
    local dir="$1"
    if [ ! -e "$dir" ]; then
        say "Cloning NanoApps into $dir"
        git clone --quiet "$NANOAPPS_REPO" "$dir"
        git -C "$dir" checkout --quiet "$NANOAPPS_COMMIT"
    fi
    [ -d "$dir/.git" ] || die "$dir exists but is not a git checkout of NanoApps"
    local head; head="$(git -C "$dir" rev-parse HEAD)"
    if [ "$head" != "$NANOAPPS_COMMIT" ]; then
        die "$dir is at ${head:0:12}, but this release is made for NanoApps ${NANOAPPS_COMMIT:0:12}.
       Check it out (git -C $dir checkout $NANOAPPS_COMMIT) or point --nanoapps at a fresh directory."
    fi
}

patch_file="$HERE/integration/nanoapps-aap.patch"

patched() { git -C "$1" apply --reverse --check "$patch_file" >/dev/null 2>&1; }
patch_paths() { sed -n 's#^+++ b/##p' "$patch_file"; }      # the files the patch changes

overlay() {    # overlay DIR: patch + new files + the app
    local dir="$1"
    if patched "$dir"; then
        note "NanoApps patch already applied"
    elif git -C "$dir" apply --check "$patch_file" 2>/dev/null; then
        git -C "$dir" apply "$patch_file"
        note "applied integration/nanoapps-aap.patch"
    elif [ "$dir" = "$HERE/.nanoapps" ]; then
        # our own checkout, patched by an older release: put the files the patch touches back, then apply it again
        note "updating the patch (an older version of it was applied)"
        git -C "$dir" checkout --quiet -- $(patch_paths)
        git -C "$dir" apply --check "$patch_file" 2>/dev/null || die "integration/nanoapps-aap.patch does not apply to $dir; delete it and run again"
        git -C "$dir" apply "$patch_file"
    else
        die "integration/nanoapps-aap.patch does not apply to $dir (files already edited by hand?).
       Use a clean checkout: --nanoapps <new directory>"
    fi
    for f in aap_le.c aap_transport.c aes128.c aes128.h btlog.c did_spoof.c; do
        cp "$HERE/integration/$f" "$dir/system/silver_resident/$f"
    done
    cp "$HERE/integration/hb_aap_shared.h" "$dir/sdk/hb_aap_shared.h"
    note "copied the resident modules"
    mkdir -p "$dir/apps/aap_ctl"
    if command -v rsync >/dev/null 2>&1; then
        rsync -a --delete --exclude build "$HERE/app/aap_ctl/" "$dir/apps/aap_ctl/"
    else
        rm -rf "$dir/apps/aap_ctl"; mkdir -p "$dir/apps/aap_ctl"
        (cd "$HERE/app/aap_ctl" && tar cf - .) | (cd "$dir/apps/aap_ctl" && tar xf -)
    fi
    if [ "${APPLE_ART:-1}" = 0 ]; then rm -f "$dir/apps/aap_ctl/icon_art.png"; note "left out Apple's artwork (--no-apple-art)"; fi
    note "copied the app (apps/aap_ctl)"
}

confirm() {    # confirm "what is about to happen"
    [ "$ASSUME_YES" = 1 ] && return 0
    printf '\n%s\nContinue? [y/N] ' "$1"
    read -r ans
    case "$ans" in y|Y|yes|YES) return 0 ;; *) echo "Cancelled. Nothing was written to the iPod."; exit 1 ;; esac
}

# --- restore the stock resident -------------------------------------------------------------------------
if [ "$RESTORE" = 1 ]; then
    STOCK="${WORK}-stock"
    say "Preparing an unpatched NanoApps checkout in $STOCK"
    checkout "$STOCK"
    patched "$STOCK" && die "$STOCK has the AirPods patch applied; use a clean directory"
    confirm "This replaces the resident on the connected iPod nano with NanoApps' own, and restarts the iPod.
The AirPods app will then show 'Resident update needed' until you install it again; delete
Apps/AirPods.nanoapp from the iPod's disk to remove it."
    say "Installing the stock resident"
    (cd "$STOCK" && ./start install --resident-only)
    say "Done. The iPod runs NanoApps' own resident again."
    exit 0
fi

# --- build ------------------------------------------------------------------------------------------------
checkout "$WORK"
say "Adding the AirPods changes to $WORK"
overlay "$WORK"

say "Building the app"
(cd "$WORK" && ./start build aap_ctl)
say "Building the resident"
make -s -C "$WORK/system/silver_resident"

app_bin="$WORK/apps/aap_ctl/build/aap_ctl.hbapp"
res_bin="$WORK/system/silver_resident/build/silver_resident.hbapp"
[ -f "$app_bin" ] || die "the app did not build ($app_bin missing)"
[ -f "$res_bin" ] || die "the resident did not build ($res_bin missing)"
sum() { if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -c1-16; else sha256sum "$1" | cut -c1-16; fi; }
note "app      $(wc -c < "$app_bin" | tr -d ' ') bytes  sha256 $(sum "$app_bin")...  $app_bin"
note "resident $(wc -c < "$res_bin" | tr -d ' ') bytes  sha256 $(sum "$res_bin")...  $res_bin"

if [ "$BUILD_ONLY" = 1 ]; then
    say "Build finished. Nothing was written to the iPod."
    exit 0
fi

# --- install ----------------------------------------------------------------------------------------------
confirm "This writes to the connected iPod nano 7G: it replaces NanoApps' resident with the patched one,
installs the AirPods app, and restarts the iPod (twice). NanoApps keeps a backup of what it replaces in
$WORK/build/install-backups. Keep the iPod connected until both steps are done."

say "Installing the patched resident (the iPod restarts)"
(cd "$WORK" && ./start install --resident-only)

say "Installing the AirPods app"
sleep 8        # the iPod has to come back from its restart and show up on USB again
ok=0
for attempt in 1 2 3 4 5 6; do
    if (cd "$WORK" && ./start install aap_ctl); then ok=1; break; fi
    note "attempt $attempt failed (the iPod may still be restarting); waiting..."
    sleep 6
done
[ "$ok" = 1 ] || die "the app did not install. The resident is already updated; run ./install.sh again."

say "Done."
note "On the iPod: pair your AirPods in Settings if you have not, connect them, then open the AirPods app."
note "If something looks wrong, see the Troubleshooting section of the README."
