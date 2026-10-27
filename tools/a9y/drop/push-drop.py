#!/usr/bin/env python3
"""Send files to the iOS root without touching HFS+: pack them into an a9ydrop archive and write it into the
blocks of /usr/local/drop.bin on /dev/block/sda29. a9ydrop unpacks it at the next XNU boot (see a9ydrop.c).

  push-drop.py DROP_EXTENTS ROOT_EXTENTS /usr/local/PATH=LOCAL_FILE [...]

DROP_EXTENTS: drop.bin's blocks inside the root image, "0xOFFSET 0xLENGTH" per line in bytes, from
`filefrag -v -b4096` on the mounted image (tools/a9y/drop/drop-extents.sh). ROOT_EXTENTS: tools/fiemap's
output for /data/ios-root.img (the image's place on sda29, as for write-root.py). Same checks as
write-root.py: the root file's extents unchanged, /data straight on sda29; every range is read back.
"""
import hashlib, os, struct, subprocess, sys, time

ROOT_FILE = '/data/ios-root.img'
PART = '/dev/block/sda29'
FIEMAP = '/data/local/tmp/fiemap'
BLOCK = 4096
DROP_SIZE = 64 << 20
PHONE_TMP = '/data/local/tmp/drop-archive.bin'
LOCAL_TMP = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'drop-archive.bin')


def shell(cmd):
    for attempt in range(5):        # adb drops now and then: every command here is idempotent
        r = subprocess.run(['adb', 'shell', cmd], capture_output=True, text=True)
        if r.returncode == 0:
            return r.stdout.replace('\r', '')
        print(f'  adb shell failed, retry {attempt + 1}', flush=True)
        subprocess.run(['adb', 'wait-for-device'])
    sys.exit(f'adb shell kept failing: {cmd}')


def runs_of(text):
    """fiemap lines -> physically contiguous runs in bytes: [logical, physical, length]."""
    runs = []
    for line in text.splitlines():
        if not line.startswith('0x'):
            continue
        lo, ph, ln = (int(x, 16) for x in line.split()[:3])
        if runs and runs[-1][0] + runs[-1][2] == lo and runs[-1][1] + runs[-1][2] == ph:
            runs[-1][2] += ln
        else:
            runs.append([lo, ph, ln])
    return runs


def pad8(b):
    return b + bytes(-len(b) % 8)


def archive(pairs):
    body = b''
    for dest, src in pairs:
        data = open(src, 'rb').read()
        p = dest.encode()
        body += struct.pack('<IIQ', len(p), os.stat(src).st_mode & 0o7777, len(data)) + pad8(p) + pad8(data)
    total = 32 + len(body)
    seq = time.time_ns()
    return struct.pack('<8sQIIQ', b'A9YDROP1', seq, len(pairs), 0, total) + body, seq


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    drop_ext, root_ext = sys.argv[1], sys.argv[2]
    pairs = []
    for a in sys.argv[3:]:
        dest, _, src = a.partition('=')
        if not dest.startswith('/usr/local/') or '..' in dest or not os.path.isfile(src):
            sys.exit(f'bad file spec {a}: /usr/local/PATH=LOCAL_FILE')
        pairs.append((dest, src))
    data, seq = archive(pairs)
    if len(data) > DROP_SIZE:
        sys.exit(f'archive {len(data)} bytes is more than drop.bin ({DROP_SIZE})')
    data += bytes(-len(data) % BLOCK)

    drop = [tuple(int(x, 16) for x in l.split()[:2]) for l in open(drop_ext) if l.startswith('0x')]
    if sum(n for _, n in drop) != DROP_SIZE:
        sys.exit('DROP_EXTENTS does not cover the 64 MiB of drop.bin')
    want = runs_of(open(root_ext).read())
    if runs_of(shell(f'{FIEMAP} {ROOT_FILE}')) != want:
        sys.exit('the root file\'s extents changed since ROOT_EXTENTS')
    if f'{PART} on /data ' not in shell('mount'):
        sys.exit(f'/data is not mounted straight from {PART}: refusing raw writes')

    # archive byte ranges -> image ranges (drop.bin's extents) -> partition ranges (the root file's runs)
    work, pos = [], 0
    for ioff, ilen in drop:
        a, end = ioff, ioff + ilen
        while a < end and pos < len(data):
            for lo, ph, ln in want:
                if lo <= a < lo + ln:
                    n = min(end, lo + ln) - a
                    n = min(n, len(data) - pos)
                    work.append((pos, ph + (a - lo), n))
                    a += n
                    pos += n
                    break
            else:
                sys.exit(f'image offset {a:#x} is outside the root file')
    with open(LOCAL_TMP, 'wb') as f:
        f.write(data)
    if subprocess.run(['adb', 'push', LOCAL_TMP, PHONE_TMP], capture_output=True).returncode:
        sys.exit('adb push failed')
    print(f'archive seq {seq}: {len(pairs)} files, {len(data) >> 10} KiB in {len(work)} ranges', flush=True)
    for i, (src, poff, n) in enumerate(work):
        assert src % BLOCK == 0 and poff % BLOCK == 0 and n % BLOCK == 0
        shell(f'dd if={PHONE_TMP} of={PART} bs={BLOCK} skip={src // BLOCK} seek={poff // BLOCK} '
              f'count={n // BLOCK} conv=notrunc,fsync 2>/dev/null')
        got = shell(f'dd if={PART} bs={BLOCK} skip={poff // BLOCK} count={n // BLOCK} 2>/dev/null | md5sum').split()[0]
        if got != hashlib.md5(data[src:src + n]).hexdigest():
            sys.exit(f'readback mismatch at {PART} {poff:#x}')
        print(f'  [{i + 1}/{len(work)}] {n >> 10} KiB -> {PART} {poff:#x}', flush=True)
    shell(f'rm {PHONE_TMP}; sync')
    print('drop written and verified: a9ydrop installs it at the next XNU boot')


if __name__ == '__main__':
    main()
