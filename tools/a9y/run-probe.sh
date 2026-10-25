#!/bin/bash
# Boot a chain-loading shim image once from the boot partition and collect
# its log, then put the LOS boot image back.
#   tools/run-probe.sh [shim image]     default shim/out/shim-boot.img
# The shim runs, then boots the wrapped LOS kernel; Android shows the shim's
# log as /sys/fs/pstore/console-ramoops-0. Logs land in logs/probe-<time>.txt.
# If Android doesn't come back: Power + Vol Up + Bixby -> OrangeFox, then
# run this with --restore.
set -uo pipefail
cd "$(dirname "$0")/.."
IMG=${1:-shim/out/shim-boot.img}
LOS=backup/boot-lineage-23.2-20261024.img
LOS_MD5=c79dba439301e133006451d9f8d1c315
BOOT=/dev/block/by-name/boot
mkdir -p logs
LOG=logs/probe-$(date +%Y%m%d-%H%M%S).txt

[ "$(md5sum < "$LOS" | cut -d' ' -f1)" = "$LOS_MD5" ] || { echo "LOS boot backup md5 mismatch" >&2; exit 1; }

as_root() {
	local i
	for i in $(seq 1 10); do
		adb wait-for-device && adb root >/dev/null 2>&1
		sleep 3
		adb wait-for-device
		[ "$(adb shell id -u 2>/dev/null | tr -d '\r')" = 0 ] && return 0
	done
	echo "no root adb" >&2
	return 1
}

# Write an image to the boot partition and check it before going on.
write_boot() {
	local img=$1 md5 n got
	md5=$(md5sum < "$img" | cut -d' ' -f1)
	n=$(stat -c %s "$img")
	as_root || return 1
	adb push "$img" /data/local/tmp/boot-write.img >/dev/null || return 1
	adb shell "dd if=/data/local/tmp/boot-write.img of=$BOOT bs=1M conv=fsync 2>/dev/null; sync; rm /data/local/tmp/boot-write.img" || return 1
	got=$(adb shell "head -c $n $BOOT | md5sum" | cut -d' ' -f1)
	[ "$got" = "$md5" ] || { echo "boot readback $got != $md5" >&2; return 1; }
	echo "boot = $(basename "$img") ($md5)"
}

wait_android() {
	local i
	for i in $(seq 1 $(( ${WAIT_MIN:-10} * 30 ))); do	# WAIT_MIN (10): an XNU try runs until the watchdog resets it
		[ "$(adb get-state 2>/dev/null | tr -d '\r')" = device ] &&
			[ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && return 0
		sleep 2
	done
	return 1
}

if [ "$IMG" = --restore ]; then
	write_boot "$LOS" && echo "LOS boot restored"
	exit
fi

write_boot "$IMG" || exit 1
echo "rebooting into the shim"
adb reboot
sleep 15
if ! wait_android; then
	echo "Android did not come back within ${WAIT_MIN:-10} min: Power + Vol Up + Bixby -> OrangeFox," >&2
	echo "then run tools/run-probe.sh --restore" >&2
	exit 1
fi
if as_root; then
	{
		echo "# shim $(md5sum < "$IMG" | cut -d' ' -f1)  $(date)"
		adb shell 'uname -v; ls -la /sys/fs/pstore; cat /sys/fs/pstore/console-ramoops-0 2>/dev/null || echo "(no console-ramoops-0)"'
	} > "$LOG"
	cat "$LOG"
	write_boot "$LOS" && echo "LOS boot restored"
else
	echo "Android is up but adb root failed: run tools/run-probe.sh --restore" >&2
	exit 1
fi
