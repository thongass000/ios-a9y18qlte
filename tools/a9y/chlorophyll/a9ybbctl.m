// a9y18qlte: Chlorophyll in backboardd, the control side (run as root from a9yrun's list; a9ybb is the launch side).
//   a9ybbctl status          backboardd's job in launchd.plist, the backup, arm / kill-switch files
//   a9ybbctl install         backboardd's job runs /usr/local/bin/a9ybb (from the next boot; launchd reads the plist
//                            at boot); the original plist is kept as /usr/local/etc/launchd.plist.orig
//   a9ybbctl restore         put the original plist back (from the next boot)
//   a9ybbctl trim            take daemons this phone has nothing for out of launchd.plist (from the next boot)
//   a9ybbctl arm             this boot: the next backboardd launch gets Chlorophyll (a9ybb.arm = this boot's id, the
//                            injection's bgmtl-bb.arm, its kill switch and state cleared, its log made writable by mobile)
//   a9ybbctl disarm          launches from now on are stock
//   a9ybbctl respring        SIGTERM to backboardd: launchd starts it again, through a9ybb
//   a9ybbctl wait SECONDS
//   a9ybbctl pids NAME       the pids of processes called NAME
//   a9ybbctl threads NAME S  where NAME's CPU time goes over S seconds, per thread
//   a9ybbctl hex FILE        a file as hex lines (a binary back through the log)
//   a9ybbctl bench           the cores' clocks (dependent adds), one thread and one per core
//   a9ybbctl top S           every process's CPU time over S seconds, the busiest first
//   a9ybbctl ls DIR [MAX]    a directory, newest first
//   a9ybbctl cat FILE [MAX]  print a file (the injection logs to /private/var/tmp/bgmtl.log), at most MAX lines
// The plist is changed on the phone, by CoreFoundation's own writer and the phone's own file system, never from Linux.
#import <Foundation/Foundation.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <mach/mach.h>

// a9ybbctl bench: a core's clock from a chain of dependent adds (one cycle each); the cpu from TPIDRRO_EL0's low bits
struct bench { unsigned cpu0, cpu1; double mhz; };

static unsigned
cur_cpu(void)
{
   uint64_t v;
   __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(v));
   return (unsigned)(v & 7);
}

static void *
bench_thread(void *arg)
{
   struct bench *b = arg;
   mach_timebase_info_data_t tb;
   mach_timebase_info(&tb);
   b->cpu0 = cur_cpu();
   uint64_t t0 = mach_absolute_time(), adds = 0, x = 0;
   while ((mach_absolute_time() - t0) * tb.numer / tb.denom < 400000000ull) {
      for (int i = 0; i < 4096; i++)
         __asm__ volatile("add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                          "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n" : "+r"(x));
      adds += 4096 * 8;
   }
   double ns = (double)((mach_absolute_time() - t0) * tb.numer / tb.denom);
   b->cpu1 = cur_cpu();
   b->mhz = adds / ns * 1000.0;
   return NULL;
}

// per-CPU ticks (user, system, idle, nice), to see which cores ran the bench
static unsigned
cpu_ticks(unsigned (*out)[4], unsigned max)
{
   natural_t ncpu = 0;
   processor_info_array_t info = NULL;
   mach_msg_type_number_t cnt = 0;
   if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &ncpu, &info, &cnt) != KERN_SUCCESS)
      return 0;
   for (unsigned c = 0; c < ncpu && c < max; c++)
      for (unsigned s = 0; s < 4; s++) out[c][s] = ((processor_cpu_load_info_t)info)[c].cpu_ticks[s];
   vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(integer_t));
   return ncpu < max ? ncpu : max;
}

static void
bench(int n)
{
   pthread_t t[8];
   struct bench b[8];
   unsigned before[16][4], after[16][4];
   unsigned nc = cpu_ticks(before, 16);
   for (int i = 0; i < n; i++) pthread_create(&t[i], NULL, bench_thread, &b[i]);
   for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
   cpu_ticks(after, 16);
   printf("bench %d thread%s:", n, n > 1 ? "s" : "");
   for (int i = 0; i < n; i++) printf(" %.0f", b[i].mhz);
   printf(" MHz; busy ticks per cpu (user+sys / idle):");
   for (unsigned c = 0; c < nc; c++)
      printf(" %u:%u/%u", c, after[c][CPU_STATE_USER] + after[c][CPU_STATE_SYSTEM] - before[c][CPU_STATE_USER] -
             before[c][CPU_STATE_SYSTEM], after[c][CPU_STATE_IDLE] - before[c][CPU_STATE_IDLE]);
   printf("\n");
}

