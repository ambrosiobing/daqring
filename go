#!/bin/sh
# daqring - one entry point, so nothing long ever has to be typed.
#
#   ./go             show this list
#   ./go demo        build and run the two-path demo
#   ./go char        full rate sweep + latency under load
#   ./go lowrate     just the 1 kHz and 2 kHz points
#   ./go overlay     compile and install the device-tree overlay
#   ./go build       start the Buildroot image build (detached)
#   ./go status      how the Buildroot build is doing
#   ./go restart     stop a stuck build and start it again
#   ./go stop        stop the Buildroot build, leave everything else alone
#   ./go clean       stop it and delete the build tree (frees the space)
#   ./go flash /dev/sdX    write the built image to a USB stick
#   ./go log         last 40 lines of the build log
#   ./go cool        kill stray CPU load, show temperature
#   ./go verify      check the built image contains driver + overlay
#   ./go export      copy the image to the Windows desktop (from WSL)
#
# Every task updates the repo first, so this file is the only thing
# worth remembering.
cd "$(dirname "$0")" || exit 1

if [ "${DAQRING_SELF_UPDATED:-0}" != "1" ]; then
	echo "--- updating ---"
	git pull --ff-only || echo "(pull failed - using local copy)"
	DAQRING_SELF_UPDATED=1
	export DAQRING_SELF_UPDATED
	exec ./go "$@"
fi

case "${1:-}" in
demo)
	exec sudo sh ./scripts/demo.sh
	;;
char)
	exec sh ./scripts/characterise.sh
	;;
lowrate)
	RATES="1000 2000"
	export RATES
	exec sh ./scripts/characterise.sh
	;;
overlay)
	make dtbo && exec make install-overlay
	;;
build)
	exec sh ./scripts/day2.sh
	;;
status)
	exec sh ./scripts/status.sh
	;;
restart)
	exec sh ./scripts/restart.sh
	;;
stop)
	pkill -f buildroot-overnight 2>/dev/null && echo "build stopped" ||
		echo "no build was running"
	pkill -f 'wget.*buildroot' 2>/dev/null
	exit 0
	;;
clean)
	pkill -f buildroot-overnight 2>/dev/null
	rm -rf "${WORK:-$HOME/br}"
	echo "removed ${WORK:-$HOME/br}"
	exit 0
	;;
flash)
	shift
	exec sudo sh ./scripts/flash-image.sh "$@"
	;;
log)
	exec tail -40 "${WORK:-$HOME/br}/build.log"
	;;
verify)
	exec sh ./scripts/verify-image.sh
	;;
export)
	shift
	exec sh ./scripts/export-image.sh "$@"
	;;
cool)
	pkill -f 'while :; do :; done' 2>/dev/null &&
		echo "killed stray load processes" || echo "no stray load found"
	command -v vcgencmd >/dev/null 2>&1 &&
		vcgencmd measure_temp && vcgencmd get_throttled
	uptime
	exit 0
	;;
*)
	awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next }
	     NR>1 { exit }' "$0"
	;;
esac
