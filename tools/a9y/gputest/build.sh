#!/bin/sh
# Build a9ygputest for iOS 16 arm64 on Linux: clang + ld64.lld, no SDK (the
# calls it makes are declared in the source; libSystem.tbd and IOKit.tbd stand
# in for the libraries), ad hoc signed by the linker.
#   tools/a9y/gputest/build.sh [CLANG_BIN_DIR]   (default ~/clang-r596125/bin)
set -e
cd "$(dirname "$0")"
B=${1:-$HOME/clang-r596125/bin}
mkdir -p out
"$B/clang" -target arm64-apple-ios16.0 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
	-c a9ygputest.c -o out/a9ygputest.o
"$B/ld64.lld" -arch arm64 -platform_version ios 16.0 16.0 -adhoc_codesign -o out/a9ygputest \
	out/a9ygputest.o libSystem.tbd IOKit.tbd
ls -la out/a9ygputest