// libproc (in libSystem; the iOS SDK has no libproc.h)
extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
#define PROC_PIDTHREADINFO 5
#define PROC_PIDLISTTHREADS 6
#define PROC_PIDTASKINFO 4
struct proc_taskinfo {   // sys/proc_info.h
   uint64_t pti_virtual_size, pti_resident_size, pti_total_user, pti_total_system, pti_threads_user,
      pti_threads_system;
   int32_t pti_policy, pti_faults, pti_pageins, pti_cow_faults, pti_messages_sent, pti_messages_received,
      pti_syscalls_mach, pti_syscalls_unix, pti_csw, pti_threadnum, pti_numrunning, pti_priority;
};
struct proc_threadinfo {   // sys/proc_info.h
   uint64_t pth_user_time, pth_system_time;   // ns
   int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority;
   char pth_name[64];
};

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
      if (!strcmp(cmd, "trim")) {
         /* Daemons this phone has nothing for, which under XNU respawned or spun and kept 3-4 cores busy with nothing
          * on screen (a9ybbctl top: launchd, ReportCrash, vmd, voiced's synthesizer extensions): the Apple Watch face
          * companion (crashed every ~10 s), Visual Voicemail (no telephony), Siri and VoiceOver speech. Taken out of
          * launchd's job table from the next boot; restore puts the original back. */
         static NSString *const trim[] = {
            @"/System/Library/LaunchDaemons/com.apple.nanotimekitcompaniond.plist",
            @"/System/Library/LaunchDaemons/com.apple.voicemail.vmd.plist",
            @"/System/Library/LaunchDaemons/com.apple.sirittsd.plist",
            @"/System/Library/LaunchDaemons/com.apple.voiced.plist",
         };
         NSMutableDictionary *p = load(PLIST);
         if (!p || !p[@"LaunchDaemons"]) { printf("no LaunchDaemons in %s: nothing changed\n", PLIST); return 1; }
         if (access(BACKUP, F_OK) != 0) {
            NSData *d = [NSData dataWithContentsOfFile:@PLIST];
            if (![d writeToFile:@BACKUP atomically:YES]) { printf("cannot keep %s: nothing changed\n", BACKUP); return 1; }
            printf("kept the original as %s\n", BACKUP);
         }
         int removed = 0;
         for (unsigned i = 0; i < sizeof(trim) / sizeof(trim[0]); i++) {
            BOOL there = p[@"LaunchDaemons"][trim[i]] != nil;
            printf("%s: %s\n", trim[i].UTF8String, there ? "removed" : "not there");
            if (there) { [p[@"LaunchDaemons"] removeObjectForKey:trim[i]]; removed++; }
         }
         return removed ? save(p, PLIST) : 0;
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
      if (!strcmp(cmd, "ls") && argc > 2) {   // a directory, newest first (crash reports name what crashed)
         NSFileManager *fm = [NSFileManager defaultManager];
         NSArray *names = [fm contentsOfDirectoryAtPath:@(argv[2]) error:NULL];
         if (!names) { printf("%s: not readable\n", argv[2]); return 1; }
         NSMutableArray *rows = [NSMutableArray array];
         for (NSString *n in names) {
            NSDictionary *a = [fm attributesOfItemAtPath:[@(argv[2]) stringByAppendingPathComponent:n] error:NULL];
            if (a) [rows addObject:@[ a[NSFileModificationDate] ?: [NSDate distantPast], n, a[NSFileSize] ?: @0 ]];
         }
         [rows sortUsingComparator:^NSComparisonResult(NSArray *x, NSArray *y) { return [y[0] compare:x[0]]; }];
         int max = argc > 3 ? atoi(argv[3]) : 40;
         printf("%s: %lu entries\n", argv[2], (unsigned long)rows.count);
         for (NSArray *r in rows) {
            if (max-- <= 0) break;
            printf("  %s %8llu %s\n", [[r[0] description] UTF8String], [r[2] unsignedLongLongValue], [r[1] UTF8String]);
         }
         return 0;
      }
      if (!strcmp(cmd, "top") && argc > 2) {   // every process's CPU time over SECONDS, the busiest first
         enum { MAXP = 1024 };
         static pid_t pid[MAXP];
         static uint64_t t0[MAXP], d[MAXP];
         int n = proc_listallpids(pid, sizeof(pid));
         if (n > MAXP) n = MAXP;
         for (int i = 0; i < n; i++) {
            struct proc_taskinfo ti;
            t0[i] = proc_pidinfo(pid[i], PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) == sizeof(ti)
                       ? ti.pti_total_user + ti.pti_total_system : 0;
         }
         unsigned secs = (unsigned)atoi(argv[2]);
         sleep(secs);
         uint64_t total = 0;
         mach_timebase_info_data_t tb;   /* task times are mach absolute time */
         mach_timebase_info(&tb);
         for (int i = 0; i < n; i++) {
            struct proc_taskinfo ti;
            d[i] = proc_pidinfo(pid[i], PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) == sizeof(ti) &&
                   ti.pti_total_user + ti.pti_total_system >= t0[i] ? ti.pti_total_user + ti.pti_total_system - t0[i] : 0;
            d[i] = d[i] * tb.numer / tb.denom;
            total += d[i];
         }
         printf("top: %llu ms CPU in %u s over %d processes (%u%% of one core)\n", (unsigned long long)(total / 1000000),
                secs, n, secs ? (unsigned)(total / 10000000 / secs) : 0);
         for (int k = 0; k < 15; k++) {
            int best = -1;
            for (int i = 0; i < n; i++) if (d[i] && (best < 0 || d[i] > d[best])) best = i;
            if (best < 0) break;
            char nm[64] = "?";
            proc_name(pid[best], nm, sizeof(nm));
            printf("  %6llu ms  %s (%d)\n", (unsigned long long)(d[best] / 1000000), pid[best] ? nm : "kernel_task", pid[best]);
            d[best] = 0;
         }
         return 0;
      }
      if (!strcmp(cmd, "bench")) {   // the cores' clocks: one thread alone, then one per core at once
         bench(1);
         bench(4);
         bench(8);
         return 0;
      }
      if (!strcmp(cmd, "threads") && argc > 3) {   // where a process's CPU time goes: per thread over SECONDS
         pid_t pp[1];
         if (pids(argv[2], pp, 1) < 1) { printf("no %s running\n", argv[2]); return 1; }
         enum { MAXT = 256 };
         static uint64_t h0[MAXT], h1[MAXT], t0[MAXT];
         static char nm[MAXT][64];
         int n0 = proc_pidinfo(pp[0], PROC_PIDLISTTHREADS, 0, h0, sizeof(h0)) / 8;
         for (int i = 0; i < n0; i++) {
            struct proc_threadinfo ti;
            t0[i] = proc_pidinfo(pp[0], PROC_PIDTHREADINFO, h0[i], &ti, sizeof(ti)) == sizeof(ti)
                       ? ti.pth_user_time + ti.pth_system_time : 0;
         }
         unsigned secs = (unsigned)atoi(argv[3]);
         sleep(secs);
         int n1 = proc_pidinfo(pp[0], PROC_PIDLISTTHREADS, 0, h1, sizeof(h1)) / 8;
         uint64_t total = 0, d[MAXT];
         for (int i = 0; i < n1; i++) {
            struct proc_threadinfo ti;
            d[i] = 0;
            nm[i][0] = 0;
            if (proc_pidinfo(pp[0], PROC_PIDTHREADINFO, h1[i], &ti, sizeof(ti)) != sizeof(ti)) continue;
            uint64_t before = 0;
            for (int j = 0; j < n0; j++) if (h0[j] == h1[i]) before = t0[j];
            d[i] = ti.pth_user_time + ti.pth_system_time - before;
            snprintf(nm[i], sizeof(nm[i]), "%s", ti.pth_name[0] ? ti.pth_name : "-");
            total += d[i];
         }
         printf("%s pid %d: %llu ms CPU in %u s (%u%% of one core), %d threads\n", argv[2], pp[0],
                (unsigned long long)(total / 1000000), secs, secs ? (unsigned)(total / 10000000 / secs) : 0, n1);
         for (int k = 0; k < 12; k++) {   // the busiest first
            int best = -1;
            for (int i = 0; i < n1; i++) if (d[i] && (best < 0 || d[i] > d[best])) best = i;
            if (best < 0) break;
            printf("  %6llu ms  %s (thread %llx)\n", (unsigned long long)(d[best] / 1000000), nm[best],
                   (unsigned long long)h1[best]);
            d[best] = 0;
         }
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
      printf("usage: a9ybbctl status|install|restore|trim|arm|disarm|respring|wait S|pids NAME|cat FILE [MAX]|hex FILE|threads NAME S|bench|top S|ls DIR [MAX]\n");
      return 2;
   }
}
