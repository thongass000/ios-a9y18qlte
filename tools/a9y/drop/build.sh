#!/bin/sh
# Build a9ydrop and a9yrun for iOS 16 arm64 on Linux, as gputest/build.sh (no SDK; libSystem.tbd here, IOKit.tbd from
# gputest).
#   tools/a9y/drop/build.sh [CLANG_BIN_DIR]   (default ~/clang-r596125/bin)
set -e
cd "$(dirname "$0")"
B=${1:-$HOME/clang-r596125/bin}
mkdir -p out
for p in a9ydrop a9yrun; do
	"$B/clang" -target arm64-apple-ios16.0 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
		-c $p.c -o out/$p.o
	"$B/ld64.lld" -arch arm64 -platform_version ios 16.0 16.0 -adhoc_codesign -o out/$p \
		out/$p.o libSystem.tbd ../gputest/IOKit.tbd
done
ls -la out/a9ydrop out/a9yrun
