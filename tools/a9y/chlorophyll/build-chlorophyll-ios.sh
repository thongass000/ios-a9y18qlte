#!/bin/bash
# Chlorophyll (github.com/ry-diffusion/Chlorophyll, MIT: Metal over Mesa freedreno, AIR -> NIR -> ir3) for iOS 16 arm64 on
# the A512, after its build-ios.sh and build-probe.sh: a second Mesa 25.0.7 tree with Bergamota's patch, ours and
# Chlorophyll's (not fd6-fb-barrier-reorder: A6xx only), its air/ front end linked in; libbgair.dylib, BGMTLDevice
# (src/bgmtl.m) and the compute and render probes with their metallibs (AIR compiled here by Xcode).
#   WORK=/same/dir/as/build-mesa-ios.sh tools/a9y/chlorophyll/build-chlorophyll-ios.sh   (run build-mesa-ios.sh first)
#   -> $WORK/out/chl: everything for /usr/local/chl on the phone (the probes find libbgair.dylib beside them; the
#      libdrm stand-in stays /usr/local/lib/libdrm.2.dylib)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MESA="$HERE/../mesa"
WORK="${WORK:?set WORK}"
CHL_REPO=https://github.com/ry-diffusion/Chlorophyll.git
CHL_SHA=787338fc7c297f54e2000bca02f4fa36f1154c5a
MIN=16.0
export PATH="$WORK/venv/bin:$PATH"
cd "$WORK"
[ -f mesa.tar.xz ] && [ -f ios-arm64.cross ] && [ -f out/libdrm.2.dylib ] || { echo "run build-mesa-ios.sh first" >&2; exit 1; }
if [ ! -d chl ]; then
	git clone -q "$CHL_REPO" chl
	git -C chl checkout -q "$CHL_SHA"
	git -C chl apply "$HERE/a9y-vs-ssbo-to-ubo.patch"     # A5xx: no SSBOs in the vertex stage
	git -C chl apply "$HERE/a9y-cb-delta-a6xx-only.patch" # A5xx: constant-buffer deltas broke render passes
	git -C chl apply "$HERE/a9y-probe-t4-wait.patch"      # the probe's T4 assumed inline commits
	git -C chl apply "$HERE/a9y-ios16-registration.patch" # iOS 16: no MTLCopyAllDevices; CA probe fixes
	git -C chl apply "$HERE/a9y-ios16-xr10-srgb.patch"    # iOS 16: CA's deep-colour pipelines in format 551 too
	git -C chl apply "$HERE/a9y-fs-half-outputs.patch"    # A5xx: 16-bit colour outputs drew nothing
	git -C chl apply "$HERE/a9y-probe-carenderer-target.patch" # CARenderer probe: target size / IOSurface
	git -C chl apply "$HERE/a9y-fbfetch-pre-a6xx.patch"   # A5xx: no framebuffer fetch in freedreno (luma_boost: black)
	git -C chl apply "$HERE/a9y-probe-carenderer-diffmap.patch" # CARenderer probe: platter scenes, where pixels differ
	git -C chl apply "$HERE/a9y-half-varyings-pre-a6xx.patch"  # A5xx: fp16 interpolation dashed every rounded edge
	git -C chl apply "$HERE/a9y-dump-numbered.patch"       # BGMTL_DUMP: one file per pipeline (specializations differ)
