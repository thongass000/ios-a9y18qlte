#!/usr/bin/env python3
"""Write an XNU root image onto the raw blocks of the root file in userdata.

  tools/write-root.py IMAGE EXTENTS [--dry-run] [--from N]   (N: resume at range N)

EXTENTS is tools/fiemap's output for /data/ios-root.img, the map the ADT
was built from (mkadt_a9y18qlte.py --root-runs). The file is only a
reservation: writing through it would encrypt the data (file-based
encryption), so the image goes to the same blocks of /dev/block/sda29 with
dd, and XNU reads it there.

Refuses unless the phone's current extents equal EXTENTS, /data is still
sda29 itself (no dm-default-key between), and IMAGE ends before the file's
last 16 MiB (sweet_ufs.c's console log). Writes only IMAGE's data regions
(holes are free space), clipped to the runs, then reads every range back
and compares md5s.
"""
import hashlib, os, subprocess, sys

ROOT_FILE = '/data/ios-root.img'
PART = '/dev/block/sda29'
FIEMAP = '/data/local/tmp/fiemap'
BLOCK = 4096
LOG_RESERVE = 16 << 20
CHUNK_PHONE = '/data/local/tmp/root-chunk.bin'
CHUNK_LOCAL = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'root-chunk.bin')


def adb(*args, **kw):
    return subprocess.run(['adb', *args], check=True, **kw)


def shell(cmd):
    for attempt in range(5):	# adb drops now and then on a long run: retry (every command is idempotent)
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


def data_regions(path, size):
    """(offset, length) of IMAGE's data, skipping holes (SEEK_DATA/SEEK_HOLE)."""
    out = []
    with open(path, 'rb') as f:
        fd, pos = f.fileno(), 0
        while pos < size:
            try:
                start = os.lseek(fd, pos, os.SEEK_DATA)
            except OSError:
                break
            end = os.lseek(fd, start, os.SEEK_HOLE)
            out.append((start, end - start))
            pos = end
    return out


def chunks(image_regions, runs, most=64 << 20):
    """Image ranges intersected with runs, block aligned and at most MOST bytes:
    (image offset, partition offset, length)."""
    for start, length in image_regions:
        s = start // BLOCK * BLOCK
        e = (start + length + BLOCK - 1) // BLOCK * BLOCK
        for lo, ph, ln in runs:
            a, b = max(s, lo), min(e, lo + ln)
            while a < b:
                n = min(b - a, most)
                yield a, ph + (a - lo), n
                a += n


def best_bs(*vals):
    bs = 1 << 20
    while bs > BLOCK and any(v % bs for v in vals):
        bs //= 2
    return bs


def md5_local(path, off, length):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        f.seek(off)
        left = length
        while left:
            b = f.read(min(left, 1 << 20))
            if not b:	# past EOF of the image: the block was padded with zeros
                b = bytes(min(left, 1 << 20))
            h.update(b)
            left -= len(b)
    return h.hexdigest()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    image, extents = sys.argv[1], sys.argv[2]
    dry = '--dry-run' in sys.argv
    size = os.path.getsize(image)

    want = runs_of(open(extents).read())
    now = runs_of(shell(f'{FIEMAP} {ROOT_FILE}'))
    if not want or now != want:
        sys.exit('the root file\'s extents changed since EXTENTS: rebuild the ADT first')
    mounts = shell('mount')
    if f'{PART} on /data ' not in mounts:
        sys.exit(f'/data is not mounted straight from {PART}: refusing raw writes')
    file_size = sum(r[2] for r in want)
    if size > file_size - LOG_RESERVE:
        sys.exit(f'image {size} bytes reaches the log area: at most {file_size - LOG_RESERVE}')

    work = list(chunks(data_regions(image, size), want))
    total = sum(c[2] for c in work)
    print(f'{len(want)} runs, image {size >> 20} MiB, {len(work)} ranges, {total >> 20} MiB to write')
    first = int(sys.argv[sys.argv.index('--from') + 1]) if '--from' in sys.argv else 1
    for i, (off, poff, ln) in enumerate(work):
        if i + 1 < first:
            continue
        bs = best_bs(off, poff, ln)
        assert any(ph <= poff and poff + ln <= ph + l for _, ph, l in want), 'range outside the runs'
        print(f'[{i + 1}/{len(work)}] image {off:#x} -> {PART} {poff:#x}, {ln >> 10} KiB (bs {bs})', flush=True)
        if dry:
            continue
        with open(image, 'rb') as f:
            f.seek(off)
            data = f.read(ln)
        data += bytes(ln - len(data))
        # adb exec-in drops stdin with the Windows adb WSL uses: push, then dd on the phone
        with open(CHUNK_LOCAL, 'wb') as f:
            f.write(data)
        for attempt in range(5):	# a push now and then fails on a busy USB link
            if subprocess.run(['adb', 'push', CHUNK_LOCAL, CHUNK_PHONE], capture_output=True).returncode == 0:
                break
            print(f'  push failed, retry {attempt + 1}', flush=True)
            subprocess.run(['adb', 'wait-for-device'])
        else:
            sys.exit(f'adb push kept failing at range {i + 1}: resume with --from {i + 1}')
        shell(f'dd if={CHUNK_PHONE} of={PART} bs={bs} seek={poff // bs} count={ln // bs} conv=fsync '
              f'2>/dev/null; rm {CHUNK_PHONE}')
        got = shell(f'dd if={PART} bs={bs} skip={poff // bs} count={ln // bs} 2>/dev/null | md5sum').split()[0]
        if got != md5_local(image, off, ln):
            sys.exit(f'readback mismatch at {PART} {poff:#x}')
    print('dry run, nothing written' if dry else 'written and verified')


if __name__ == '__main__':
    main()
