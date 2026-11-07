#!/bin/bash
# Rate and bank before/after one deep S3 suspend. Logs are written as it goes.
set -u
cd "$(dirname "$0")/.."
TAG="${TAG:-$(date +%Y%m%d-%H%M%S)}"
LOG="$PWD/sr/rate-cycle-$TAG.log"
exec > >(tee -a "$LOG") 2>&1
echo "# start $(date -Is) mem_sleep=$(cat /sys/power/mem_sleep)"

if [ "$(id -u)" -ne 0 ]; then
	echo "need root"
	exit 1
fi

PY="$PWD/.venv/bin/python"
rate() {
	runuser -u kuzz -- env CUDA_DEVICE_ORDER=PCI_BUS_ID CUDA_VISIBLE_DEVICES=0 "$PY" probe/mma_rate.py --json "sr/rate-$TAG-$1.json" 2>/dev/null \
		| grep -E 'peak summary|x fp16->fp16' -A0
}
bank() {
	insmod ./gpu_peek_gb206.ko target=0000:08:00.0 && sleep 1 \
		&& dmesg | grep -E 'SS0 \(|SS1 \(' | tail -2
	rmmod gpu_peek_gb206
}

echo "# rate before"
rate before
echo "# bank before"
bank
sync
echo "# suspending in 8s (deep). if it does not come back, power-button it."
if [ -c /dev/tty1 ]; then
	echo "gpu-unlock: suspending in 8s for a rate cycle. Wake with a key or the power button." > /dev/tty1
fi
sleep 8
echo mem > /sys/power/state
echo "# resumed $(date -Is) rc=$?"
sleep 3
echo "# bank after"
bank
echo "# rate after"
rate after
echo "# done"
