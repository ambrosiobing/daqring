#!/bin/sh
# Write the built image to a USB stick, with guards against the classic
# catastrophe of typing the wrong device.
#
#   sudo ./scripts/flash-image.sh /dev/sda [image.img]
#
# Refuses to touch any mmcblk* device (that is the SD card the system
# is running from) and refuses a device holding a mounted filesystem.
set -e

DEV=$1
IMG=$2

if [ -z "$IMG" ]; then
	IMG=$(ls "$HOME"/br/buildroot-*/output/images/sdcard.img 2>/dev/null | head -1)
	[ -z "$IMG" ] && IMG=$(ls /home/*/br/buildroot-*/output/images/sdcard.img 2>/dev/null | head -1)
fi

if [ -z "$DEV" ]; then
	echo "usage: sudo $0 /dev/sdX [image.img]" >&2
	echo >&2
	lsblk -o NAME,SIZE,TYPE,MOUNTPOINT >&2
	exit 2
fi

case "$DEV" in
*mmcblk*)
	echo "refusing: $DEV is an SD card device - that is the running system." >&2
	exit 1
	;;
/dev/*) ;;
*)
	echo "refusing: $DEV does not look like a device path." >&2
	exit 1
	;;
esac

[ -b "$DEV" ] || { echo "error: $DEV is not a block device." >&2; exit 1; }
[ -f "$IMG" ] || { echo "error: image not found: $IMG" >&2; exit 1; }

if lsblk -no MOUNTPOINT "$DEV" 2>/dev/null | grep -q '^/$\|^/boot'; then
	echo "refusing: $DEV carries the running root or boot filesystem." >&2
	exit 1
fi

echo "image : $IMG ($(du -h "$IMG" | cut -f1))"
echo "target: $DEV"
lsblk "$DEV"
echo
echo "EVERYTHING ON $DEV WILL BE DESTROYED."
printf 'type YES to continue: '
read -r ans
[ "$ans" = "YES" ] || { echo "aborted."; exit 1; }

# Unmount anything auto-mounted from the target first.
for m in $(lsblk -no MOUNTPOINT "$DEV" 2>/dev/null | grep -v '^$'); do
	echo "unmounting $m"
	umount "$m" || true
done

dd if="$IMG" of="$DEV" bs=4M conv=fsync status=progress
sync
echo "done. remove the stick, or boot the Pi from it with the SD card out."
