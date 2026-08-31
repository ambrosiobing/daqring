#!/bin/sh
# Show the state of the Buildroot build in one screen.
#   ./scripts/status.sh
WORK=${WORK:-$HOME/br}
LOG=$WORK/build.log

echo "=== process ==="
pgrep -af 'buildroot-overnight|make' 2>/dev/null | head -5 \
	|| echo "(nothing running)"

echo
echo "=== disk ==="
df -h "$WORK" 2>/dev/null | tail -1 || df -h "$HOME" | tail -1

echo
echo "=== images ==="
ls -la "$WORK"/buildroot-*/output/images/ 2>/dev/null \
	|| echo "(none yet - still building)"

echo
echo "=== last 25 log lines ($LOG) ==="
tail -25 "$LOG" 2>/dev/null || echo "(no log file yet)"
