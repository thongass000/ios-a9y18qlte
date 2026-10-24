#!/usr/bin/env python3
"""Fetch XNU's on-disk console log from the root file in userdata.

  tools/read-xnu-log.py EXTENTS [OUT.txt]

bsd/dev/sweet_ufs.c (ufsrw=1) keeps the console in the root's last 1025
blocks: a header block ("SWEETLOG", u32 version, u32 ring size, u64 total
bytes, u64 seq), then the 4 MiB ring (byte n at n % size). EXTENTS is
tools/fiemap's output for /data/ios-root.img (the map in the ADT); the
blocks are read raw from /dev/block/sda29, as XNU wrote them.
"""
import os, struct, subprocess, sys

PART = '/dev/block/sda29'
BLOCK = 4096
LOG_BLOCKS = 1 + (4 << 20) // BLOCK
TMP_PHONE = '/data/local/tmp/xnu-log.bin'


def runs_of(path):
    runs = []
    for line in open(path):
        if not line.startswith('0x'):
            continue
        lo, ph, ln = (int(x, 16) for x in line.split()[:3])
        if runs and runs[-1][0] + runs[-1][2] == lo and runs[-1][1] + runs[-1][2] == ph:
            runs[-1][2] += ln
        else:
            runs.append([lo, ph, ln])
    return runs


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    runs = runs_of(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else None
    size = sum(r[2] for r in runs)
    start = size - LOG_BLOCKS * BLOCK
    run = next(r for r in runs if r[0] <= start < r[0] + r[2])
    if start + LOG_BLOCKS * BLOCK > run[0] + run[2]:
        sys.exit('log area is not in one run')
    phys = run[1] + (start - run[0])
    subprocess.run(['adb', 'shell', f'dd if={PART} of={TMP_PHONE} bs={BLOCK} skip={phys // BLOCK} '
                    f'count={LOG_BLOCKS} 2>/dev/null'], check=True)
    local = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'xnu-log.bin')
    subprocess.run(['adb', 'pull', TMP_PHONE, local], check=True, capture_output=True)
    subprocess.run(['adb', 'shell', f'rm {TMP_PHONE}'], check=True)
    d = open(local, 'rb').read()
    magic, version, rsize, total, seq = struct.unpack_from('<8sIIQQ', d)
    if magic != b'SWEETLOG':
        sys.exit(f'no SWEETLOG header at {PART} {phys:#x} (XNU never wrote its log)')
    ring = d[BLOCK:BLOCK + rsize]
    text = ring[:total] if total <= rsize else ring[total % rsize:] + ring[:total % rsize]
    print(f'# SWEETLOG v{version}: {total} bytes, flush #{seq}', file=sys.stderr)
    if out:
        open(out, 'wb').write(text)
    else:
        sys.stdout.buffer.write(text)


if __name__ == '__main__':
    main()
