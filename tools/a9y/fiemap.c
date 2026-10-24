/*
 * fiemap FILE: print a file's extents, one per line, as
 *   <logical byte> <physical byte> <length> <flags>   (hex)
 * via FS_IOC_FIEMAP, for placing the XNU root's raw blocks (Android has no
 * filefrag). Static, raw syscalls: no libc needed on the phone.
 *   clang --target=aarch64-linux-gnu -static -nostdlib -ffreestanding -O2 \
 *         -fno-stack-protector fiemap.c -o fiemap
 */
typedef unsigned long u64;
typedef unsigned int u32;

#define SYS_openat	56
#define SYS_ioctl	29
#define SYS_write	64
#define SYS_exit	93
#define AT_FDCWD	-100
#define FS_IOC_FIEMAP	0xc020660bUL
#define FIEMAP_FLAG_SYNC 1
#define MAX_EXTENTS	2048

static long sys(long n, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

struct extent {
	u64 logical, physical, length, reserved64[2];
	u32 flags, reserved[3];
};

static struct {
	u64 start, length;
	u32 flags, mapped, count, reserved;
	struct extent ext[MAX_EXTENTS];
} fm;

static char out[64];

static void put_hex(char **p, u64 v)
{
	char tmp[16];
	int n = 0;
	do
		tmp[n++] = "0123456789abcdef"[v & 15];
	while (v >>= 4);
	*(*p)++ = '0';
	*(*p)++ = 'x';
	while (n)
		*(*p)++ = tmp[--n];
}

static void say(const char *s)
{
	long n = 0;
	while (s[n])
		n++;
	sys(SYS_write, 2, (long)s, n, 0);
}

void _start_c(long argc, char **argv)
{
	if (argc != 2) {
		say("usage: fiemap FILE\n");
		sys(SYS_exit, 2, 0, 0, 0);
	}
	long fd = sys(SYS_openat, AT_FDCWD, (long)argv[1], 0, 0);
	if (fd < 0) {
		say("fiemap: cannot open\n");
		sys(SYS_exit, 1, 0, 0, 0);
	}
	fm.length = ~0UL;
	fm.flags = FIEMAP_FLAG_SYNC;
	fm.count = MAX_EXTENTS;
	if (sys(SYS_ioctl, fd, FS_IOC_FIEMAP, (long)&fm, 0) < 0) {
		say("fiemap: FS_IOC_FIEMAP failed\n");
		sys(SYS_exit, 1, 0, 0, 0);
	}
	for (u32 i = 0; i < fm.mapped; i++) {
		char *p = out;
		put_hex(&p, fm.ext[i].logical);
		*p++ = ' ';
		put_hex(&p, fm.ext[i].physical);
		*p++ = ' ';
		put_hex(&p, fm.ext[i].length);
		*p++ = ' ';
		put_hex(&p, fm.ext[i].flags);
		*p++ = '\n';
		sys(SYS_write, 1, (long)out, p - out, 0);
	}
	sys(SYS_exit, fm.mapped == MAX_EXTENTS ? 3 : 0, 0, 0, 0);
}

/* the kernel enters with sp -> argc, argv[] */
__asm__(".globl _start\n_start:\n\tldr x0, [sp]\n\tadd x1, sp, #8\n\tbl _start_c\n");
