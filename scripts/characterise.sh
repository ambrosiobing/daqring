#!/bin/sh
# Characterise daqring on real hardware: a sample-rate sweep, then the
# same rate again under CPU load. Reloads the module between runs so
# each line reports counters for that run alone.
#
#   ./scripts/characterise.sh              # 1k..50k, 5 s each
#   SECS=10 RATES="1000 20000" ./scripts/characterise.sh
#
# Output is one compact line per operating point, ready to read off the
# screen or paste into the README.
cd "$(dirname "$0")/.."

SECS=${SECS:-5}
RATES=${RATES:-"1000 5000 10000 20000 50000"}
SYSFS=/sys/class/misc/daqring
OUT=/tmp/daqring_run.txt

[ -f daqring.ko ] || make module || exit 1
[ -x test/daqring_test ] || make test/daqring_test || exit 1

run_point() {	# $1 = rate, $2 = label
	sudo rmmod daqring 2>/dev/null
	sudo insmod daqring.ko || return 1
	# misc devices get a fresh dynamic minor on every load; wait for
	# udev to recreate the node, or the first run opens a stale one.
	command -v udevadm >/dev/null 2>&1 && sudo udevadm settle 2>/dev/null
	i=0
	while [ ! -c /dev/daqring ] && [ "$i" -lt 50 ]; do
		sleep 0.1
		i=$((i + 1))
	done
	sudo ./test/daqring_test mmap "$1" "$SECS" >"$OUT" 2>&1
	ACH=$(sed -n 's/.*(\([0-9]*\) samples\/s).*/\1/p' "$OUT" | head -1)
	GAPS=$(sed -n 's/.*, \([0-9]*\) sequence gaps.*/\1/p' "$OUT" | head -1)
	MODE=$(cat $SYSFS/mode 2>/dev/null)
	LAT=$(cat $SYSFS/irq_latency 2>/dev/null)
	printf '%-6s %-6s ach=%-7s gaps=%-4s %s\n' \
		"$1" "$2" "${ACH:-?}/s" "${GAPS:-?}" "$LAT"
	# If the run produced nothing parseable, show what it actually said -
	# guessing from counters alone wastes a round trip.
	if [ -z "$ACH" ]; then
		echo "       ---- raw output from the failed run ----"
		sed 's/^/       /' "$OUT" | head -6
		echo "       ---- mode=$MODE, dmesg tail ----"
		dmesg | tail -3 | sed 's/^/       /'
	fi
	sudo rmmod daqring 2>/dev/null
}

echo "=== daqring characterisation: ${SECS}s per point, mode=$(cat $SYSFS/mode 2>/dev/null || echo '?') ==="
echo "kernel: $(uname -r)  board: $(sed -n 's/^Model\s*:\s*//p' /proc/cpuinfo | head -1)"
echo

for r in $RATES; do
	run_point "$r" "idle"
done

# Same rate under load: one spinner per core. This is the number that
# matters for determinism - an acquisition box is never idle.
echo
NCPU=$(nproc 2>/dev/null || echo 4)
echo "--- loading $NCPU cores ---"
i=0
while [ "$i" -lt "$NCPU" ]; do
	sh -c 'while :; do :; done' &
	i=$((i + 1))
done
LOADPIDS=$(jobs -p)
run_point 20000 "load"
kill $LOADPIDS 2>/dev/null
echo "--- load stopped ---"
