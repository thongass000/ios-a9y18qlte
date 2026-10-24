#!/bin/bash
# Build the a9y18qlte iOS root image in WSL: the iPhone X iOS 16.7.16 (20H392)
# userland in a case-sensitive HFS+ image, as ios-flame's mkiosroot.sh +
# sweet-root-tweaks.py make it.
#
#   tools/build-ios-root.sh          (run it yourself: it uses sudo, and
#                                     clones/builds outside code)
#
# WSL's kernel has no HFS+ and Ubuntu 22.04 has no apfs-fuse package, so it
# first builds both into ~/a9-ios/hostbuild:
#   - hfsplus.ko from Microsoft's source for the running kernel (same tag and
#     /proc/config.gz unchanged but for HFSPLUS_FS=m, so the module's symbol
#     CRCs match: BTF must stay on, it adds fields to struct module and so
#     changes module_layout; pahole from dwarves builds it), checked against a
#     shipped module, then insmod'ed (again after every WSL restart:
#     sudo insmod hostbuild/hfsplus.ko)
#   - apfs-fuse (sgan81/apfs-fuse)
# then fetches only the three DMGs from Apple's CDN (tools/remotezip.py) and
# builds hostbuild/ios-root.img (16 GiB: the root file on the phone is 16 GiB
# plus the 16 MiB console log area). Each step is skipped when its output exists.
set -euo pipefail
A9=$HOME/a9-ios
FLAME=$HOME/ios-flame-src/fork
W=$A9/hostbuild
IPSW=https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77006/1158E502-D76D-4226-AA06-ED5E30CD1111/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw
OS_DMG=087-86698-044.dmg SYSOS_DMG=087-86593-047.dmg APPOS_DMG=087-86715-047.dmg
mkdir -p "$W"

echo "== packages"
sudo apt install -y git cmake g++ flex bison bc libelf-dev libssl-dev dwarves \
	libfuse3-dev libbz2-dev zlib1g-dev libattr1-dev hfsprogs rsync

echo "== hfsplus.ko for $(uname -r)"
KVER=$(uname -r)
if [ ! -f "$W/hfsplus.ko" ]; then
	[ -d "$W/kernel" ] || git clone --depth 1 --branch "linux-msft-wsl-${KVER%%-*}" \
		https://github.com/microsoft/WSL2-Linux-Kernel.git "$W/kernel"
	cd "$W/kernel"
	zcat /proc/config.gz > .config
	scripts/config --module HFSPLUS_FS
	make olddefconfig >/dev/null
	grep -q '^CONFIG_DEBUG_INFO_BTF=y' .config || { echo "BTF dropped from .config (pahole missing?)" >&2; exit 1; }
	rel=$(make -s kernelrelease LOCALVERSION=)
	[ "$rel" = "$KVER" ] || { echo "kernelrelease $rel != $KVER" >&2; exit 1; }
	make -j"$(nproc)" LOCALVERSION=
	# the CRCs it imports must equal those a shipped module of this kernel imports
	# (find fails on the unreadable lost+found there: not an error)
	ref=$(find "/lib/modules/$KVER/kernel" -name '*.ko' 2>/dev/null | head -1 || true)
	[ -n "$ref" ] || { echo "no shipped module to check CRCs against" >&2; exit 1; }
	bad=$(join <(/sbin/modprobe --dump-modversions fs/hfsplus/hfsplus.ko | awk '{print $2, $1}' | sort) \
		<(/sbin/modprobe --dump-modversions "$ref" | awk '{print $2, $1}' | sort) | awk '$2 != $3' | wc -l)
	[ "$bad" = 0 ] || { echo "symbol CRCs differ from $ref: not loading" >&2; exit 1; }
	cp fs/hfsplus/hfsplus.ko "$W/"
	cd "$A9"
fi
grep -qw hfsplus /proc/filesystems || sudo insmod "$W/hfsplus.ko"
grep -w hfsplus /proc/filesystems

echo "== apfs-fuse"
if [ ! -x "$W/apfs-fuse/build/apfs-fuse" ]; then
	[ -d "$W/apfs-fuse" ] || git clone --recursive https://github.com/sgan81/apfs-fuse.git "$W/apfs-fuse"
	cmake -S "$W/apfs-fuse" -B "$W/apfs-fuse/build" -DCMAKE_BUILD_TYPE=Release
	make -C "$W/apfs-fuse/build" -j"$(nproc)"
fi
export PATH=$W/apfs-fuse/build:$PATH

echo "== IPSW DMGs"
mkdir -p "$W/ipsw"
for d in "$OS_DMG" "$SYSOS_DMG" "$APPOS_DMG"; do
	[ -f "$W/ipsw/$d" ] || python3 -I "$FLAME/tools/remotezip.py" "$IPSW" "$d" "$W/ipsw"
done
ls -l "$W/ipsw"

echo "== root image"
IMG=$W/ios-root.img
# Linux hfsplus leaves the free block count and bitmap slightly off and
# composes some names differently from Apple: one fsck -y pass may leave work
# for the next (mkiosroot.sh's own final fsck -n then fails), so repair until clean.
repair() {
	for i in 1 2 3; do
		fsck.hfsplus -f -y "$1" | grep -v 'Illegal name' || true
		fsck.hfsplus -f -n "$1" >/dev/null && return 0
	done
	fsck.hfsplus -f -n "$1"
}
if [ ! -f "$IMG" ]; then
	"$FLAME/tools/mkiosroot.sh" "$W/ipsw/$OS_DMG" "$W/ipsw/$SYSOS_DMG" "$W/ipsw/$APPOS_DMG" "$IMG.tmp" 16G ||
		echo "mkiosroot.sh's final check failed: repairing"
	repair "$IMG.tmp"
	mnt=$(mktemp -d)
	sudo mount -t hfsplus -o loop,force "$IMG.tmp" "$mnt"
	sudo python3 -I "$FLAME/tools/sweet-root-tweaks.py" "$mnt" || { sudo umount "$mnt"; exit 1; }
	sudo umount "$mnt"
	rmdir "$mnt"
	repair "$IMG.tmp"
	mv "$IMG.tmp" "$IMG"
fi
ls -ls "$IMG"
echo "done: $IMG"
