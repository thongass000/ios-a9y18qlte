#!/usr/bin/env python3
"""Build the Apple Device Tree for the A9Y18QLTE xnu on the Samsung Galaxy A9
2018 (a9y18qlte, Qualcomm SDM660).

Starts from mkadt.py's tree (the /chosen identity the iOS userland needs stays
in step with it) and replaces what is board specific. Every address is from
the a9y18qlte DTB, /proc/iomem or the probe shim's log. Nodes for hardware
this board does not have, or whose drivers are not ported (UFS, Wi-Fi,
display), are dropped: the flame drivers only start from sweet-ufs, so
without it none of them touches SDM660 registers.
Usage:  mkadt_a9y18qlte.py OUT.adt [--dump]
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


def build():
    root = mkadt.build(highmem=True)

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
    for core, (mpidr, hz) in enumerate(CPUS):
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
        set_prop(n, 'timebase-frequency', u32(TIMEBASE))
        set_prop(n, 'clock-frequency', u32(hz))
        set_prop(n, 'peripheral-frequency', u32(TIMEBASE))
        set_prop(n, 'fixed-frequency', u32(TIMEBASE))
        cpus.children.append(n)

    arm_io = child(root, 'arm-io')
    gic = child(arm_io, 'gic')
    set_prop(gic, 'reg', u64(GICD - SOC_BASE, GICD_SIZE, GICR - SOC_BASE, GICR_SIZE))
    set_prop(gic, 'virtual-timer-intid', u32(VTIMER_INTID))
    drop(arm_io, 'wlan', 'disp0')

    fbcon = child(root, 'sweet-fbcon')
    set_prop(fbcon, 'reg', u64(FB, FB_SIZE))
    set_prop(fbcon, 'width', u32(FB_W))
    set_prop(fbcon, 'height', u32(FB_H))
    set_prop(fbcon, 'stride', u32(FB_STRIDE))

    set_prop(child(root, 'pram'), 'reg', u64(PANIC_LOG_PA, mkadt.PANIC_LOG_SIZE))
    drop(root, 'sweet-ufs')
    return root


if __name__ == '__main__':
    blob = adt.serialize(build())
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes, RAM {PHYS_BASE:#x}+{MEM_SIZE:#x}', file=sys.stderr)
