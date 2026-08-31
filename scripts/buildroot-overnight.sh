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
TARBALL=$WORK/buildroot-$BR_VERSION.tar.gz

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

# Download with timeouts and retries. A bare "wget -q" can hang forever
# on a stalled connection (and prints nothing, so the log looks frozen);
# -4 avoids blackholed IPv6 routes, --continue resumes a partial file,
# and dot:giga keeps writing progress so the log shows liveness.
fetch() {
	echo "--- trying: $1"
	wget -4 --timeout=30 --tries=3 --continue --progress=dot:giga \
		-O "$TARBALL" "$1"
}

if [ ! -d "$BR" ]; then
	echo "--- fetching buildroot $BR_VERSION ---"
	fetch "https://buildroot.org/downloads/buildroot-$BR_VERSION.tar.gz" ||
	fetch "https://github.com/buildroot/buildroot/archive/refs/tags/$BR_VERSION.tar.gz" ||
	# Last resort: an EOL distro's CA bundle may reject a modern chain.
	wget -4 --timeout=30 --tries=3 --continue --no-check-certificate \
		--progress=dot:giga -O "$TARBALL" \
		"https://buildroot.org/downloads/buildroot-$BR_VERSION.tar.gz" || {
		echo "error: could not download buildroot. check the Pi's network:" >&2
		echo "       wget -4 -O /dev/null https://buildroot.org/" >&2
		exit 1
	}

	echo "--- downloaded $(du -h "$TARBALL" | cut -f1), extracting ---"
	if ! gzip -t "$TARBALL" 2>/dev/null; then
		echo "error: $TARBALL is not a valid archive (partial download?)." >&2
		echo "       delete it and re-run." >&2
		exit 1
	fi
	tar -xzf "$TARBALL" -C "$WORK"
	# The GitHub tag tarball unpacks without the "buildroot-" prefix.
	[ -d "$BR" ] || [ ! -d "$WORK/$BR_VERSION" ] || mv "$WORK/$BR_VERSION" "$BR"
fi

cd "$BR"

if [ ! -f .config ]; then
	echo "--- configuring from $DEFCONFIG + daqring fragment ---"
	make BR2_EXTERNAL="$REPO/br2-external" "$DEFCONFIG"
	cat "$REPO/br2-external/configs/daqring.fragment" >>.config
	make BR2_EXTERNAL="$REPO/br2-external" olddefconfig
fi

# A toolchain choice that did not apply to this target leaves
# BR2_TOOLCHAIN_EXTERNAL_CUSTOM selected with an empty path, which only
# fails ~20 minutes in with "Cannot execute cross-compiler
# '/arm-linux-gcc'". Detect that up front and fall back to the internal
# toolchain, which always matches the target.
if grep -q '^BR2_TOOLCHAIN_EXTERNAL_CUSTOM=y' .config; then
	echo "--- unusable external toolchain selected; switching to internal ---"
	sed -i '/^BR2_TOOLCHAIN_EXTERNAL/d' .config
	make BR2_EXTERNAL="$REPO/br2-external" olddefconfig
	make BR2_EXTERNAL="$REPO/br2-external" clean
fi

echo "--- toolchain now selected: ---"
grep -E '^BR2_TOOLCHAIN_[A-Z_]*=y' .config || true

# Parallelism: the fragment defaults to 2 jobs for a 1 GB Pi. On a
# roomier build host use more, but stay within about one job per GB of
# RAM - Buildroot's compile steps are memory-hungry and the OOM killer
# is a slow way to learn that.
CORES=$(nproc 2>/dev/null || echo 2)
RAM_GB=$(awk '/MemTotal/{printf "%d", $2/1048576}' /proc/meminfo 2>/dev/null || echo 1)
JOBS=$RAM_GB
[ "$JOBS" -lt 2 ] && JOBS=2
[ "$JOBS" -gt "$CORES" ] && JOBS=$CORES
echo "--- build host: $CORES cores, ${RAM_GB}G RAM -> BR2_JLEVEL=$JOBS ---"
sed -i "s/^BR2_JLEVEL=.*/BR2_JLEVEL=$JOBS/" .config
make BR2_EXTERNAL="$REPO/br2-external" olddefconfig

echo "--- building (this is the long part) ---"
make BR2_EXTERNAL="$REPO/br2-external"

echo "=== $(date -u) DONE ==="
ls -la output/images/
