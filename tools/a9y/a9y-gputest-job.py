#!/usr/bin/env python3
"""Install (or remove) tools/a9y/gputest's a9ygputest on a mounted iOS root.

  sudo a9y-gputest-job.py MOUNTED_ROOT BINARY [A9YDROP]   install
  sudo a9y-gputest-job.py MOUNTED_ROOT --remove            take it out again

The binary goes to /usr/local/bin/a9ygputest; launchd.plist gets a job that
runs it once at boot as root with stdout and stderr on /dev/console, which
XNU's on-disk console log keeps (tools/read-xnu-log.py). Needs the kernel's
SweetAccelerator user client: boot-args sweetgpu=1 sweetmtl=3.

With A9YDROP (tools/a9y/drop/out/a9ydrop) the job runs that first: it unpacks
/usr/local/drop.bin, made here once as a 64 MiB file whose blocks never move
(tools/a9y/drop/push-drop.py writes archives into them from Linux), and then
runs a9ygputest. The job has a 1 GB jetsam limit: launchd's default for a
daemon without one killed Mesa (reason namespace 1 code 7, per-process limit).
"""
import os, plistlib, shutil, sys

LABEL = 'org.a9y.gputest'
KEY = '/System/Library/LaunchDaemons/org.a9y.gputest.plist'
DEST = 'usr/local/bin/a9ygputest'
DROP_BIN = 'usr/local/bin/a9ydrop'
DROP_FILE = 'usr/local/drop.bin'
DROP_SIZE = 64 << 20


def put(src, root, rel):
    dest = os.path.join(root, rel)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copyfile(src, dest)
    os.chown(dest, 0, 0)
    os.chmod(dest, 0o755)


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    root, what = sys.argv[1], sys.argv[2]
    drop = sys.argv[3] if len(sys.argv) == 4 else None
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    jobs = plist['LaunchDaemons']
    if what == '--remove':
        jobs.pop(KEY, None)
        for rel in (DEST, DROP_BIN):
            if os.path.exists(os.path.join(root, rel)):
                os.remove(os.path.join(root, rel))
    else:
        put(what, root, DEST)
        program = DEST
        if drop:
            put(drop, root, DROP_BIN)
            program = DROP_BIN
            df = os.path.join(root, DROP_FILE)
            if not os.path.exists(df) or os.path.getsize(df) != DROP_SIZE:
                with open(df, 'wb') as f:      # real zeros, not a sparse file: every block allocated now
                    for _ in range(DROP_SIZE >> 20):
                        f.write(bytes(1 << 20))
                os.chown(df, 0, 0)
                os.chmod(df, 0o644)
        jobs[KEY] = {
            'Label': LABEL,
            'ProgramArguments': ['/' + program],
            'RunAtLoad': True,
            'KeepAlive': False,
            'LaunchOnlyOnce': True,
            'UserName': 'root',
            'StandardOutPath': '/dev/console',
            'StandardErrorPath': '/dev/console',
            'JetsamProperties': {'JetsamPriority': 10, 'JetsamMemoryLimit': 1024},
        }
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'launchd.plist: {LABEL} {"removed" if what == "--remove" else "installed"}; '
          f'{len(jobs)} jobs')


if __name__ == '__main__':
    main()
