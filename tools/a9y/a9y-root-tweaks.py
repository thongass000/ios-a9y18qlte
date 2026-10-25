#!/usr/bin/env python3
"""a9y18qlte changes on top of tools/sweet-root-tweaks.py.

  sudo a9y-root-tweaks.py MOUNTED_ROOT

Idempotent. launchd.plist: jobs for hardware this phone has no driver for
are removed, so their services fail at lookup instead of relaunching every
few seconds (each relaunch is an xpcproxy exec plus a crash):
  com.apple.bluetoothd    no Bluetooth driver (46 relaunches in 8 min)
  com.apple.fairplayd.H2  FairPlay needs Apple's secure hardware (46)
"""
import os, plistlib, sys

REMOVE = ('com.apple.bluetoothd', 'com.apple.fairplayd.H2')


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = os.path.join(sys.argv[1], 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    jobs = plist['LaunchDaemons']
    gone = [k for k, j in jobs.items() if j.get('Label') in REMOVE]
    for k in gone:
        del jobs[k]
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'launchd.plist: removed {len(gone)} jobs: {", ".join(sorted(gone)) or "none"}')


if __name__ == '__main__':
    main()
