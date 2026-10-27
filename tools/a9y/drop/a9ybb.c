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
 * Anything unexpected: plain backboardd.
 */
#include <stddef.h>

typedef long ssize_t;

extern int open(const char *, int, ...);
extern int close(int);
extern ssize_t read(int, void *, size_t);
extern int access(const char *, int);
extern int execve(const char *, char *const[], char *const[]);
extern int sysctlbyname(const char *, void *, size_t *, void *, size_t);
extern char **environ;

#define O_RDONLY        0
#define F_OK            0
#define BACKBOARDD      "/usr/libexec/backboardd"
#define ARM             "/private/var/tmp/a9ybb.arm"
#define OFF             "/private/var/tmp/bgmtl-bb.off"
#define INJECT          "/usr/local/chl/bgmtl-inject.dylib"
#define MAX_ENV         256

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
	if (armed()) {
		static char insert[] = "DYLD_INSERT_LIBRARIES=" INJECT;
		static char bb[] = "BGMTL_BB=1";
		char *envp[MAX_ENV + 3];
		int n = 0;
		envp[n++] = insert;
		envp[n++] = bb;
		for (char **e = environ; *e && n < MAX_ENV; e++)
			envp[n++] = *e;
		envp[n] = 0;
		execve(BACKBOARDD, av, envp);
	}
	execve(BACKBOARDD, av, environ);
	return 127;
}
