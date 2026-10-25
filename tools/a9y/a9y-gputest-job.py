#!/usr/bin/env python3
"""Install (or remove) tools/a9y/gputest's a9ygputest on a mounted iOS root.

  sudo a9y-gputest-job.py MOUNTED_ROOT BINARY     install
  sudo a9y-gputest-job.py MOUNTED_ROOT --remove   take it out again

The binary goes to /usr/local/bin/a9ygputest; launchd.plist gets a job that
runs it once at boot as root with stdout and stderr on /dev/console, which
XNU's on-disk console log keeps (tools/read-xnu-log.py). Needs the kernel's
SweetAccelerator user client: boot-args sweetgpu=1 sweetmtl=3.
"""
import os, plistlib, shutil, sys

LABEL = 'org.a9y.gputest'
KEY = '/System/Library/LaunchDaemons/org.a9y.gputest.plist'
DEST = 'usr/local/bin/a9ygputest'


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, what = sys.argv[1], sys.argv[2]
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    jobs = plist['LaunchDaemons']
    dest = os.path.join(root, DEST)
    if what == '--remove':
        jobs.pop(KEY, None)
        if os.path.exists(dest):
            os.remove(dest)
    else:
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(what, dest)
        os.chown(dest, 0, 0)
        os.chmod(dest, 0o755)
        jobs[KEY] = {
            'Label': LABEL,
            'ProgramArguments': ['/' + DEST],
            'RunAtLoad': True,
            'KeepAlive': False,
            'LaunchOnlyOnce': True,
            'UserName': 'root',
            'StandardOutPath': '/dev/console',
            'StandardErrorPath': '/dev/console',
        }
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'launchd.plist: {LABEL} {"removed" if what == "--remove" else "installed"}; '
          f'{len(jobs)} jobs')


if __name__ == '__main__':
    main()
