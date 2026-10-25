# Adreno 512 bring-up tools

Tools used to bring up the a9y18qlte's Adreno 512 from the probe shim
(`tools/a9y/shim`, `gpu_probe()` and onwards), run from Android:

- `smmudump/`: kernel module that prints the GPU SMMU (0x5040000), GPU clock
  controller and GCC GPU clocks as kgsl leaves them. Load it with the GPU forced
  on (`/sys/class/kgsl/kgsl-3d0/force_{rail,clk,bus}_on`). Only reads registers
  that the clock drivers use: others, like GPUCC +0x28, abort and panic.
- `tbutest/`: kernel module that replays the shim's GPU power-up on a GPU that
  kgsl has collapsed, then tries an SMMU TLB sync.
- `kgsltest/`: freestanding (no libc) program that submits the shim's packet
  streams through `/dev/kgsl-3d0`, to tell a broken packet stream from a broken
  GPU init. The draw context needs `KGSL_CONTEXT_PREAMBLE | NO_GMEM_ALLOC`.

Modules build against the LineageOS 4.19 kernel's build output (the CRCs come
from its `Module.symvers`):

    make -C <kernel>/ O=out ARCH=arm64 CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm \
        OBJCOPY=llvm-objcopy CLANG_TRIPLE=aarch64-linux-gnu- \
        CROSS_COMPILE=aarch64-linux-android- M=$PWD modules

    clang --target=aarch64-linux-gnu -O2 -static -nostdlib -ffreestanding \
        -fno-builtin -fno-stack-protector -fno-pic -fuse-ld=lld -o kgsltest kgsltest.c

The shim's `build.sh` packs the GPU firmware (`a530_pm4.fw`, `a530_pfp.fw`,
`a512_zap.*`) from the phone's `/vendor/firmware` into the boot image; the
firmware is not in this repository.
