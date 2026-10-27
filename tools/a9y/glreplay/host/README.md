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
