#!/bin/sh
# Copy the built image out of WSL onto the Windows desktop, where
# Raspberry Pi Imager ("Use custom") can write it to a USB stick.
WORK=${WORK:-$HOME/br}
IMG=$(ls "$WORK"/buildroot-*/output/images/sdcard.img 2>/dev/null | head -1)
[ -f "$IMG" ] || { echo "no sdcard.img found under $WORK"; exit 1; }

DEST=$1
if [ -z "$DEST" ]; then
	# Ask Windows where the user profile is, rather than guessing.
	if command -v cmd.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
		WINHOME=$(cmd.exe /c 'echo %USERPROFILE%' 2>/dev/null | tr -d '\r\n')
		[ -n "$WINHOME" ] && DEST=$(wslpath "$WINHOME")/Desktop
	fi
fi
[ -n "$DEST" ] || DEST=$(ls -d /mnt/c/Users/*/Desktop 2>/dev/null | head -1)
[ -d "$DEST" ] || { echo "cannot find a Windows desktop; pass one: ./go export /mnt/c/..."; exit 1; }

echo "copying $(du -h "$IMG" | cut -f1) to $DEST ..."
cp "$IMG" "$DEST/daqring-sdcard.img"
echo "done: $DEST/daqring-sdcard.img"
echo
echo "Next: Raspberry Pi Imager -> Use custom -> daqring-sdcard.img"
echo "      write it to the USB stick, then boot the Pi with the SD card OUT."