fi
if [ ! -d mesa-air ]; then
	mkdir -p air-src && tar -C air-src -xf mesa.tar.xz && mv air-src/mesa-* mesa-air && rmdir air-src
	patch -p1 -d mesa-air < "$MESA/bergamota-mesa-25.0.7.patch"
	patch -p1 -d mesa-air < "$MESA/a9y-fd5-a512-no-2d-blit.patch"
	for p in air-frontend air-ir3 ir3-round-default ir3-shift-split ir3-ssbo-subdword fd-bo-cache-keep fd-bo-cache-skip-busy \
		fd-bo-new-purge-retry; do
		patch -p1 -d mesa-air < "chl/patches/$p.patch"
	done
	mkdir -p mesa-air/src/compiler/air
	cp chl/air/* mesa-air/src/compiler/air/
fi
BLD="$WORK/build-air"
if [ ! -d "$BLD" ]; then
	(cd mesa-air && meson setup "$BLD" --cross-file "$WORK/ios-arm64.cross" --buildtype=release -Dprefix=/usr/local \
		-Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dgallium-xa=disabled -Dtools= -Dvulkan-drivers= \
		-Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dplatforms= -Dosmesa=false -Dopengl=true \
		-Dgles1=disabled -Dgles2=enabled -Dshared-glapi=enabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
		-Dxmlconfig=disabled -Dbuild-tests=false -Dglvnd=disabled -Dlmsensors=disabled -Dgallium-opencl=disabled \
		-Dgallium-rusticl=false -Dshader-cache=disabled)
fi
ninja -C "$BLD" src/gallium/drivers/freedreno/libbgair.dylib

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC=(xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=$MIN -isysroot "$SDK")
OBJC=(-O1 -g -fno-objc-arc -Wall -Wno-deprecated-declarations)
O="$WORK/out/chl"
mkdir -p "$O"
cp -f "$BLD/src/gallium/drivers/freedreno/libbgair.dylib" "$O/libbgair.dylib"
xcrun strip -x "$O/libbgair.dylib"
"${CC[@]}" "${OBJC[@]}" -I chl/air -c chl/src/bgmtl.m -o "$O/bgmtl.o"
# the Metal probes, then CoreAnimation over the device in a test process (MTLAddDevice, CAMetalLayer drawables, CARenderer
# against CA's CPU renderer, every fragment function of QuartzCore's own metallib): what backboardd will ask of it on iOS 16
PROBES="bgmtl_probe bgmtl_render_probe bgmtl_adddev bgmtl_layer_probe bgmtl_carenderer_probe bgmtl_ca_probe"
for probe in $PROBES; do
	"${CC[@]}" "${OBJC[@]}" -I chl/air -c "chl/test/$probe.m" -o "$O/$probe.o"
	"${CC[@]}" -o "$O/$probe" "$O/bgmtl.o" "$O/$probe.o" "$O/libbgair.dylib" "$WORK/out/libdrm.2.dylib" \
		-framework Foundation -framework Metal -framework IOSurface -framework QuartzCore -framework CoreGraphics \
		-Wl,-rpath,@executable_path
done
# the injection dylib (inert unless armed, see chl/inject/bgmtl_inject.m), beside libbgair.dylib in /usr/local/chl
"${CC[@]}" "${OBJC[@]}" -c chl/inject/bgmtl_inject.m -o "$O/bgmtl_inject.o"
"${CC[@]}" -dynamiclib -o "$O/bgmtl-inject.dylib" "$O/bgmtl.o" "$O/bgmtl_inject.o" "$O/libbgair.dylib" "$WORK/out/libdrm.2.dylib" \
	-framework Foundation -framework Metal -framework IOSurface -install_name /usr/local/chl/bgmtl-inject.dylib \
	-Wl,-rpath,@loader_path
# does dyld take DYLD_INSERT_LIBRARIES into a platform binary on this kernel? (a constructor that says where it runs)
"${CC[@]}" -O1 -dynamiclib -o "$O/a9yhello.dylib" "$HERE/a9yhello.c" -install_name /usr/local/chl/a9yhello.dylib
# backboardd's side: edits launchd.plist on the phone, arms a boot, resprings (tools/a9y/drop/a9ybb.c is the launcher)
"${CC[@]}" "${OBJC[@]}" -o "$O/a9ybbctl" "$HERE/a9ybbctl.m" -framework Foundation
for k in bg_kernels bg_render; do
	xcrun -sdk iphoneos metal -mios-version-min=$MIN -c "chl/test/$k.metal" -o "$O/$k.air"
	xcrun -sdk iphoneos metallib "$O/$k.air" -o "$O/$k.metallib"
done
rm -f "$O"/*.o "$O"/*.air
for f in "$O/libbgair.dylib" "$O/bgmtl-inject.dylib" "$O/a9yhello.dylib" "$O/a9ybbctl" $(for p in $PROBES; do echo "$O/$p"; done); do
	codesign -f -s - "$f"
	otool -L "$f"
done
ls -l "$O"
