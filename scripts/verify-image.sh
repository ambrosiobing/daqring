#!/bin/sh
# Check that the built image really contains what makes the demo work:
# the driver, the test client, the overlay, and the config.txt line
# that loads it. Without the overlay the image boots into simulation
# mode, which looks fine until you look at /sys/.../mode.
WORK=${WORK:-$HOME/br}
BR=$(ls -d "$WORK"/buildroot-*/ 2>/dev/null | head -1)
[ -n "$BR" ] || { echo "no buildroot tree under $WORK"; exit 1; }
IMG=$BR/output/images
TGT=$BR/output/target

ok()   { printf '  OK    %s\n' "$1"; }
bad()  { printf '  MISS  %s\n' "$1"; FAIL=1; }
FAIL=0

echo "=== image: $IMG ==="
[ -f "$IMG/sdcard.img" ] &&
	ok "sdcard.img ($(du -h "$IMG/sdcard.img" | cut -f1))" ||
	bad "sdcard.img"

echo "=== boot partition ==="
[ -f "$IMG/rpi-firmware/overlays/daqring.dtbo" ] &&
	ok "overlays/daqring.dtbo" || bad "overlays/daqring.dtbo"
grep -q '^dtoverlay=daqring' "$IMG/rpi-firmware/config.txt" 2>/dev/null &&
	ok "config.txt: dtoverlay=daqring" || bad "config.txt: dtoverlay=daqring"
[ -f "$IMG/bcm2710-rpi-3-b-plus.dtb" ] &&
	ok "bcm2710-rpi-3-b-plus.dtb (Pi 3 B+)" || bad "3 B+ device tree"

echo "=== root filesystem ==="
KO=$(find "$TGT/lib/modules" -name 'daqring.ko*' 2>/dev/null | head -1)
[ -n "$KO" ] && ok "daqring.ko (${KO#$TGT})" || bad "daqring.ko"
[ -x "$TGT/usr/bin/daqring_test" ] &&
	ok "/usr/bin/daqring_test" || bad "/usr/bin/daqring_test"
[ -f "$TGT/etc/init.d/S99daqring" ] &&
	ok "/etc/init.d/S99daqring (loads at boot)" || bad "S99daqring"

echo
if [ "$FAIL" = "1" ]; then
	echo "Something is missing - the image will boot, but the demo will not"
	echo "be complete. Re-run ./go restart after a fix."
	exit 1
fi
echo "All present. ./go export copies the image to Windows for flashing."
