#!/bin/sh
# Show the state of the Buildroot build in one screen.
#   ./scripts/status.sh
WORK=${WORK:-$HOME/br}
LOG=$WORK/build.log

echo "=== build process ==="
PIDS=$(pgrep -f buildroot-overnight 2>/dev/null | tr '\n' ' ')
if [ -n "$PIDS" ]; then
	echo "RUNNING (pid $PIDS)"
else
	if [ -f "$LOG" ] && tail -400 "$LOG" | grep -q "DONE ==="; then
		echo "NOT running - FINISHED SUCCESSFULLY"
	elif [ -f "$LOG" ] && tail -400 "$LOG" | grep -qE "^make(\[[0-9]+\])?: \*\*\*"; then
		echo "NOT running - BUILD FAILED. First error in the log:"
		grep -m1 -E "Error:|error:" "$LOG" | cut -c1-100
	else
		echo "NOT running - finished, or stopped (check the log below)"
	fi
fi

echo
echo "=== progress ==="
if [ -f "$LOG" ]; then
	NOW=$(date +%s)
	MOD=$(stat -c %Y "$LOG" 2>/dev/null || echo "$NOW")
	echo "log last wrote $((NOW - MOD))s ago  (a live build writes constantly)"
else
	echo "no log yet at $LOG"
fi
echo "work dir: $(du -sh "$WORK" 2>/dev/null | cut -f1) used"
df -h "$WORK" 2>/dev/null | tail -1

echo
echo "=== images ==="
ls -la "$WORK"/buildroot-*/output/images/ 2>/dev/null \
	|| echo "(none yet - still building)"

echo
echo "=== last 25 log lines ==="
tail -25 "$LOG" 2>/dev/null || echo "(no log file yet)"
