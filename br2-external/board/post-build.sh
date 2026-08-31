#!/bin/sh
# Buildroot post-build hook: put the daqring device-tree overlay into
# the Raspberry Pi boot partition and enable it in config.txt, so the
# booted image probes the platform driver in *hardware* mode instead of
# falling back to simulation.
#
# Buildroot exports BINARIES_DIR and HOST_DIR to post-build scripts.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
DTS=$HERE/../../overlays/daqring-overlay.dts
BOOT=${BINARIES_DIR:?}/rpi-firmware
DTC=${HOST_DIR:-/usr}/bin/dtc
command -v "$DTC" >/dev/null 2>&1 || DTC=dtc

[ -d "$BOOT" ] || {
	echo "post-build: $BOOT missing - not a Raspberry Pi config?" >&2
	exit 0
}

mkdir -p "$BOOT/overlays"
"$DTC" -@ -I dts -O dtb -o "$BOOT/overlays/daqring.dtbo" "$DTS"
echo "post-build: installed overlays/daqring.dtbo"

if ! grep -q '^dtoverlay=daqring' "$BOOT/config.txt" 2>/dev/null; then
	printf '\n# daqring: GPIO17 trigger looped to GPIO27 interrupt\ndtoverlay=daqring\n' \
		>>"$BOOT/config.txt"
	echo "post-build: enabled dtoverlay=daqring in config.txt"
fi
