#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read-only GC573 diagnostics. No privileged commands or device changes.
set -eu

vendor=0x1461
device=0x0054
found=0

printf 'Kernel: %s\n' "$(uname -r)"
if [ -r /etc/os-release ]; then
	. /etc/os-release
	printf 'OS: %s\n' "${PRETTY_NAME:-unknown}"
fi

if command -v mokutil >/dev/null 2>&1; then
	printf 'Secure Boot: '
	mokutil --sb-state 2>&1 || true
else
	printf 'Secure Boot: unknown (mokutil is not installed)\n'
fi

for pci in /sys/bus/pci/devices/*; do
	[ -r "$pci/vendor" ] && [ -r "$pci/device" ] || continue
	[ "$(cat "$pci/vendor")" = "$vendor" ] || continue
	[ "$(cat "$pci/device")" = "$device" ] || continue
	found=1
	bdf=${pci##*/}
	printf '\nGC573 PCI device: %s (%s:%s)\n' "$bdf" "$vendor" "$device"
	if [ -L "$pci/driver" ]; then
		printf 'Bound driver: %s\n' "$(basename "$(readlink -f "$pci/driver")")"
	else
		printf 'Bound driver: none\n'
	fi

	device_path=$(readlink -f "$pci")
	video_found=0
	for node in /sys/class/video4linux/video*; do
		[ -e "$node/device" ] || continue
		video_path=$(readlink -f "$node/device")
		case "$video_path" in
			"$device_path"|"$device_path"/*)
				video_found=1
				printf 'Video node: /dev/%s\n' "${node##*/}"
				;;
		esac
	done
	[ "$video_found" -eq 1 ] || printf 'Video nodes: none associated in sysfs\n'
done

if [ "$found" -eq 0 ]; then
	printf '\nGC573 PCI device %s:%s not found in sysfs\n' "$vendor" "$device"
fi

if command -v lspci >/dev/null 2>&1; then
	printf '\nPCI driver details (lspci -Dnnk):\n'
	lspci -Dnnk -d 1461:0054 || true
fi
