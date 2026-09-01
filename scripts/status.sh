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
	# Compare where the last success line and the last make failure
	# appear: the log is appended across runs, so an old "DONE ==="
	# further up must not mask a failure from the latest run.
	if [ -f "$LOG" ]; then
		D=$(grep -n "DONE ===" "$LOG" | tail -1 | cut -d: -f1)
		E=$(grep -nE "^make(\[[0-9]+\])?: \*\*\*" "$LOG" | tail -1 | cut -d: -f1)
		if [ -n "$E" ] && [ "${E:-0}" -gt "${D:-0}" ]; then
			echo "NOT running - BUILD FAILED. Last error:"
			sed -n "$((E > 3 ? E - 3 : 1)),${E}p" "$LOG" | cut -c1-110
		elif [ -n "$D" ]; then
			echo "NOT running - FINISHED SUCCESSFULLY"
		else
			echo "NOT running - stopped before finishing (see log below)"
		fi
	else
		echo "NOT running - no log yet"
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
