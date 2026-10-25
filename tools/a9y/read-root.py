#!/usr/bin/env python3
"""Read the XNU root back from the phone: the root file's raw blocks in
userdata, in file order, into a local image (what XNU sees as ufsroot0).

  tools/read-root.py EXTENTS OUT.img [--from N]

EXTENTS is tools/fiemap's output the ADT was built from. Reads 64 MiB at a
time with dd on the phone, pulls it, writes it at its place in OUT.img and
checks the md5 of each piece on both sides. --from N resumes at piece N.
"""
import hashlib, os, subprocess, sys

PART = '/dev/block/sda29'
CHUNK_PHONE = '/data/local/tmp/root-read.bin'
PIECE = 64 << 20


def adb_ok(*args):
    for attempt in range(5):	# adb drops now and then on a long run
        r = subprocess.run(['adb', *args], capture_output=True, text=True)
        if r.returncode == 0:
            return r.stdout.replace('\r', '')
        print(f'  adb {args[0]} failed, retry {attempt + 1}', flush=True)
        subprocess.run(['adb', 'wait-for-device'])
    sys.exit(f'adb kept failing: {args}')


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
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    runs, out = runs_of(sys.argv[1]), sys.argv[2]
    first = int(sys.argv[sys.argv.index('--from') + 1]) if '--from' in sys.argv else 1
    local = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'root-read.bin')
    pieces = [(lo + o, ph + o, min(PIECE, ln - o)) for lo, ph, ln in runs for o in range(0, ln, PIECE)]
    with open(out, 'r+b' if os.path.exists(out) else 'w+b') as f:
        f.truncate(sum(r[2] for r in runs))
        for i, (lo, ph, ln) in enumerate(pieces):
            if i + 1 < first:
                continue
            print(f'[{i + 1}/{len(pieces)}] {PART} {ph:#x} -> {lo:#x}, {ln >> 20} MiB', flush=True)
            bs = 1 << 20 if not (ph | ln) % (1 << 20) else 4096
            want = adb_ok('shell', f'dd if={PART} of={CHUNK_PHONE} bs={bs} skip={ph // bs} count={ln // bs} '
                          f'2>/dev/null && md5sum {CHUNK_PHONE}').split()[0]
            adb_ok('pull', CHUNK_PHONE, local)
            data = open(local, 'rb').read()
            if len(data) != ln or hashlib.md5(data).hexdigest() != want:
                sys.exit(f'piece {i + 1} md5 mismatch: resume with --from {i + 1}')
            f.seek(lo)
            f.write(data)
    adb_ok('shell', f'rm -f {CHUNK_PHONE}')
    print('read and verified')


if __name__ == '__main__':
    main()
