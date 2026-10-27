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
for probe in bgmtl_probe bgmtl_render_probe; do
	"${CC[@]}" "${OBJC[@]}" -I chl/air -c "chl/test/$probe.m" -o "$O/$probe.o"
	"${CC[@]}" -o "$O/$probe" "$O/bgmtl.o" "$O/$probe.o" "$O/libbgair.dylib" "$WORK/out/libdrm.2.dylib" \
		-framework Foundation -framework Metal -framework IOSurface -Wl,-rpath,@executable_path
done
for k in bg_kernels bg_render; do
	xcrun -sdk iphoneos metal -mios-version-min=$MIN -c "chl/test/$k.metal" -o "$O/$k.air"
	xcrun -sdk iphoneos metallib "$O/$k.air" -o "$O/$k.metallib"
done
rm -f "$O"/*.o "$O"/*.air
for f in "$O/libbgair.dylib" "$O/bgmtl_probe" "$O/bgmtl_render_probe"; do
	codesign -f -s - "$f"
	otool -L "$f"
done
ls -l "$O"
