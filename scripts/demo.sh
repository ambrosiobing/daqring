#!/bin/sh
# Build, load, exercise both data paths, show sysfs, unload.
set -e
cd "$(dirname "$0")/.."

make
sudo insmod daqring.ko
trap 'sudo rmmod daqring' EXIT INT TERM

echo "--- read()/poll() path, 5 kHz for 3 s ---"
sudo ./test/daqring_test read 5000 3

echo "--- mmap zero-copy path, 20 kHz for 3 s ---"
sudo ./test/daqring_test mmap 20000 3

echo "--- sysfs ---"
for f in mode sample_rate_hz produced overruns running irq_latency; do
	printf '%-16s %s\n' "$f" "$(cat /sys/class/misc/daqring/$f)"
done
if [ "$(cat /sys/class/misc/daqring/mode)" = "hardware" ]; then
	echo "--- irq latency histogram ---"
	cat /sys/class/misc/daqring/irq_latency_hist
fi

echo "--- dmesg ---"
sudo dmesg | grep daqring | tail -n 4
