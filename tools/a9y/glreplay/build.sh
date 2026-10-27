#!/bin/sh
# Build a9yreplay for iOS 16 arm64 on Linux, the same way as gputest/build.sh (no SDK; the .tbd stubs there
# stand in for libSystem and IOKit). capture.h is made by ~/a9-ios/gl/rd2c.py from a Mesa FD512 capture.
#   tools/a9y/glreplay/build.sh [CLANG_BIN_DIR]   (default ~/clang-r596125/bin)
set -e
cd "$(dirname "$0")"
B=${1:-$HOME/clang-r596125/bin}
mkdir -p out
"$B/clang" -target arm64-apple-ios16.0 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
	-c a9yreplay.c -o out/a9yreplay.o
"$B/ld64.lld" -arch arm64 -platform_version ios 16.0 16.0 -adhoc_codesign -o out/a9yreplay \
	out/a9yreplay.o ../gputest/libSystem.tbd ../gputest/IOKit.tbd
ls -la out/a9yreplay
