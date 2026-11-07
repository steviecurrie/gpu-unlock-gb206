#!/bin/bash
# Control: suspend with no probe armed, read the speed-select bank before and after.
set -u
cd "$(dirname "$0")/.."
LOG="${LOG:-$PWD/sr/control-$(date +%Y%m%d-%H%M%S).log}"
exec > >(tee -a "$LOG") 2>&1
echo "# start $(date -Is) mem_sleep=$(cat /sys/power/mem_sleep)"

if [ "$(id -u)" -ne 0 ]; then
	echo "need root"
	exit 1
fi

peek() {
	insmod ./gpu_peek_gb206.ko target=0000:08:00.0 || return 1
	sleep 1
	dmesg | grep -E 'SS0 \(|SS1 \(|FEAT_OVR_PLM = |SS0 nibbles' | tail -4
	rmmod gpu_peek_gb206
}

echo "# before"
peek
sync
echo "# suspending in 8s, wake with a key"
sleep 8
echo mem > /sys/power/state
echo "# resumed $(date -Is) rc=$?"
sleep 3
echo "# after"
peek
echo "# done"
