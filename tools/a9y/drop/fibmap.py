#!/usr/bin/env python3
"""Byte extents of a file on a mounted image, from FIBMAP (Linux hfsplus has no FIEMAP): "0xOFFSET 0xLENGTH" per
run, offsets in the filesystem's device (the root image). Run as root.   fibmap.py FILE"""
import fcntl, os, struct, sys

FIBMAP, FIGETBSZ = 1, 2
fd = os.open(sys.argv[1], os.O_RDONLY)
bs = struct.unpack('i', fcntl.ioctl(fd, FIGETBSZ, struct.pack('i', 0)))[0]
nblocks = (os.fstat(fd).st_size + bs - 1) // bs
runs = []
for i in range(nblocks):
    p = struct.unpack('i', fcntl.ioctl(fd, FIBMAP, struct.pack('i', i)))[0]
    if p == 0:
        sys.exit(f'block {i} has no place (hole)')
    if runs and runs[-1][0] + runs[-1][1] == p:
        runs[-1][1] += 1
    else:
        runs.append([p, 1])
for p, n in runs:
    print(f'{p * bs:#x} {n * bs:#x}')
