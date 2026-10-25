#!/bin/bash
# Build the a9y18qlte probe shim as a boot image that chain-loads Linux or XNU.
#
#   shim/build.sh [BOOT_IMG] [OUT]
#     BOOT_IMG: boot image whose kernel, ramdisk and cmdline are wrapped
#               (default: the LOS 23.2 boot partition backup)
#     OUT:      default out/shim-boot.img
#   shim/build.sh --xnu PAYLOAD [BOOT_IMG] [OUT]
#     PAYLOAD:  tools/mkpayload.py output (kernel + ADT)
#     OUT:      default out/shim-xnu.img
#
# Linux: kernel blob = gzip(shim | zero pad to 2 MiB | Linux Image) + the
# stock DTB. The shim header's image_size covers the whole blob plus Linux's
# bss and 2 MiB of slack, so the bootloader puts the DTB and ramdisk after
# it; res2 holds the Linux file size. Header v0 with the LOS offsets.
# XNU: the same, with the payload behind the LOS ramdisk (4 KiB aligned);
# res3 = its offset in the ramdisk, res4 = the LOS ramdisk size. The shim
# boots XNU once, and Linux after the watchdog reset that ends the try.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)
XNU=""
if [ "${1:-}" = --xnu ]; then
	XNU=$(realpath "${2:?--xnu needs the payload}")
	shift 2
	DEF_OUT=out/shim-xnu.img
else
	DEF_OUT=out/shim-boot.img
fi
BOOT=${1:-$ROOT/backup/boot-lineage-23.2-20261024.img}
OUT=${2:-$DEF_OUT}
LLVM=$HOME/fox_12.1/prebuilts/clang/host/linux-x86/clang-r416183b/bin
MKBOOTIMG=$HOME/fox_12.1/system/tools/mkbootimg
DTB=$ROOT/backup/split/dtb0.dtb
PAYLOAD_OFF=$((0x200000))
CC="$LLVM/clang --target=aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
	-fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra ${SHIM_CFLAGS:-}"
mkdir -p out
$CC $CFLAGS -c start.S -o out/start.o
$CC $CFLAGS -c shim.c -o out/shim.o
# Runs wherever the bootloader puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$($LLVM/llvm-readelf -r out/shim.o | grep -c R_AARCH64_ABS64 || true)
[ "$abs" = 0 ] || { $LLVM/llvm-readelf -r out/shim.o | grep -B5 R_AARCH64_ABS64 >&2
	echo "error: $abs absolute relocations in shim.o" >&2; exit 1; }
$LLVM/ld.lld -static -nostdlib -T shim.ld --build-id=none -N out/start.o out/shim.o -o out/shim.elf
$LLVM/llvm-objcopy -O binary out/shim.elf out/Image
end=$((16#$($LLVM/llvm-nm out/shim.elf | awk '$3=="_end"{print $1}')))
[ $end -le $((0x100000)) ] || { echo "error: shim ($end bytes) overruns the GPU firmware at 1 MiB" >&2; exit 1; }

# Linux kernel, ramdisk and cmdline from the boot image
rm -rf out/payload && mkdir -p out/payload
python3 -I "$MKBOOTIMG/unpack_bootimg.py" --boot_img "$BOOT" --out out/payload > out/payload/info.txt
CMDLINE=$(sed -n 's/^command line args: //p' out/payload/info.txt)
python3 -I "$ROOT/tools/split_gz_dtb.py" out/payload/kernel out/payload > /dev/null
LINUX=out/payload/Image
lsize=$(stat -c %s $LINUX)
lisz=$(od -An -tu8 -j16 -N8 $LINUX | tr -d ' ')
reserved=$((PAYLOAD_OFF + lisz + 0x200000))

truncate -s $PAYLOAD_OFF out/Image
# GPU firmware for the shim's GPU probe in the gap at 1 MiB (shim.c GPUFW_OFF):
# "A9YGPUFW", u32 count, {name[16], u32 offset, u32 size} each, data 4 KiB aligned
GPUFW=$ROOT/gpufw
if [ -d "$GPUFW" ]; then
	python3 -I - "$GPUFW" out/gpufw.bin a530_pm4.fw a530_pfp.fw a512_zap.mdt a512_zap.b00 \
		a512_zap.b01 a512_zap.b02 <<'EOF'
import struct, sys, os
d, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
hdr = 8 + 4 + 24 * len(names)
off = (hdr + 0xfff) & ~0xfff
ents, blobs = [], b''
for n in names:
    data = open(os.path.join(d, n), 'rb').read()
    ents.append(struct.pack('<16sII', n.encode(), off + len(blobs), len(data)))
    blobs += data + bytes(-len(data) % 0x1000)
blob = b'A9YGPUFW' + struct.pack('<I', len(names)) + b''.join(ents)
open(out, 'wb').write(blob + bytes(off - len(blob)) + blobs)
EOF
	[ $(stat -c %s out/gpufw.bin) -le $((PAYLOAD_OFF - 0x100000)) ] || { echo "gpufw too big" >&2; exit 1; }
	dd if=out/gpufw.bin of=out/Image bs=1M seek=1 conv=notrunc status=none
fi
cat $LINUX >> out/Image
# little-endian u64 at an offset of out/Image
put64() { printf "$(printf '%016x' $2 | sed -E 's/(..)(..)(..)(..)(..)(..)(..)(..)/\\x\8\\x\7\\x\6\\x\5\\x\4\\x\3\\x\2\\x\1/')" |
	dd of=out/Image bs=1 seek=$1 conv=notrunc status=none; }
put64 16 $reserved	# image_size
put64 32 $lsize		# res2: Linux file size
RAMDISK=out/payload/ramdisk
if [ -n "$XNU" ]; then
	rdsize=$(stat -c %s $RAMDISK)
	xoff=$(((rdsize + 0xfff) & ~0xfff))
	cp $RAMDISK out/ramdisk-xnu
	truncate -s $xoff out/ramdisk-xnu
	cat "$XNU" >> out/ramdisk-xnu
	RAMDISK=out/ramdisk-xnu
	put64 40 $xoff		# res3: XNU payload offset in the ramdisk
	put64 48 $rdsize	# res4: LOS ramdisk size
	printf 'LOS ramdisk %d bytes, XNU payload %d bytes at %#x\n' $rdsize $(stat -c %s "$XNU") $xoff
fi
gzip -9 -n -c out/Image > out/Image.gz
cat out/Image.gz "$DTB" > out/Image.gz-dtb

python3 -I "$MKBOOTIMG/mkbootimg.py" --header_version 0 --kernel out/Image.gz-dtb \
	--ramdisk $RAMDISK --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x2000000 \
	--second_offset 0xf00000 --tags_offset 0x1e00000 --pagesize 4096 --os_version 16.0.0 \
	--os_patch_level 2026-10 --cmdline "$CMDLINE" -o "$OUT"
printf 'shim %d bytes, Linux %d bytes (image_size %#x), reserved %#x\n' $end $lsize $lisz $reserved
ls -l "$OUT"
