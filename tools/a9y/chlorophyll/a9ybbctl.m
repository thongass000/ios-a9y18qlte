// a9y18qlte: Chlorophyll in backboardd, the control side (run as root from a9yrun's list; a9ybb is the launch side).
//   a9ybbctl status          backboardd's job in launchd.plist, the backup, arm / kill-switch files
//   a9ybbctl install         backboardd's job runs /usr/local/bin/a9ybb (from the next boot; launchd reads the plist
//                            at boot); the original plist is kept as /usr/local/etc/launchd.plist.orig
//   a9ybbctl restore         put the original plist back (from the next boot)
//   a9ybbctl arm             this boot: the next backboardd launch gets Chlorophyll (a9ybb.arm = this boot's id, the
//                            injection's bgmtl-bb.arm, its kill switch and state cleared, its log made writable by mobile)
//   a9ybbctl disarm          launches from now on are stock
//   a9ybbctl respring        SIGTERM to backboardd: launchd starts it again, through a9ybb
//   a9ybbctl wait SECONDS
//   a9ybbctl pids NAME       the pids of processes called NAME
//   a9ybbctl cat FILE [MAX]  print a file (the injection logs to /private/var/tmp/bgmtl.log), at most MAX lines
// The plist is changed on the phone, by CoreFoundation's own writer and the phone's own file system, never from Linux.
#import <Foundation/Foundation.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

// libproc (in libSystem; the iOS SDK has no libproc.h)
extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);

#define PLIST "/System/Library/xpc/launchd.plist"
#define BACKUP "/usr/local/etc/launchd.plist.orig"
#define JOB @"/System/Library/LaunchDaemons/com.apple.backboardd.plist"
#define WRAPPER @"/usr/local/bin/a9ybb"
#define ARM "/private/var/tmp/a9ybb.arm"
#define BB_ARM "/private/var/tmp/bgmtl-bb.arm"
#define BB_OFF "/private/var/tmp/bgmtl-bb.off"
#define BB_STATE "/private/var/tmp/bgmtl-bb.state"
#define BB_LOG "/private/var/tmp/bgmtl.log"
#define TRIES "/private/var/tmp/a9ybb.tries"
#define MOBILE 501

static NSMutableDictionary *
load(const char *path)
{
   NSData *d = [NSData dataWithContentsOfFile:@(path)];
   if (!d) return nil;
   NSError *e = nil;
   id p = [NSPropertyListSerialization propertyListWithData:d options:NSPropertyListMutableContainersAndLeaves format:NULL error:&e];
   if (!p) printf("%s: %s\n", path, [[e localizedDescription] UTF8String]);
   return [p isKindOfClass:[NSMutableDictionary class]] ? p : nil;
}

static NSArray *
job_args(NSDictionary *plist)
{
   return plist[@"LaunchDaemons"][JOB][@"ProgramArguments"];
}

// tmp file in the same directory, fsync, rename: a reader sees the old plist or the new one, never half of one
static int
save(NSDictionary *plist, const char *path)
{
   NSError *e = nil;
   NSData *d = [NSPropertyListSerialization dataWithPropertyList:plist format:NSPropertyListBinaryFormat_v1_0 options:0 error:&e];
   if (!d) { printf("serialize: %s\n", [[e localizedDescription] UTF8String]); return 1; }
   char tmp[256];
   snprintf(tmp, sizeof(tmp), "%s.a9ytmp", path);
   FILE *f = fopen(tmp, "w");
   if (!f) { printf("%s: cannot write\n", tmp); return 1; }
   int ok = fwrite(d.bytes, 1, d.length, f) == d.length;
   ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
   fclose(f);
   chmod(tmp, 0644);
   if (!ok || rename(tmp, path) != 0) { printf("%s: write or rename failed\n", path); unlink(tmp); return 1; }
   NSDictionary *back = load(path);
   printf("wrote %s (%lu bytes); backboardd job now runs %s\n", path, (unsigned long)d.length,
          [[job_args(back) description] UTF8String]);
   return back ? 0 : 1;
}

static const char *
boot_id(void)
{
   static char u[64];
   size_t len = sizeof(u) - 1;
   if (sysctlbyname("kern.bootsessionuuid", u, &len, NULL, 0) != 0) return NULL;
   u[len] = 0;
   return u;
}

static void
write_file(const char *path, const char *text, mode_t mode, uid_t owner)
{
   FILE *f = fopen(path, "w");
   if (!f) { printf("%s: cannot write\n", path); return; }
   fputs(text, f);
   fclose(f);
   chmod(path, mode);
   chown(path, owner, (gid_t)-1);
}

static int
pids(const char *name, pid_t *out, int max)
{
   pid_t all[4096];
   int n = proc_listallpids(all, sizeof(all)) , k = 0;
   for (int i = 0; i < n && k < max; i++) {
      char nm[64] = "";
      if (proc_name(all[i], nm, sizeof(nm)) > 0 && !strcmp(nm, name)) out[k++] = all[i];
   }
   return k;
}

