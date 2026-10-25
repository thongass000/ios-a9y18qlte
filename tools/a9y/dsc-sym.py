#!/usr/bin/env python3
"""Symbolicate addresses in an iOS 16 split dyld shared cache.

  tools/dsc-sym.py CACHE_DIR SLIDE ADDR...

CACHE_DIR holds dyld_shared_cache_arm64 and its .NN / .symbols subcaches
(System/Library/Caches/com.apple.dyld of the root image); SLIDE is the cache
slide the kernel's CRASH lines print. Each address is printed as
image`symbol+offset, from the local symbols (.symbols) and each image's own
symbol table (the .dyldlinkedit subcache).
"""
import bisect, glob, mmap, os, struct, sys


class Cache:
    def __init__(self, d):
        self.files = []	# (path, mm, mappings[(addr, size, fileoff)])
        for p in sorted(glob.glob(os.path.join(d, 'dyld_shared_cache_arm64*'))):
            f = open(p, 'rb')
            mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            if mm[:4] != b'dyld':
                continue
            moff, mcnt = struct.unpack_from('<II', mm, 0x10)
            maps = [struct.unpack_from('<QQQ', mm, moff + 32 * i) for i in range(mcnt)]
            self.files.append((p, mm, maps))
        self.main = next(f for f in self.files if f[0].endswith('arm64'))
        mm = self.main[1]
        self.base = self.main[2][0][0]
        ioff, icnt = struct.unpack_from('<II', mm, 0x1c0)
        if not icnt:
            ioff, icnt = struct.unpack_from('<II', mm, 0x18)
        imgs = []
        for i in range(icnt):
            addr, _, _, poff = struct.unpack_from('<QQQI', mm, ioff + 32 * i)
            path = mm[poff:mm.find(b'\0', poff)].decode()
            imgs.append((addr, path))
        imgs.sort()
        self.imgs = imgs
        self.img_addrs = [a for a, _ in imgs]
        self.syms = {}

    def read(self, addr, n):
        for _, mm, maps in self.files:
            for a, s, fo in maps:
                if a <= addr < a + s:
                    o = fo + (addr - a)
                    return mm[o:o + n]
        return None

    def local_symbols(self):
        """dylibOffset -> [(addr, name)] from the .symbols subcache."""
        sf = next((f for f in self.files if f[0].endswith('.symbols')), None)
        out = {}
        if not sf:
            return out
        mm = sf[1]
        lso, lss = struct.unpack_from('<QQ', mm, 0x48)
        no, nc, so, ss, eo, ec = struct.unpack_from('<IIIIII', mm, lso)
        for i in range(ec):
            dyoff, start, cnt = struct.unpack_from('<QII', mm, lso + eo + 16 * i)
            lst = out.setdefault(dyoff, [])
            for k in range(start, start + cnt):
                strx, typ, sect, desc, val = struct.unpack_from('<IBBHQ', mm, lso + no + 16 * k)
                if typ & 0xe0 or not val:	# stabs / undefined
                    continue
                e = mm.find(b'\0', lso + so + strx)
                lst.append((val, mm[lso + so + strx:e].decode('latin1')))
        return out

    def image_symbols(self, addr):
        """Symbols of the image loaded at addr: its LC_SYMTAB plus local symbols."""
        if addr in self.syms:
            return self.syms[addr]
        if not hasattr(self, '_local'):
            self._local = self.local_symbols()
        lst = list(self._local.get(addr - self.base, []))
        hdr = self.read(addr, 32)
        ncmds = struct.unpack_from('<I', hdr, 16)[0]
        lc = self.read(addr + 32, struct.unpack_from('<I', hdr, 20)[0])
        linkedit_vm = linkedit_off = None
        symtab = None
        p = 0
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', lc, p)
            if cmd == 0x19 and lc[p + 8:p + 24].rstrip(b'\0') == b'__LINKEDIT':
                linkedit_vm, _, linkedit_off = struct.unpack_from('<QQQ', lc, p + 24)
            elif cmd == 0x2:
                symtab = struct.unpack_from('<IIII', lc, p + 8)
            p += size
        if symtab and linkedit_vm is not None:
            symoff, nsyms, stroff, strsize = symtab
            # __LINKEDIT fileoff is relative to the cache file it sits in
            sym_addr = linkedit_vm + (symoff - linkedit_off)
            str_addr = linkedit_vm + (stroff - linkedit_off)
            st = self.read(sym_addr, 16 * nsyms) or b''
            strs = self.read(str_addr, strsize) or b''
            for k in range(len(st) // 16):
                strx, typ, sect, desc, val = struct.unpack_from('<IBBHQ', st, 16 * k)
                if typ & 0xe0 or not val or (typ & 0x0e) != 0x0e:
                    continue
                e = strs.find(b'\0', strx)
                lst.append((val, strs[strx:e].decode('latin1')))
        lst.sort()
        self.syms[addr] = lst
        return lst

    def symbolicate(self, a):
        i = bisect.bisect_right(self.img_addrs, a) - 1
        if i < 0:
            return '?'
        iaddr, path = self.imgs[i]
        lst = self.image_symbols(iaddr)
        j = bisect.bisect_right([v for v, _ in lst], a) - 1
        name = os.path.basename(path)
        if j < 0:
            return f'{name}+{a - iaddr:#x}'
        v, s = lst[j]
        return f'{name}`{s}+{a - v:#x}'


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    c = Cache(sys.argv[1])
    slide = int(sys.argv[2], 16)
    for a in sys.argv[3:]:
        x = int(a, 16)
        print(f'{a} -> {c.symbolicate(x - slide)}')


if __name__ == '__main__':
    main()
