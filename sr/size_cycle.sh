#!/bin/bash
# One SR-descriptor size poke (stock size halved) across one deep S3 suspend.
# Rate and bank are read before and after. Logs are written as it goes.
set -u
cd "$(dirname "$0")/.."
FIELD="${FIELD:-size}"
VAL="${VAL:-0}"
TAG="${TAG:-$(date +%Y%m%d-%H%M%S)-$FIELD$VAL}"
LOG="$PWD/sr/size-cycle-$TAG.log"
exec > >(tee -a "$LOG") 2>&1
echo "# start $(date -Is) field=$FIELD val=$VAL mem_sleep=$(cat /sys/power/mem_sleep)"

if [ "$(id -u)" -ne 0 ]; then
	echo "need root"
	exit 1
fi

PY="$PWD/.venv/bin/python"
rate() {
	runuser -u kuzz -- env CUDA_DEVICE_ORDER=PCI_BUS_ID CUDA_VISIBLE_DEVICES=0 "$PY" probe/mma_rate.py --json "sr/rate-$TAG-$1.json" 2>/dev/null \
		| grep -E 'x fp16->fp16' | grep -E 'fp16_acc_fp32|bf16_acc_fp32'
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

SYM=$(awk '$3 == "kgspPrepareSuspendResumeData_TU102" {print $1}' /proc/kallsyms)
PROBE=$(python3 -c "print(hex(int('$SYM',16)+0x16b))")
echo "# arm sr_poke $FIELD=$VAL at $PROBE"
insmod ./sr/sr_poke.ko "addr=$PROBE" "field=$FIELD" "val=$VAL" || { dmesg | grep sr_poke | tail -3; exit 1; }

sync
echo "# suspending in 8s (deep). if it does not come back, power-button it."
if [ -c /dev/tty1 ]; then
	echo "gpu-unlock: size-field suspend in 8s. Wake with a key or the power button." > /dev/tty1
fi
sleep 8
echo mem > /sys/power/state
echo "# resumed $(date -Is) rc=$?"
sleep 3
dmesg | grep sr_poke | tail -3
rmmod sr_poke 2>/dev/null || true
echo "# bank after"
bank
echo "# rate after"
rate after
echo "# done"
