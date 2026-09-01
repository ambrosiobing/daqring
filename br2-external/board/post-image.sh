#!/bin/sh
# Buildroot post-image hook: put the daqring device-tree overlay into
# the Raspberry Pi boot partition and enable it in config.txt, so the
# booted image probes the platform driver in *hardware* mode instead of
# falling back to simulation.
#
# This must be a post-IMAGE hook, and must run before the Pi's own
# post-image script: rpi-firmware/ is only installed into BINARIES_DIR
# during the images step (too late for post-build), and genimage packs
# the boot partition from it (too early to append afterwards).
#
# $1 is BINARIES_DIR; extra args from BR2_ROOTFS_POST_SCRIPT_ARGS are
# ignored.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
DTS=$HERE/../../overlays/daqring-overlay.dts
BOOT=${BINARIES_DIR:-$1}/rpi-firmware
DTC=${HOST_DIR:-/usr}/bin/dtc
command -v "$DTC" >/dev/null 2>&1 || DTC=dtc

[ -d "$BOOT" ] || {
	echo "post-image: $BOOT missing - not a Raspberry Pi config?" >&2
	exit 0
}

mkdir -p "$BOOT/overlays"
"$DTC" -@ -I dts -O dtb -o "$BOOT/overlays/daqring.dtbo" "$DTS"
echo "post-image: installed overlays/daqring.dtbo"

if ! grep -q '^dtoverlay=daqring' "$BOOT/config.txt" 2>/dev/null; then
	printf '\n# daqring: GPIO17 trigger looped to GPIO27 interrupt\ndtoverlay=daqring\n' \
		>>"$BOOT/config.txt"
	echo "post-image: enabled dtoverlay=daqring in config.txt"
fi
