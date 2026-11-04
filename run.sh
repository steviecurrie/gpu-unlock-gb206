#!/usr/bin/env bash
# Load gpu_peek_gb206, capture its output, unload it. Needs root.
#
#   sudo ./run.sh              # read-only
#   sudo MODE=write ./run.sh   # read-only + the three-write ladder (volatile)
#
# Every write the ladder makes is undone before the module unloads, and a
# reboot or power-cycle clears anything left over.
set -uo pipefail
cd "$(dirname "$0")" || exit 1

GPU="${GPU:-0000:08:00.0}"
MODE="${MODE:-read}"
OUT="${OUT:-gb206-$(date +%Y%m%d-%H%M%S)-$MODE.log}"
KO="./gpu_peek_gb206.ko"
# insmod/rmmod live in /usr/sbin and are not always on a normal user's PATH.
INSMOD="${INSMOD:-$(command -v insmod || echo /usr/sbin/insmod)}"
RMMOD="${RMMOD:-$(command -v rmmod || echo /usr/sbin/rmmod)}"

if [ ! -f "$KO" ]; then
	echo "missing $KO — run 'make' first (as your normal user)" >&2
	exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
	echo "run me with sudo" >&2
	exit 1
fi
if lsmod | grep -q '^gpu_peek_gb206 '; then
	echo "gpu_peek_gb206 already loaded; rmmod it first" >&2
	exit 1
fi

args=("target=$GPU")
[ "$MODE" = write ] && args+=(writetest=1)

"$INSMOD" "$KO" "${args[@]}"
rc=$?

{
	echo "# gpu_peek_gb206"
	uname -a
	echo "# card: $(lspci -nn -s "$GPU" 2>/dev/null)"
	echo "# driver: $(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null)"
	echo "# mode: $MODE"
} > "$OUT"
dmesg 2>/dev/null | grep -F 'gpu_peek_gb206' >> "$OUT"

"$RMMOD" gpu_peek_gb206 2>/dev/null

cat "$OUT"
echo
echo "saved to $OUT (insmod rc=$rc)"
