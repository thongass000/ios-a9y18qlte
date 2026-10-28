# Capturing a Mesa FD512 submit for a9yreplay

Mesa 25.0.7 (archive.mesa3d.org tarball) with `mesa-25.0.7-noop-a512.patch`: the noop drm-shim learns the
A512 (chip 0x05010200) and hands out GPU addresses inside the phone's first SweetAccelerator v2 window
(0x10_0000_0000), so the capture replays without relocation.

```sh
meson setup build -Dgallium-drivers=freedreno -Dvulkan-drivers= -Dplatforms= -Degl=enabled -Dgbm=disabled \
    -Dglx=disabled -Dllvm=disabled -Dtools=drm-shim,freedreno -Dgles2=enabled -Dprefix=$HOME/a9-ios/mesa-inst
ninja -C build install
gcc -O2 -o tri tri.c -lEGL -lGLESv2
L=$HOME/a9-ios/mesa-inst/lib/x86_64-linux-gnu
__EGL_VENDOR_LIBRARY_FILENAMES=$HOME/a9-ios/mesa-inst/share/glvnd/egl_vendor.d/50_mesa.json \
    LD_LIBRARY_PATH=$L LD_PRELOAD=$L/libfreedreno_noop_drm_shim.so \
    FD_GPU_ID=512 FD_RD_DUMP=full,combine FD_MESA_DEBUG=sysmem ./tri    # -> /tmp/tri_combined.rd.gz
build/src/freedreno/decode/cffdump --no-color /tmp/tri_combined.rd.gz    # find RB_MRT[0].BASE
python3 rd2c.py /tmp/tri_combined.rd.gz RT_IOVA 64 64 256 > ../capture.h
```

## The kernel's present template (fit.c)

`fit.c` is the panel present as one draw: iOS's 1125x2436 frame (4608-byte rows) scaled into the 1026x2220
centre of the 1080x2220 panel (4352-byte rows). Its capture becomes `xnu/bsd/dev/sweet_a5xx_fit.h`
(in patch 0125), which `sweet_a5xx.c a5_fit_ib` relocates into the GPU pool:

```sh
gcc -O2 -o fit fit.c -lEGL -lGLESv2
... FD_GPU_ID=512 FD_RD_DUMP=full,combine FD_MESA_DEBUG=sysmem,notile ./fit    # -> /tmp/fit_combined.rd.gz
cffdump --no-color /tmp/fit_combined.rd.gz | grep -e 'TEX_CONST' -A4 -e 'RB_MRT\[0\].BASE'   # source, target
python3 rd2tmpl.py /tmp/fit_combined.rd.gz SRC_IOVA DST_IOVA > sweet_a5xx_fit.h
```
