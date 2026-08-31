#!/bin/sh
# Unattended overnight Buildroot build, intended to run ON the Pi.
#
#   ./scripts/buildroot-overnight.sh          # start (foreground)
#   nohup ./scripts/buildroot-overnight.sh &  # start, survives logout
#
# Produces ~/br/buildroot-*/output/images/sdcard.img and logs everything
# to ~/br/build.log. Safe to re-run: it resumes rather than restarting.
set -e

# Buildroot refuses to build as root (and root-owned output is a mess).
# Run as your normal user; the few privileged steps use sudo themselves.
if [ "$(id -u)" = "0" ]; then
	echo "error: do not run this as root / with sudo." >&2
	echo "       run it as your normal user: ./scripts/buildroot-overnight.sh" >&2
	exit 1
fi

BR_VERSION=${BR_VERSION:-2024.02.9}
WORK=${WORK:-$HOME/br}
DEFCONFIG=${DEFCONFIG:-raspberrypi3_defconfig}
REPO=$(cd "$(dirname "$0")/.." && pwd)
BR=$WORK/buildroot-$BR_VERSION
LOG=$WORK/build.log

if ! mkdir -p "$WORK" 2>/dev/null || [ ! -w "$WORK" ]; then
	echo "error: $WORK is not writable by $(id -un)." >&2
	echo "       pick a WORK dir you own, e.g. WORK=\$HOME/br" >&2
	exit 1
fi
exec >>"$LOG" 2>&1
echo "=== $(date -u) starting: $BR_VERSION / $DEFCONFIG ==="

# Build-time prerequisites (harmless if already present).
if ! command -v bison >/dev/null 2>&1; then
	# An EOL distro's mirrors may 404; that is not fatal on its own,
	# the packages may still be installable from the archive host.
	sudo apt-get update || echo "warning: apt-get update failed, continuing"
	sudo apt-get install -y build-essential bison flex bc cpio unzip rsync \
		file wget python3 libncurses-dev git || {
		echo "error: could not install build prerequisites." >&2
		echo "       on an EOL Raspbian, repoint /etc/apt/sources.list" >&2
		echo "       at legacy.raspbian.org first." >&2
		exit 1
	}
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
