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

# Write an image to the boot partition and check it before going on. Nothing is written when the partition
# already holds it.
write_boot() {
	local img=$1 md5 n got
	md5=$(md5sum < "$img" | cut -d' ' -f1)
	n=$(stat -c %s "$img")
	as_root || return 1
	got=$(adb shell "head -c $n $BOOT | md5sum" | cut -d' ' -f1)
	[ "$got" = "$md5" ] && { echo "boot = $(basename "$img") ($md5), already"; return 0; }
	adb push "$img" /data/local/tmp/boot-write.img >/dev/null || return 1
	adb shell "dd if=/data/local/tmp/boot-write.img of=$BOOT bs=1M conv=fsync 2>/dev/null; sync; rm /data/local/tmp/boot-write.img" || return 1
	as_root || return 1	# the readback after a fresh adb root: an empty answer was a false mismatch
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
if [ "${ONESHOT:-0}" = 1 ]; then
	# The shim boots XNU once, no Vol Down (shim.c rtc_requested): the PMIC RTC survives into the shim (the
	# PMIC spare and IMEM restart reason come back cleared, ramoops is overwritten), so the request is an
	# alarm 0x40000000 s ahead, honoured while it is under 120 s old. The shim sees it ~40 s after this;
	# XNU's try lasts 300 s, so the boot after it is Linux again. A whole run is ~10 min.
	as_root || exit 1
	adb shell 'R=/sys/class/rtc/rtc0; echo 0 > $R/wakealarm; echo $(( $(cat $R/since_epoch) + 1073741824 )) > $R/wakealarm && cat $R/wakealarm' ||
		{ echo "RTC request failed" >&2; exit 1; }
	echo "one-shot XNU request set (RTC alarm): rebooting into the shim"
	adb reboot
else
	echo "rebooting into the shim${REBOOT_ARG:+ (reboot $REBOOT_ARG)}"
	adb reboot ${REBOOT_ARG:-}	# REBOOT_ARG: a reboot reason for msm-poweroff
fi
sleep 15
if ! wait_android && [ "$(adb get-state 2>/dev/null | tr -d '\r')" = device ]; then
	# Up but never boot_completed: after XNU the touch controller once stayed hung (bt532 probe failed), the
	# LineageOS touch HAL never registered and system_server's watchdog looped. A reboot resets it.
	echo "Android up but not boot_completed: rebooting it once (touch controller left hung?)" >&2
	adb shell 'dmesg 2>/dev/null | grep -i "bt532.*probe"' >&2
	adb reboot
	sleep 15
fi
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
	[ "${ONESHOT:-0}" = 1 ] && adb shell 'echo 0 > /sys/class/rtc/rtc0/wakealarm'	# the request is spent
	if [ "${KEEP:-0}" = 1 ]; then	# unattended series: the shim image stays (it boots Linux unless asked)
		echo "boot partition keeps $(basename "$IMG") (KEEP=1); tools/run-probe.sh --restore puts LOS back"
	else
		write_boot "$LOS" && echo "LOS boot restored"
	fi
else
	echo "Android is up but adb root failed: run tools/run-probe.sh --restore" >&2
	exit 1
fi
