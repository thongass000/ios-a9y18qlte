/*
 * a9ydrop: install files into the iOS root from inside iOS, so a test needs no HFS+ change from Linux.
 *
 * /usr/local/drop.bin is a file of fixed size and fixed blocks, made once (a9y-gputest-job.py --drop). Linux writes
 * an archive straight into its blocks on sda29 (tools/a9y/drop/push-drop.py); file size and place never change, so
 * that touches no HFS+ metadata. At boot launchd runs this program: if the archive's sequence number is new, every
 * file in it is written (temp file + rename) under /usr/local, then the sequence is kept in /usr/local/.a9ydrop-seq.
 * Then it runs /usr/local/bin/a9ygputest, if there is one, so the test always sees the files of this boot.
 *
 * Archive (little endian, 8-byte aligned): "A9YDROP1", u64 seq, u32 nfiles, u32 0, u64 total bytes; per file
 * u32 path length, u32 mode, u64 size, the path (absolute, under /usr/local/, no "..") and the data, each padded to 8.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

typedef unsigned int mach_port_t;
typedef mach_port_t io_object_t;
typedef long ssize_t;
typedef long long off_t;

extern int printf(const char *, ...);
extern int vsnprintf(char *, size_t, const char *, va_list);
extern int snprintf(char *, size_t, const char *, ...);
extern io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern int IOObjectRelease(io_object_t object);
extern int open(const char *, int, ...);
extern int close(int);
extern ssize_t pread(int, void *, size_t, off_t);
extern ssize_t write(int, const void *, size_t);
extern int fsync(int);
extern int rename(const char *, const char *);
extern int unlink(const char *);
extern int mkdir(const char *, unsigned short);
extern int execv(const char *, char *const[]);
extern int access(const char *, int);
extern void *malloc(size_t);
extern void free(void *);
extern int *__error(void);

#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_CREAT         0x0200
#define O_TRUNC         0x0400
#define X_OK            1

#define DROP            "/usr/local/drop.bin"
#define SEQ_FILE        "/usr/local/.a9ydrop-seq"
#define TEST            "/usr/local/bin/a9ygputest"
#define PREFIX          "/usr/local/"

static void say(const char *fmt, ...)
{
	char line[256] = "IOService:/a9ydrop ";
	size_t pre = sizeof("IOService:/a9ydrop ") - 1;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + pre, sizeof(line) - pre, fmt, ap);
	va_end(ap);
	printf("%s\n", line + pre);
	io_object_t o = IORegistryEntryFromPath(0, line);
	if (o)
		IOObjectRelease(o);
}

static int rd(int fd, void *p, size_t n, uint64_t off)
{
	return pread(fd, p, n, (off_t)off) == (ssize_t)n ? 0 : -1;
}

static uint64_t pad8(uint64_t v)
{
	return (v + 7) & ~7ull;
}

/* every directory of path below /usr/local, so a file can go anywhere there */
static void mkdirs(char *path)
{
	for (char *p = path + sizeof(PREFIX) - 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(path, 0755);
			*p = '/';
		}
	}
}

static int path_ok(const char *p, uint32_t n)
{
	if (n <= sizeof(PREFIX) - 1 || n >= 240)
		return 0;
	for (uint32_t i = 0; i < sizeof(PREFIX) - 1; i++)
		if (p[i] != PREFIX[i])
			return 0;
	for (uint32_t i = 0; i < n; i++) {
		if (p[i] == 0)
			return 0;
		if (p[i] == '.' && i > 0 && p[i - 1] == '/' && i + 1 < n && p[i + 1] == '.')
			return 0;
	}
	return 1;
}

static int install(int fd, uint64_t total, uint32_t nfiles)
{
	enum { BUF = 1 << 20 };
	char *buf = malloc(BUF);
	uint64_t off = 32;
	int bad = 0;

	if (!buf)
		return -1;
	for (uint32_t f = 0; f < nfiles; f++) {
		struct { uint32_t plen, mode; uint64_t size; } h;
		char path[256], tmp[272];
		if (off + sizeof(h) > total || rd(fd, &h, sizeof(h), off) || h.plen >= sizeof(path) ||
		    off + sizeof(h) + pad8(h.plen) + h.size > total) {
			say("file %u: bad header at %llu", f, (unsigned long long)off);
			bad++;
			break;
		}
		off += sizeof(h);
		if (rd(fd, path, h.plen, off) || !path_ok(path, h.plen)) {
			say("file %u: refused path", f);
			bad++;
			break;
		}
		path[h.plen] = 0;
		off += pad8(h.plen);
		mkdirs(path);
		snprintf(tmp, sizeof(tmp), "%s.a9ydrop", path);
		int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, h.mode & 07777);
		uint64_t left = h.size, at = off;
		while (out >= 0 && left) {
			size_t n = left < BUF ? (size_t)left : BUF;
			if (rd(fd, buf, n, at) || write(out, buf, n) != (ssize_t)n)
				break;
			left -= n;
			at += n;
		}
		if (out < 0 || left || fsync(out)) {
			say("%s: write failed (errno %d)", path, *__error());
			if (out >= 0)
				close(out);
			unlink(tmp);
			bad++;
		} else {
			close(out);
			if (rename(tmp, path)) {
				say("%s: rename failed (errno %d)", path, *__error());
				bad++;
			} else {
				say("installed %s (%llu bytes, mode %o)", path, (unsigned long long)h.size, h.mode & 07777);
			}
		}
		off += pad8(h.size);
	}
	free(buf);
	return bad ? -1 : 0;
}

int main(void)
{
	struct { char magic[8]; uint64_t seq; uint32_t nfiles, zero; uint64_t total; } hd;
	uint64_t last = 0;
	int fd = open(DROP, O_RDONLY);

	if (fd < 0 || rd(fd, &hd, sizeof(hd), 0)) {
		say("no %s", DROP);
	} else if (hd.magic[0] != 'A' || hd.magic[1] != '9' || hd.magic[2] != 'Y' || hd.magic[3] != 'D' ||
		   hd.magic[4] != 'R' || hd.magic[5] != 'O' || hd.magic[6] != 'P' || hd.magic[7] != '1') {
		say("drop is empty");
	} else {
		int sf = open(SEQ_FILE, O_RDONLY);
		if (sf >= 0) {
			rd(sf, &last, sizeof(last), 0);
			close(sf);
		}
		if (hd.seq == last) {
			say("drop %llu already installed", (unsigned long long)hd.seq);
		} else if (install(fd, hd.total, hd.nfiles) == 0) {
			sf = open(SEQ_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (sf >= 0) {
				write(sf, &hd.seq, sizeof(hd.seq));
				fsync(sf);
				close(sf);
			}
			say("drop %llu: %u files installed", (unsigned long long)hd.seq, hd.nfiles);
		} else {
			say("drop %llu: install FAILED, will retry next boot", (unsigned long long)hd.seq);
		}
	}
	if (fd >= 0)
		close(fd);
	if (access(TEST, X_OK) == 0) {
		char *argv[] = { TEST, 0 };
		execv(TEST, argv);
		say("exec %s failed (errno %d)", TEST, *__error());
	}
	return 0;
}