int
main(int argc, char **argv)
{
   @autoreleasepool {
      setvbuf(stdout, NULL, _IOLBF, 0);
      const char *cmd = argc > 1 ? argv[1] : "status";
      if (!strcmp(cmd, "status")) {
         NSDictionary *p = load(PLIST);
         printf("launchd.plist: backboardd job runs %s\n", p ? [[job_args(p) description] UTF8String] : "(unreadable)");
         printf("backup %s: %s\n", BACKUP, access(BACKUP, F_OK) == 0 ? "present" : "absent");
         const char *b = boot_id();
         char arm[64] = "";
         FILE *f = fopen(ARM, "r");
         if (f) { if (!fgets(arm, sizeof(arm), f)) arm[0] = 0; fclose(f); }
         printf("armed this boot: %s; bgmtl-bb.arm %s; kill switch bgmtl-bb.off %s\n", b && !strcmp(arm, b) ? "yes" : "no",
                access(BB_ARM, F_OK) == 0 ? "present" : "absent", access(BB_OFF, F_OK) == 0 ? "PRESENT" : "absent");
         pid_t pp[8];
         int n = pids("backboardd", pp, 8);
         for (int i = 0; i < n; i++) printf("backboardd pid %d\n", pp[i]);
         return 0;
      }
      if (!strcmp(cmd, "install")) {
         NSMutableDictionary *p = load(PLIST);
         if (!p || !job_args(p)) { printf("no backboardd job in %s: nothing changed\n", PLIST); return 1; }
         if ([job_args(p) isEqual:@[ WRAPPER ]]) { printf("already installed\n"); return 0; }
         if (access(BACKUP, F_OK) != 0) {
            NSData *d = [NSData dataWithContentsOfFile:@PLIST];
            if (![d writeToFile:@BACKUP atomically:YES]) { printf("cannot keep %s: nothing changed\n", BACKUP); return 1; }
            printf("kept the original as %s\n", BACKUP);
         }
         if (access("/usr/local/bin/a9ybb", X_OK) != 0) { printf("no /usr/local/bin/a9ybb: nothing changed\n"); return 1; }
         p[@"LaunchDaemons"][JOB][@"ProgramArguments"] = @[ WRAPPER ];
         return save(p, PLIST);
      }
      if (!strcmp(cmd, "restore")) {
         NSMutableDictionary *p = load(BACKUP);
         if (!p) { printf("no readable %s: nothing changed\n", BACKUP); return 1; }
         return save(p, PLIST);
      }
      if (!strcmp(cmd, "arm")) {
         const char *b = boot_id();
         if (!b) { printf("no kern.bootsessionuuid: not armed\n"); return 1; }
         unlink(BB_OFF);
         unlink(BB_STATE);
         write_file(ARM, b, 0644, 0);
         write_file(BB_ARM, "on\n", 0644, 0);
         write_file(BB_LOG, "", 0666, MOBILE);
         write_file(TRIES, "0", 0666, MOBILE);   // a9ybb counts armed launches here (backboardd runs as mobile)
         printf("armed for boot %s\n", b);
         return 0;
      }
      if (!strcmp(cmd, "disarm")) {
         unlink(ARM);
         printf("disarmed\n");
         return 0;
      }
      if (!strcmp(cmd, "respring")) {
         pid_t pp[8];
         int n = pids("backboardd", pp, 8);
         for (int i = 0; i < n; i++) printf("SIGTERM backboardd %d: %d\n", pp[i], kill(pp[i], SIGTERM));
         if (!n) printf("no backboardd running\n");
         return n ? 0 : 1;
      }
      if (!strcmp(cmd, "wait") && argc > 2) {
         sleep((unsigned)atoi(argv[2]));
         return 0;
      }
      if (!strcmp(cmd, "pids") && argc > 2) {
         pid_t pp[16];
         int n = pids(argv[2], pp, 16);
         printf("%s:", argv[2]);
         for (int i = 0; i < n; i++) printf(" %d", pp[i]);
         printf("%s\n", n ? "" : " none");
         return 0;
      }
      if (!strcmp(cmd, "cat") && argc > 2) {
         FILE *f = fopen(argv[2], "r");
         if (!f) { printf("%s: absent\n", argv[2]); return 1; }
         int max = argc > 3 ? atoi(argv[3]) : 400, n = 0;
         char line[1024];
         while (n < max && fgets(line, sizeof(line), f)) { fputs(line, stdout); n++; }
         fclose(f);
         return 0;
      }
      if (!strcmp(cmd, "hex") && argc > 2) {   // a binary back through the log: "hex OFFSET BYTES" lines of 64 bytes
         FILE *f = fopen(argv[2], "rb");
         if (!f) { printf("%s: absent\n", argv[2]); return 1; }
         unsigned char b[64];
         size_t n, off = 0;
         printf("hexfile %s\n", argv[2]);
         while ((n = fread(b, 1, sizeof(b), f)) > 0) {
            char line[2 * sizeof(b) + 1];
            for (size_t i = 0; i < n; i++) sprintf(line + 2 * i, "%02x", b[i]);
            printf("hex %zx %s\n", off, line);
            off += n;
         }
         printf("hexend %zu\n", off);
         fclose(f);
         return 0;
      }
      printf("usage: a9ybbctl status|install|restore|arm|disarm|respring|wait S|pids NAME|cat FILE [MAX]|hex FILE\n");
      return 2;
   }
}
