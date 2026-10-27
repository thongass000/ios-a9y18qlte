/*
 * a9ybb: backboardd's launchd program on a9y18qlte (a9ybbctl install points the job here). It runs
 * /usr/libexec/backboardd in the same process (exec: same pid, launchd's ports, backboardd's own entitlements), and
 * only when this boot was armed does it add Chlorophyll:
 *
 *   DYLD_INSERT_LIBRARIES=/usr/local/chl/bgmtl-inject.dylib BGMTL_BB=1
 *
 * Armed = /private/var/tmp/a9ybb.arm holds this boot's kern.bootsessionuuid (a9ybbctl arm writes it), the injection's
 * kill switch /private/var/tmp/bgmtl-bb.off is absent, and the dylib is there. So every boot starts stock, and a
 * launch the injection gave up on (its dead-man writes bgmtl-bb.off and exits) comes back stock from launchd.
 * A crash is not something the injection sees, so this counts (as Bergamota's bgl-launch does): armed launches since
 * the injection last wrote "alive=1" to bgmtl-bb.state are in /private/var/tmp/a9ybb.tries, and after MAX_TRIES of
 * them this writes bgmtl-bb.off itself (backboardd crashed on its first frame, ten times a boot, with none of this).
 * Anything unexpected: plain backboardd.
 */
#include <stddef.h>

typedef long ssize_t;

extern int open(const char *, int, ...);
extern int close(int);
extern ssize_t read(int, void *, size_t);
extern ssize_t write(int, const void *, size_t);
extern int access(const char *, int);
extern int execve(const char *, char *const[], char *const[]);
extern int sysctlbyname(const char *, void *, size_t *, void *, size_t);
extern char **environ;

#define O_RDONLY        0
#define O_WRONLY        1
#define O_CREAT         0x200
#define O_TRUNC         0x400
#define F_OK            0
#define BACKBOARDD      "/usr/libexec/backboardd"
#define ARM             "/private/var/tmp/a9ybb.arm"
#define OFF             "/private/var/tmp/bgmtl-bb.off"
#define INJECT          "/usr/local/chl/bgmtl-inject.dylib"
#define STATE           "/private/var/tmp/bgmtl-bb.state"
#define TRIES           "/private/var/tmp/a9ybb.tries"
#define ENV_FILE        "/usr/local/etc/a9ybb.env"
#define MAX_TRIES       2
#define MAX_ENV         256

static int slurp(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return (int)n;
}

static void put(const char *path, const char *text, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd >= 0) {
		write(fd, text, len);
		close(fd);
	}
}

/* 1: this launch may take the injection (and is counted); 0: too many launches without "alive=1": kill switch */
static int another_try(void)
{
	char buf[128];
	int tries = 0;
	if (slurp(STATE, buf, sizeof(buf)) > 0)
		for (char *p = buf; *p; p++)
			if (p[0] == 'a' && p[1] == 'l' && p[2] == 'i' && p[3] == 'v' && p[4] == 'e' && p[5] == '=' && p[6] == '1')
				return 1;       /* healthy once this boot: every relaunch may try again */
	if (slurp(TRIES, buf, sizeof(buf)) > 0 && buf[0] >= '0' && buf[0] <= '9')
		tries = buf[0] - '0';
	if (tries >= MAX_TRIES) {
		static const char why[] = "a9ybb: armed launches crashed before the injection called them healthy\n";
		put(OFF, why, sizeof(why) - 1);
		return 0;
	}
	char n = (char)('0' + tries + 1);
	put(TRIES, &n, 1);
	return 1;
}

static int armed(void)
{
	char boot[64], arm[64];
	size_t len = sizeof(boot) - 1;
	if (sysctlbyname("kern.bootsessionuuid", boot, &len, 0, 0) || len < 8)
		return 0;
	boot[len] = 0;
	int fd = open(ARM, O_RDONLY);
	if (fd < 0)
		return 0;
	ssize_t n = read(fd, arm, sizeof(arm) - 1);
	close(fd);
	if (n < 8)
		return 0;
	arm[n] = 0;
	for (size_t i = 0; boot[i]; i++)
		if (arm[i] != boot[i])
			return 0;
	return access(OFF, F_OK) != 0 && access(INJECT, F_OK) == 0;
}

int main(int argc, char **argv)
{
	static char *argv_bb[] = { BACKBOARDD, 0 };
	char **av = argc > 0 ? argv : argv_bb;
	av[0] = BACKBOARDD;
	if (armed() && another_try()) {
		static char insert[] = "DYLD_INSERT_LIBRARIES=" INJECT;
		static char bb[] = "BGMTL_BB=1";
		/* A/B switches that come with the drop file (/private/var/tmp is out of its reach): one line of
		 * NAME=VALUE in /usr/local/etc/a9ybb.env, e.g. FD_MESA_DEBUG=sysmem */
		static char extra[256];
		char *envp[MAX_ENV + 4];
		int n = 0;
		envp[n++] = insert;
		envp[n++] = bb;
		if (slurp(ENV_FILE, extra, sizeof(extra)) > 2) {
			for (char *p = extra; *p; p++)
				if (*p == '\n' || *p == '\r')
					*p = 0;
			if (extra[0] && extra[0] != '#')
				envp[n++] = extra;
		}
		for (char **e = environ; *e && n < MAX_ENV; e++)
			envp[n++] = *e;
		envp[n] = 0;
		execve(BACKBOARDD, av, envp);
	}
	execve(BACKBOARDD, av, environ);
	return 127;
}
