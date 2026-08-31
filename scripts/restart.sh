#!/bin/sh
# Stop a stuck/stale build and start a fresh one, detached.
#   ./scripts/restart.sh
cd "$(dirname "$0")/.."

echo "=== stopping any previous run ==="
pkill -f buildroot-overnight 2>/dev/null && echo "killed build script" || true
pkill -f 'wget.*buildroot' 2>/dev/null && echo "killed stalled download" || true
sleep 2

echo
echo "=== connectivity check (IPv4) ==="
if wget -4 --timeout=15 --tries=1 -q -O /dev/null https://buildroot.org/ 2>/dev/null; then
	echo "buildroot.org: reachable"
elif wget -4 --timeout=15 --tries=1 -q --no-check-certificate \
		-O /dev/null https://buildroot.org/ 2>/dev/null; then
	echo "buildroot.org: reachable, but TLS certificate rejected"
	echo "(old CA bundle - the build script falls back automatically)"
else
	echo "buildroot.org: NOT reachable - fix the Pi's network first"
fi

echo
echo "=== restarting build ==="
rm -f nohup.out
nohup ./scripts/buildroot-overnight.sh >/dev/null 2>&1 &
sleep 5
echo "started. check with: ./scripts/status.sh"
