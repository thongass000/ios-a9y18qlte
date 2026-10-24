#!/usr/bin/env python3
"""Build the Apple Device Tree for the A9Y18QLTE xnu on the Samsung Galaxy A9
2018 (a9y18qlte, Qualcomm SDM660).

Starts from mkadt.py's tree (the /chosen identity the iOS userland needs stays
in step with it) and replaces what is board specific. Every address is from
the a9y18qlte DTB, /proc/iomem or the probe shim's log. Nodes for hardware
this board does not have, or whose drivers are not ported (Wi-Fi), are
dropped; disp0 is the CPU framebuffer on the splash buffer. sweet-ufs points at the SDM660 UFS host; on A9Y18QLTE
sweet_ufs.c starts only the console log after it, none of the flame drivers.
Usage:  mkadt_a9y18qlte.py OUT.adt [--dump] [--qemu] [--root-runs FIEMAP.txt]
--root-runs: the root is a preallocated file in userdata; FIEMAP.txt is
tools/fiemap's output for it (a9y-ios), its extents become a9y,root-runs.
--qemu: the phone's RAM map with QEMU virt's GIC, CNTFRQ and one CPU (the
kernel's console mirror sits just below the phone's end of RAM, so QEMU
needs the same memory layout, not mkadt.py --qemu's).
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402
import mkadt  # noqa: E402
from mkadt import node, u32, u64, cstr  # noqa: E402

# RAM: Linux sees 0x9fa00000-0x1fd8bffff as one range; the fixed carve-outs,
# the splash buffer, the shim, ramoops and the bootloader's ramdisk are all
# below 4G. XNU takes 4G up to the panic region; then the console log mirror
# (bsd/dev/sweet_ufs.c SWEET_MIRROR_PA) up to 0x1fd801000, RAM ends 0x1fd8c0000.
PHYS_BASE = 0x100000000
PANIC_LOG_PA = 0x1fd300000
MEM_SIZE = PANIC_LOG_PA - PHYS_BASE

SOC_BASE = mkadt.SOC_BASE
# GIC-500: interrupt-controller@17a00000, reg <0x17a00000 0x10000 0x17b00000 0x100000>
GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17b00000, 0x100000
# arm,armv8-timer: Qualcomm PPI numbering (1 2 3 0) as on flame, 19.2 MHz
TIMEBASE = 19200000
VTIMER_INTID = 19
# cont_splash_mem: MDP VIG0 scans out 1080x2220 a8r8g8b8, 4320-byte rows
FB, FB_SIZE = 0x9d400000, 0x23ff000
FB_W, FB_H, FB_STRIDE = 1080, 2220, 1080 * 4

# ufshc@1da4000 "ufs_mem" (the ICE at 0x1db0000 stays off: requests carry no crypto)
UFS, UFS_SIZE = 0x1da4000, 0x3000

# Kryo 260: cpu0-3 Cortex-A53 based (MPIDR 0x0-0x3), cpu4-7 Cortex-A73 based (0x100-0x103)
CPUS = [(0x000 + i, 1843200000) for i in range(4)] + [(0x100 + i, 2208000000) for i in range(4)]


def prop(n, key):
    for k, f, v in n.props:
        if k == key:
            return v
    return None


def set_prop(n, key, value):
    n.props = [(k, f, value if k == key else v) for k, f, v in n.props]
    if prop(n, key) is None:
        n.props.append((key, 0, value))


def child(n, name):
    for c in n.children:
        if prop(c, 'name') == cstr(name):
            return c
    raise SystemExit(f'mkadt tree has no {name} node')


def drop(n, *names):
    n.children = [c for c in n.children if prop(c, 'name') not in [cstr(x) for x in names]]


def build(qemu=False, root_runs=None):
    root = mkadt.build(qemu=qemu, highmem=True)

    set_prop(root, 'compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet')
             + cstr('samsung,a9y18qlte') + cstr('qcom,sdm660') + cstr('AppleARM'))

    chosen = child(root, 'chosen')
    set_prop(chosen, 'dram-base', u64(PHYS_BASE))
    set_prop(chosen, 'dram-size', u64(MEM_SIZE))
    set_prop(chosen, 'firmware-version', cstr('a9y18qlte-shim'))
    set_prop(chosen, 'system-firmware-version', cstr('a9y18qlte-shim'))

    cpus = child(root, 'cpus')
    template = cpus.children[0]
    cpus.children = []
    timebase = mkadt.QEMU_TIMEBASE if qemu else TIMEBASE
    for core, (mpidr, hz) in enumerate(CPUS[:1] if qemu else CPUS):
        n = adt.Node()
        n.props = list(template.props)
        n.children = []
        set_prop(n, 'name', cstr(f'cpu{core}'))
        set_prop(n, 'AAPL,phandle', u32(900 + core))
        # qcom,kryo470 is what SweetCPU matches
        set_prop(n, 'compatible', cstr('qcom,kryo470') + cstr('qcom,kryo260') + cstr('ARM,v8'))
        set_prop(n, 'reg', u32(mpidr))
        set_prop(n, 'cpu-id', u32(core))
        set_prop(n, 'cluster-core-id', u32(core))
        set_prop(n, 'l2-cache-id', u32(mpidr >> 8))
        set_prop(n, 'state', cstr('running' if core == 0 else 'stopped'))
        set_prop(n, 'timebase-frequency', u32(timebase))
        set_prop(n, 'clock-frequency', u32(hz))
        set_prop(n, 'peripheral-frequency', u32(timebase))
        set_prop(n, 'fixed-frequency', u32(timebase))
        cpus.children.append(n)

    arm_io = child(root, 'arm-io')
    if not qemu:	# mkadt.build(qemu=True) already has QEMU virt's GIC and timer PPI
        gic = child(arm_io, 'gic')
        set_prop(gic, 'reg', u64(GICD - SOC_BASE, GICD_SIZE, GICR - SOC_BASE, GICR_SIZE))
        set_prop(gic, 'virtual-timer-intid', u32(VTIMER_INTID))
    # disp0 stays: SweetFramebuffer (CPU copies into the splash buffer ABL keeps scanning out)
    drop(arm_io, 'wlan')

    fbcon = child(root, 'sweet-fbcon')
    set_prop(fbcon, 'reg', u64(FB, FB_SIZE))
    set_prop(fbcon, 'width', u32(FB_W))
    set_prop(fbcon, 'height', u32(FB_H))
    set_prop(fbcon, 'stride', u32(FB_STRIDE))

    # iBoot's panic region; mkadt.build(qemu=True) has none, the phone and QEMU both get it here
    drop(root, 'pram')
    root.children.append(node('pram', [('AAPL,phandle', u32(899)),
                                       ('reg', u64(PANIC_LOG_PA, mkadt.PANIC_LOG_SIZE))]))
    if not qemu:	# bsd/dev/sweet_ufs.c: the UFS host ABL leaves running
        ufs = child(root, 'sweet-ufs')
        set_prop(ufs, 'reg', u64(UFS, UFS_SIZE))
        if root_runs:
            set_prop(ufs, 'a9y,root-runs', b''.join(u64(*r) for r in root_runs))
    return root


def read_runs(path):
    """tools/fiemap output for the root file in userdata (bytes, hex) ->
    physically contiguous runs in 4 KiB blocks: (logical, physical, count)."""
    runs = []
    for line in open(path):
        if not line.startswith('0x'):
            continue
        lo, ph, ln = (int(x, 16) for x in line.split()[:3])
        if (lo | ph | ln) % 4096:
            raise SystemExit(f'{path}: extent not 4 KiB aligned: {line.strip()}')
        lo, ph, ln = lo // 4096, ph // 4096, ln // 4096
        if runs and runs[-1][0] + runs[-1][2] == lo and runs[-1][1] + runs[-1][2] == ph:
            runs[-1][2] += ln
        else:
            runs.append([lo, ph, ln])
    if not runs or runs[0][0] != 0 or any(a[0] + a[2] != b[0] for a, b in zip(runs, runs[1:])):
        raise SystemExit(f'{path}: extents do not cover the file from 0 without holes')
    return [tuple(r) for r in runs]


if __name__ == '__main__':
    runs = None
    if '--root-runs' in sys.argv:
        runs = read_runs(sys.argv[sys.argv.index('--root-runs') + 1])
        print(f'root file: {len(runs)} runs, {sum(r[2] for r in runs) * 4096 >> 20} MiB', file=sys.stderr)
    blob = adt.serialize(build(qemu='--qemu' in sys.argv, root_runs=runs))
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes, RAM {PHYS_BASE:#x}+{MEM_SIZE:#x}', file=sys.stderr)
