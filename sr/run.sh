#!/bin/bash
# Arm sr_poke and suspend once. Wake the box to resume. A hang means reboot.
set -u
cd "$(dirname "$0")"
FIELD="${FIELD:-flags}"
VAL="${VAL:-2}"
LOG="${LOG:-$PWD/sr-${FIELD}-$(date +%Y%m%d-%H%M%S).log}"
exec > >(tee -a "$LOG") 2>&1
echo "# start $(date -Is) field=$FIELD val=$VAL"

if [ "$(id -u)" -ne 0 ]; then
	echo "need root"
	exit 1
fi

SYM=$(awk '$3 == "kgspPrepareSuspendResumeData_TU102" {print $1; exit}' /proc/kallsyms)
if [ -z "$SYM" ]; then
	echo "symbol missing"
	exit 1
fi
ADDR=$(printf '%#x' $((0x$SYM + 0x16b)))
echo "# probe $ADDR (sym $SYM)"

insmod ./sr_poke.ko "addr=$ADDR" "field=$FIELD" "val=$VAL" || {
	dmesg | grep sr_poke | tail -5
	exit 1
}

echo "# armed. suspending in 8s. wake with the power button."
if [ -c /dev/tty1 ]; then
	echo "gpu-unlock: suspending in 8s ($FIELD=$VAL). Wake with the power button." > /dev/tty1
fi
sync
sleep 8
echo mem > /sys/power/state
echo "# resumed $(date -Is) rc=$?"
dmesg | grep sr_poke | tail -8
rmmod sr_poke 2>/dev/null || true
echo "# done"
