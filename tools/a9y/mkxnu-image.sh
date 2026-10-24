#!/bin/bash
# Build the a9y18qlte XNU test boot image (Vol Down at the shim screen boots XNU).
#   tools/mkxnu-image.sh KERNEL OUT.img [EXTRA BOOT-ARGS]
# ADT with the root file's extent map (logs/ios-root-extents.txt), root on
# it (rd=ufsroot0), console log to disk, and sweetrtcoff = now - PM660 RTC,
# read from Android (it keeps the same offset), so XNU gets the time of day.
set -euo pipefail
A9=$(cd "$(dirname "$0")/.." && pwd)
FLAME=$HOME/ios-flame-src/fork/tools
K=$(realpath "$1")
OUT=$(realpath -m "$2")
EXTRA=${3:-}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

rtc=$(adb shell cat /sys/class/rtc/rtc0/since_epoch | tr -d '\r')
off=$(( $(date -u +%s) - rtc ))
args="debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 flamereboot=120"
args="$args rd=ufsroot0 ufsroot=userdata ufsrw=1 rootrw=1 ufslogsecs=2 sweetrtcoff=$off"
# the iOS userland on a macOS-target xnu, as ios-flame's flame-run.sh boots it
args="$args unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 $EXTRA"

python3 -I "$FLAME/mkadt_a9y18qlte.py" "$W/phone.adt" --root-runs "$A9/logs/ios-root-extents.txt" 2>&1 |
	grep 'root file'
python3 -I "$FLAME/mkpayload.py" "$K" "$W/phone.adt" "$W/payload.bin" --cmdline "$args" > /dev/null
"$A9/shim/build.sh" --xnu "$W/payload.bin" "$A9/backup/boot-lineage-23.2-20261024.img" "$OUT" | tail -1
echo "boot-args: $args"
