/*
 * a9yrun: the boot-time test runner. a9ydrop execs /usr/local/bin/a9ytest; install this as that, and the tests to
 * run become a list, /usr/local/etc/a9yrun.txt, pushed with them through the drop file:
 *
 *   cd DIR                  working directory for the next runs
 *   env NAME=VALUE          added to the environment of the next runs
 *   clearenv                forget the env lines so far
 *   timeout SECONDS         from the next run on, kill a run still going after that long (0: wait for ever)
 *   run PROGRAM [ARG...]    run it, wait for it (arguments split on spaces)
 *   # ...                   comment
 *
 * Each program starts suspended, gets a 1 GB memory limit that is not fatal (launchd's default for a spawned
 * process killed Mesa: reason namespace 1, code 7), then runs with stdout and stderr on a pipe whose lines go to
 * SWEETLOG through IOKit's path lookup log (sweetiolog= must name a9ytest).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

typedef unsigned int mach_port_t;
typedef mach_port_t io_object_t;
typedef long ssize_t;
typedef int pid_t;
typedef void *posix_spawnattr_t;
typedef void *posix_spawn_file_actions_t;

extern int printf(const char *, ...);
extern int vsnprintf(char *, size_t, const char *, va_list);
extern io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern int IOObjectRelease(io_object_t object);
extern int open(const char *, int, ...);
extern int close(int);
extern ssize_t read(int, void *, size_t);
extern int pipe(int[2]);
extern int chdir(const char *);
extern int kill(pid_t, int);
extern pid_t waitpid(pid_t, int *, int);
extern long time(long *);
struct pollfd { int fd; short events, revents; };
extern int poll(struct pollfd *, unsigned int, int);
extern int getpid(void);
extern int *__error(void);
extern char **environ;
extern int posix_spawnattr_init(posix_spawnattr_t *);
extern int posix_spawnattr_setflags(posix_spawnattr_t *, short);
extern int posix_spawnattr_destroy(posix_spawnattr_t *);
extern int posix_spawn_file_actions_init(posix_spawn_file_actions_t *);
extern int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *, int, int);
extern int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *, int);
extern int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *);
extern int posix_spawn(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *,
		       char *const[], char *const[]);
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize);

#define O_RDONLY                        0
#define SIGCONT                         19
#define SIGKILL                         9
#define POLLIN                          0x0001
#define POSIX_SPAWN_START_SUSPENDED     0x0080
#define MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES 7
#define LIST                            "/usr/local/etc/a9yrun.txt"
#define MAX_ENV                         64
#define MAX_ARGS                        32

static void say(const char *fmt, ...)
{
	char line[300] = "IOService:/a9ytest ";
	size_t pre = sizeof("IOService:/a9ytest ") - 1;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + pre, sizeof(line) - pre, fmt, ap);
	va_end(ap);
	printf("%s\n", line + pre);
	io_object_t o = IORegistryEntryFromPath(0, line);
	if (o)
		IOObjectRelease(o);
}

static int strncmp_(const char *a, const char *b, size_t n)
{
	for (; n && *a && *a == *b; n--, a++, b++)
		;
	return n ? *a - *b : 0;
}

static int strcmp_(const char *a, const char *b)
{
	return strncmp_(a, b, (size_t)-1);
}

static int limit(pid_t pid)
{
	struct { int32_t active; uint32_t active_attr; int32_t inactive; uint32_t inactive_attr; } lim = { 1024, 0, 1024, 0 };
	return memorystatus_control(MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, pid, 0, &lim, sizeof(lim));
}

static char *env[MAX_ENV + 1];
static int nenv;
static long timeout_s;

static void run(char **argv)
{
	int p[2];
	pid_t pid;
	posix_spawnattr_t attr;
	posix_spawn_file_actions_t fa;
	char *envp[MAX_ENV * 2 + 1];
	int n = 0;

	for (int i = 0; i < nenv; i++)
		envp[n++] = env[i];
	for (char **e = environ; *e && n < MAX_ENV * 2; e++)
		envp[n++] = *e;
	envp[n] = 0;
	if (pipe(p)) {
		say("run %s: no pipe", argv[0]);
		return;
	}
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_START_SUSPENDED);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_adddup2(&fa, p[1], 2);
	posix_spawn_file_actions_addclose(&fa, p[0]);
	posix_spawn_file_actions_addclose(&fa, p[1]);
	int r = posix_spawn(&pid, argv[0], &fa, &attr, argv, envp);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);
	close(p[1]);
	if (r) {
		say("run %s: spawn failed (%d)", argv[0], r);
		close(p[0]);
		return;
	}
	int lr = limit(pid);
	say("run %s: pid %d%s", argv[0], pid, lr ? " (memory limit refused)" : "");
	kill(pid, SIGCONT);

	char buf[1024], line[280];
	size_t len = 0;
	ssize_t got;
	long deadline = timeout_s ? time(0) + timeout_s : 0;
	for (;;) {
		if (deadline) {
			struct pollfd pf = { p[0], POLLIN, 0 };
			long left = deadline - time(0);
			if (left <= 0 || poll(&pf, 1, (int)left * 1000) == 0) {
				say("run %s: still going after %ld s: killed", argv[0], timeout_s);
				kill(pid, SIGKILL);
				break;
			}
		}
		if ((got = read(p[0], buf, sizeof(buf))) <= 0)
			break;
		for (ssize_t i = 0; i < got; i++) {
			if (buf[i] == '\n' || len == sizeof(line) - 1) {
				line[len] = 0;
				say("| %s", line);
				len = 0;
			}
			if (buf[i] != '\n')
				line[len++] = buf[i];
		}
	}
	if (len) {
		line[len] = 0;
		say("| %s", line);
	}
	close(p[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	if ((status & 0x7f) == 0)
		say("run %s: exit %d", argv[0], (status >> 8) & 0xff);
	else
		say("run %s: killed by signal %d", argv[0], status & 0x7f);
}

int main(void)
{
	static char text[16384];
	int fd = open(LIST, O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;

	if (fd >= 0)
		close(fd);
	limit(getpid());
	if (n <= 0) {
		say("a9yrun: no %s", LIST);
		return 1;
	}
	text[n] = 0;
	say("a9yrun: %s, %ld bytes", LIST, (long)n);
	for (char *l = text; *l;) {
		char *end = l;
		while (*end && *end != '\n')
			end++;
		char *next = *end ? end + 1 : end;
		*end = 0;
		while (*l == ' ' || *l == '\t')
			l++;
		if (l[0] == 'c' && l[1] == 'd' && l[2] == ' ') {
			if (chdir(l + 3))
				say("cd %s: failed (errno %d)", l + 3, *__error());
		} else if (l[0] == 'e' && l[1] == 'n' && l[2] == 'v' && l[3] == ' ') {
			if (nenv < MAX_ENV)
				env[nenv++] = l + 4;
		} else if (!strcmp_(l, "clearenv")) {
			nenv = 0;
		} else if (!strncmp_(l, "timeout ", 8)) {
			timeout_s = 0;
			for (char *d = l + 8; *d >= '0' && *d <= '9'; d++)
				timeout_s = timeout_s * 10 + (*d - '0');
		} else if (l[0] == 'r' && l[1] == 'u' && l[2] == 'n' && l[3] == ' ') {
			char *argv[MAX_ARGS + 1];
			int argc = 0;
			for (char *a = l + 4; *a && argc < MAX_ARGS;) {
				while (*a == ' ')
					*a++ = 0;
				if (!*a)
					break;
				argv[argc++] = a;
				while (*a && *a != ' ')
					a++;
			}
			argv[argc] = 0;
			if (argc)
				run(argv);
		} else if (*l && *l != '#') {
			say("a9yrun: ignored line: %s", l);
		}
		l = next;
	}
	say("a9yrun: done");
	return 0;
}
