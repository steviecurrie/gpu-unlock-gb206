#!/bin/bash
# One FLR-window write, then a registry-key reload if the write did not stick.
# Logs to a file, restores the nvidia module, and reboots only if the hammer hangs.
# Display will die. nvidia is not in the initramfs, so a reboot binds the stock module.
set -u
cd "$(dirname "$0")"
LOG="${LOG:-$PWD/flr-window-$(date +%Y%m%d-%H%M%S).log}"
GPU="${GPU:-0000:08:00.0}"
exec > >(tee -a "$LOG") 2>&1
echo "# start $(date -Is) log=$LOG"

if [ "$(id -u)" -ne 0 ]; then
	echo "need root"
	exit 1
fi

# Detach before killing X, or SIGHUP kills the run mid-reset.
if [ -z "${INVOCATION_ID:-}" ] && [ -z "${FLR_DETACHED:-}" ]; then
	export FLR_DETACHED=1
	exec systemd-run --collect --unit=gb206-flr --working-directory="$PWD" \
		/bin/bash "$PWD/flr_run.sh"
fi

restore() {
	if lsmod | grep -q '^nvidia '; then
		echo "# nvidia already bound, leave it"
	else
		echo "# restore nvidia"
		modprobe nvidia 2>&1 || true
		modprobe nvidia_modeset 2>&1 || true
		modprobe nvidia_drm 2>&1 || true
		modprobe nvidia_uvm 2>&1 || true
	fi
	echo "# nvidia modules: $(lsmod | grep -c '^nvidia' || true)"
	if [ -c /dev/tty1 ]; then
		echo "gpu-unlock: experiment finished. log=$LOG  — startx to get the display back" > /dev/tty1 || true
	fi
}
trap restore EXIT

echo "# stop clients"
systemctl stop nvidia-persistenced ollama 2>/dev/null || true
/usr/bin/killall -q Xorg xinit i3 nvtop 2>/dev/null || true
sleep 1
# Whatever still holds the device goes. Display dies here.
/usr/bin/fuser -k /dev/nvidia0 /dev/nvidiactl /dev/nvidia-modeset /dev/dri/card0 2>/dev/null || true
sleep 1

echo "# unload nvidia"
modprobe -r nvidia_uvm nvidia_drm nvidia_modeset nvidia 2>&1 || {
	echo "# rmmod failed, still holding:"
	lsmod | grep nvidia || true
	fuser -v /dev/nvidia* /dev/dri/card0 2>&1 || true
	exit 1
}

# Memory enable, in case unbind cleared it. COMMAND is config byte 4.
setpci -s "$GPU" COMMAND=0x06
echo "# command $(setpci -s "$GPU" COMMAND)"

if [ ! -x ./flr_hammer ]; then
	echo "missing ./flr_hammer"
	exit 1
fi

echo "# hammer"
set +e
timeout -k 5 20 ./flr_hammer \
	"/sys/bus/pci/devices/$GPU/resource0" \
	"/sys/bus/pci/devices/$GPU/reset"
rc=$?
set -e
echo "# hammer rc=$rc"
sync

if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
	echo "# hammer hung, reboot"
	sync
	reboot
	exit 0
fi

echo "# bind stock and peek"
modprobe nvidia
sleep 2
if [ -f ./gpu_peek_gb206.ko ]; then
	insmod ./gpu_peek_gb206.ko "target=$GPU" || true
	dmesg | grep -F 'gpu_peek_gb206' | tail -40
	rmmod gpu_peek_gb206 2>/dev/null || true
fi

if [ "$rc" -eq 0 ]; then
	echo "# write stuck across the window; leaving stock driver bound for a rate check"
	exit 0
fi

echo "# window write did not stick; registry reload"
modprobe -r nvidia_uvm nvidia_drm nvidia_modeset nvidia || true
modprobe nvidia NVreg_RegistryDwords="RMOverrideSmSpeedSelect=0x88888888;RMOverrideSmSpeedSelect1=0x8;RMDisableFeatureDisablement=1"
sleep 3
echo "# registry param: $(cat /sys/module/nvidia/parameters/NVreg_RegistryDwords 2>/dev/null)"
if [ -f ./gpu_peek_gb206.ko ]; then
	insmod ./gpu_peek_gb206.ko "target=$GPU" || true
	dmesg | grep -F 'gpu_peek_gb206' | tail -20
	rmmod gpu_peek_gb206 2>/dev/null || true
fi
nvidia-smi -L 2>&1 || true
echo "# done $(date -Is)"
