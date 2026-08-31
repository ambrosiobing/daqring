#!/bin/sh
# Unattended overnight Buildroot build, intended to run ON the Pi.
#
#   ./scripts/buildroot-overnight.sh          # start (foreground)
#   nohup ./scripts/buildroot-overnight.sh &  # start, survives logout
#
# Produces ~/br/buildroot-*/output/images/sdcard.img and logs everything
# to ~/br/build.log. Safe to re-run: it resumes rather than restarting.
set -e

BR_VERSION=${BR_VERSION:-2024.02.9}
WORK=${WORK:-$HOME/br}
DEFCONFIG=${DEFCONFIG:-raspberrypi3_defconfig}
REPO=$(cd "$(dirname "$0")/.." && pwd)
BR=$WORK/buildroot-$BR_VERSION
LOG=$WORK/build.log

mkdir -p "$WORK"
exec >>"$LOG" 2>&1
echo "=== $(date -u) starting: $BR_VERSION / $DEFCONFIG ==="

# Build-time prerequisites (harmless if already present).
if ! command -v bison >/dev/null 2>&1; then
	sudo apt-get update
	sudo apt-get install -y build-essential bison flex bc cpio unzip rsync \
		file wget python3 libncurses-dev git
fi

if [ ! -d "$BR" ]; then
	echo "--- fetching buildroot $BR_VERSION ---"
	wget -q -O "$WORK/buildroot.tar.gz" \
		"https://buildroot.org/downloads/buildroot-$BR_VERSION.tar.gz"
	tar -xzf "$WORK/buildroot.tar.gz" -C "$WORK"
fi

cd "$BR"

if [ ! -f .config ]; then
	echo "--- configuring from $DEFCONFIG + daqring fragment ---"
	make BR2_EXTERNAL="$REPO/br2-external" "$DEFCONFIG"
	cat "$REPO/br2-external/configs/daqring.fragment" >>.config
	make BR2_EXTERNAL="$REPO/br2-external" olddefconfig
fi

echo "--- building (this is the long part) ---"
make BR2_EXTERNAL="$REPO/br2-external"

echo "=== $(date -u) DONE ==="
ls -la output/images/
