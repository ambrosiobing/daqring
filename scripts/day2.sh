#!/bin/sh
# One-shot day-2 starter, meant to be run ON the Pi:
#
#   ./scripts/day2.sh
#
# Repairs apt if the distro is EOL, installs the build prerequisites,
# then launches the Buildroot build detached so it survives logout.
# Check on it any time with ./scripts/status.sh
set -e
cd "$(dirname "$0")/.."

if [ "$(id -u)" = "0" ]; then
	echo "error: run this as your normal user, not with sudo." >&2
	exit 1
fi

echo "=== 1/3 package index ==="
if ! sudo apt-get update; then
	echo "--- update failed; repointing EOL Raspbian mirrors at the archive ---"
	sudo sed -i.bak \
		-e 's|mirrordirector.raspbian.org|legacy.raspbian.org|g' \
		-e 's|raspbian.raspberrypi.org|legacy.raspbian.org|g' \
		/etc/apt/sources.list
	sudo apt-get update || echo "(still not clean - continuing anyway)"
fi

echo "=== 2/3 build prerequisites ==="
sudo apt-get install -y build-essential bison flex bc cpio unzip rsync \
	file wget python3 libncurses-dev git device-tree-compiler

echo "=== 3/3 launching Buildroot (detached) ==="
rm -f nohup.out
nohup ./scripts/buildroot-overnight.sh >/dev/null 2>&1 &
sleep 5

echo
echo "started. check progress with:   ./scripts/status.sh"
echo "the build runs for hours - leave the Pi powered on."
