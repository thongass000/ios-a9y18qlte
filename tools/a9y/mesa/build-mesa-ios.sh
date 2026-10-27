#!/bin/bash
# Cross-build Mesa 25.0.7 for iOS 16 arm64 on macOS (Xcode with the iphoneos SDK): OSMesa with GLES contexts over Gallium
# freedreno (A5xx fd5 + ir3) and the msm backend, which runs over a9y_drm.c, the libdrm stand-in for SweetAccelerator v2.
# Mesa gets Bergamota's patch (bergamota-mesa-25.0.7.patch, MIT, github.com/ry-diffusion/Bergamota at 37a7a7a: OSMesa ES
# profile and hardware target, BO maps through the stand-in, iOS build fixes), the recipe follows their build-mesa.sh.
#   WORK=/some/dir tools/a9y/mesa/build-mesa-ios.sh       -> $WORK/out: libdrm.2.dylib libOSMesa.8.dylib (+ libglapi) a9yosmtri
# Needs: python3, brew bison; mako/pyyaml/meson/ninja go into a venv.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:?set WORK}"
MESA_VER=25.0.7
DRM_VER=2.4.124
MIN=16.0
mkdir -p "$WORK"
cd "$WORK"
if [ ! -d venv ]; then
	python3 -m venv venv
	./venv/bin/pip -q install mako pyyaml packaging setuptools meson ninja
fi
export PATH="$WORK/venv/bin:$PATH"
if [ ! -d mesa ]; then
	curl -sSfL -o mesa.tar.xz "https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz"
	tar xf mesa.tar.xz && mv "mesa-$MESA_VER" mesa
	patch -p1 -d mesa < "$HERE/bergamota-mesa-$MESA_VER.patch"
fi
if [ ! -d libdrm ]; then    # headers only (xf86drm.h for the msm backend); the library is the stand-in
	curl -sSfL -o libdrm.tar.xz "https://dri.freedesktop.org/libdrm/libdrm-$DRM_VER.tar.xz"
	tar xf libdrm.tar.xz && mv "libdrm-$DRM_VER" libdrm
fi
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC=(xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=$MIN -isysroot "$SDK")
OUT="$WORK/out"
mkdir -p "$OUT" drm pc

# the stand-in, installed on the phone as /usr/local/lib/libdrm.2.dylib
"${CC[@]}" -O2 -g -Wall -Wextra -fvisibility=hidden -dynamiclib \
	-install_name /usr/local/lib/libdrm.2.dylib -current_version 2.4.124 -compatibility_version 2.0.0 \
	-I"$WORK/mesa/include" "$HERE/a9y_drm.c" -o "$OUT/libdrm.2.dylib" -framework IOKit -framework CoreFoundation
cp "$OUT/libdrm.2.dylib" drm/libdrm.2.dylib
ln -sf libdrm.2.dylib drm/libdrm.dylib
cat > pc/libdrm.pc << EOT
Name: libdrm
Description: a9y18qlte libdrm stand-in (tools/a9y/mesa/a9y_drm.c)
Version: $DRM_VER
Cflags: -I$WORK/libdrm/include/drm -I$WORK/libdrm
Libs: -L$WORK/drm -ldrm
EOT

CROSS="$WORK/ios-arm64.cross"
cat > "$CROSS" << EOT
[binaries]
c = ['xcrun','--sdk','iphoneos','clang','-arch','arm64']
cpp = ['xcrun','--sdk','iphoneos','clang++','-arch','arm64']
objc = ['xcrun','--sdk','iphoneos','clang','-arch','arm64']
ar = ['xcrun','--sdk','iphoneos','ar']
strip = ['xcrun','--sdk','iphoneos','strip']
ranlib = ['xcrun','--sdk','iphoneos','ranlib']
bison = '$(brew --prefix bison)/bin/bison'
pkg-config = '$(command -v pkg-config)'

[built-in options]
pkg_config_path = ['$WORK/pc']
c_args = ['-miphoneos-version-min=$MIN','-isysroot','$SDK']
cpp_args = ['-miphoneos-version-min=$MIN','-isysroot','$SDK']
c_link_args = ['-miphoneos-version-min=$MIN','-isysroot','$SDK']
cpp_link_args = ['-miphoneos-version-min=$MIN','-isysroot','$SDK']

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOT
BLD="$WORK/build"
if [ ! -d "$BLD" ]; then
	(cd mesa && meson setup "$BLD" --cross-file "$CROSS" --buildtype=release -Dprefix=/usr/local \
		-Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dgallium-xa=disabled -Dtools= \
		-Dvulkan-drivers= -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dplatforms= \
		-Dosmesa=true -Dopengl=true -Dgles1=enabled -Dgles2=enabled -Dshared-glapi=enabled \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dbuild-tests=false \
		-Dglvnd=disabled -Dlmsensors=disabled -Dgallium-opencl=disabled -Dgallium-rusticl=false \
		-Dshader-cache=enabled -Dshader-cache-max-size=128M)
fi
ninja -C "$BLD"
# the real files (not the version symlinks), each under the name its install name ends in
find "$BLD/src" -type f \( -name "libOSMesa*.dylib" -o -name "libglapi*.dylib" \) | while read -r f; do
	cp "$f" "$OUT/$(basename "$(otool -D "$f" | tail -1)")"
done

# the test: GLES through OSMesa on the GPU
"${CC[@]}" -O2 -Wall -Wextra -I"$WORK/mesa/include" "$HERE/osmtri.c" -o "$OUT/a9yosmtri" \
	"$OUT/libOSMesa.8.dylib" -framework IOKit -framework CoreFoundation

# every library lives in /usr/local/lib on the phone: absolute install names and references, then a fresh ad hoc signature
for f in "$OUT"/*.dylib "$OUT/a9yosmtri"; do
	case "$f" in *.dylib) install_name_tool -id "/usr/local/lib/$(basename "$f")" "$f" ;; esac
	otool -L "$f" | awk 'NR > 1 { print $1 }' | grep '^@rpath/' | while read -r dep; do
		install_name_tool -change "$dep" "/usr/local/lib/${dep#@rpath/}" "$f"
	done
	codesign -f -s - "$f"
	otool -L "$f"
done
ls -l "$OUT"
