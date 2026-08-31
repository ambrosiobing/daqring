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
#   ./go flash /dev/sdX    write the built image to a USB stick
#   ./go log         last 40 lines of the build log
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
flash)
	shift
	exec sudo sh ./scripts/flash-image.sh "$@"
	;;
log)
	exec tail -40 "${WORK:-$HOME/br}/build.log"
	;;
*)
	awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next }
	     NR>1 { exit }' "$0"
	;;
esac
