#!/bin/sh
# Build wland for iOS 16 arm64 into phone/wifi/out (signed ad hoc).
#   WLAND_BOARD=a9y: the Galaxy A9 2018 (sdm660, board.h) -> out/wland-a9y; default the Pixel 4 -> out/wland
#   IOS_CC: the compiler (default work/userland/bin/ios-cc)
set -e
cd "$(dirname "$0")"
CC=${IOS_CC:-../../work/userland/bin/ios-cc}
OUT=out/wland
BOARD_FLAGS=
if [ "${WLAND_BOARD:-}" = a9y ]; then
	OUT=out/wland-a9y
	BOARD_FLAGS=-DBOARD_A9Y
fi
mkdir -p out
$CC -O2 -Wall -Wextra -Wno-unused-parameter $BOARD_FLAGS -o $OUT wland.c smem.c pmem.c glink.c link.c qmi.c rmtfs.c \
	pdmapper.c tftp.c scm.c wlfw.c radio.c ipnet.c bridge.c mss_a9y.c
codesign -f -s - $OUT 2>/dev/null
ls -la $OUT
