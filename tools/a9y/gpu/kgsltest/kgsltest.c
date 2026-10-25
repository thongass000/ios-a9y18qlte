// SPDX-License-Identifier: GPL-2.0
/*
 * Runs the a9y18qlte shim's GPU packet streams through Android's kgsl on a
 * GPU that Linux brought up: a CP_MEM_WRITE, then the shim's 2D-engine blit
 * (as freedreno's fd5_blitter.c emit_blit_buffer). If the blit works here
 * and not in the shim, the shim's GPU init lacks something; if it fails
 * here too, the packets are wrong.
 *
 * Freestanding (raw syscalls, no libc): run as root on the phone,
 *   kgsltest [lines]
 */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

static long sys(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4),
			 "r"(x5) : "memory");
	return x0;
}
#define SYS_openat	56
#define SYS_ioctl	29
#define SYS_mmap	222
#define SYS_write	64
#define SYS_exit	93

static void puts1(const char *s)
{
	long n = 0;
	while (s[n])
		n++;
	sys(SYS_write, 1, (long)s, n, 0, 0, 0);
}

static void putx(const char *label, u64 v)
{
	char b[32];
	int i = 31;
	b[i] = 0;
	do {
		b[--i] = "0123456789abcdef"[v & 15];
		v >>= 4;
	} while (v);
	puts1(label);
	puts1(b + i);
}

/* msm_kgsl.h */
struct kgsl_drawctxt_create { u32 flags, drawctxt_id; };
struct kgsl_gpumem_alloc_id { u32 id, flags; u64 size, mmapsize, gpuaddr, pad[2]; };
struct kgsl_command_object { u64 offset, gpuaddr, size; u32 flags, id; };
struct kgsl_gpu_command {
	u64 flags, cmdlist; u32 cmdsize, numcmds;
	u64 objlist; u32 objsize, numobjs;
	u64 synclist; u32 syncsize, numsyncs;
	u32 context_id, timestamp;
};
struct kgsl_wait { u32 context_id, timestamp, timeout; };
#define IOWR(nr, sz)	(3UL << 30 | (u64)(sz) << 16 | 0x09 << 8 | (nr))
#define IOW(nr, sz)	(1UL << 30 | (u64)(sz) << 16 | 0x09 << 8 | (nr))
#define IOCTL_DRAWCTXT_CREATE	IOWR(0x13, sizeof(struct kgsl_drawctxt_create))
#define IOCTL_GPUMEM_ALLOC_ID	IOWR(0x34, sizeof(struct kgsl_gpumem_alloc_id))
#define IOCTL_GPU_COMMAND	IOWR(0x4a, sizeof(struct kgsl_gpu_command))
#define IOCTL_WAIT_CTXTID	IOW(0x7, sizeof(struct kgsl_wait))
#define KGSL_CONTEXT_NO_GMEM_ALLOC	0x2
#define KGSL_CONTEXT_PREAMBLE		0x10	/* both required by adreno_drawctxt_create() */
#define KGSL_CMDLIST_IB			0x1

static int fd;

static volatile u32 *gpu_alloc(u64 size, u64 *gpuaddr)
{
	struct kgsl_gpumem_alloc_id a = { .size = size };
	long rc = sys(SYS_ioctl, fd, IOCTL_GPUMEM_ALLOC_ID, (long)&a, 0, 0, 0);
	if (rc) {
		putx("alloc failed ", -rc);
		puts1("\n");
		sys(SYS_exit, 1, 0, 0, 0, 0, 0);
	}
	*gpuaddr = a.gpuaddr;
	/* PROT_READ|PROT_WRITE, MAP_SHARED, offset = id pages */
	return (volatile u32 *)sys(SYS_mmap, 0, a.mmapsize, 3, 1, fd, (long)a.id << 12);
}

/* the shim's packet helpers */
static u32 pm4_parity(u32 v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^
				  (v >> 24) ^ (v >> 28)))) & 1;
}
static u32 pkt7(u32 op, u32 cnt)
{
	return 0x70000000U | cnt | pm4_parity(cnt) << 15 | op << 16 | pm4_parity(op) << 23;
}
static u32 pkt4(u32 reg, u32 cnt)
{
	return 0x40000000U | cnt | pm4_parity(cnt) << 7 | reg << 8 | pm4_parity(reg) << 27;
}
#define CP_MEM_WRITE		0x3d
#define CP_WAIT_FOR_IDLE	0x26
#define CP_EVENT_WRITE		0x46
#define CP_SET_RENDER_MODE	0x6c
#define CP_BLIT			0x2c

/* the variant under test: surface format, CP_BLIT op, render mode, rectangle */
static u32 g_fmt, g_op, g_mode, g_w, g_h, g_pitch, g_array, g_unk;

/* shim.c gpu_blit_emit() */
static u32 blit_emit(volatile u32 *r, u32 w, u64 src, u64 dst, u32 bytes, u32 line, u32 lines,
		     u64 done)
{
	r[w++] = pkt7(CP_SET_RENDER_MODE, 5);
	r[w++] = 1;
	for (int k = 0; k < 4; k++)
		r[w++] = 0;
	r[w++] = pkt4(0xe91, 5);
	for (int k = 0; k < 4; k++)
		r[w++] = 0;
	r[w++] = 0x12;
	r[w++] = pkt7(CP_WAIT_FOR_IDLE, 0);
	/* fd5_emit_sysmem_prep(): CCU invalidate, power, CCU in BYPASS */
	r[w++] = pkt7(0x1d, 1);		/* CP_SKIP_IB2_ENABLE_GLOBAL */
	r[w++] = 0;
	r[w++] = pkt7(CP_EVENT_WRITE, 1);
	r[w++] = 0x19;			/* PC_CCU_INVALIDATE_COLOR */
	r[w++] = pkt4(0xe3b0, 1);	/* PC_POWER_CNTL: a512 has 2 SP cores */
	r[w++] = 1;
	r[w++] = pkt4(0xe4f0, 1);	/* VFD_POWER_CNTL */
	r[w++] = 1;
	r[w++] = pkt7(CP_WAIT_FOR_IDLE, 0);
	r[w++] = pkt4(0x0cc7, 1);	/* RB_CCU_CNTL: BYPASS */
	r[w++] = 0x10000000;
	static const unsigned short setup[][2] = {
		{ 0xe141, 0x8 }, { 0x0cc6, 0x4 }, { 0x0ec2, 0xc }, { 0x0f02, 0x344 },
		{ 0x0e06, 0x2 }, { 0xe000, 0x181 }, { 0x2184, 0x9 },
	};
	for (unsigned k = 0; k < sizeof(setup) / sizeof(setup[0]); k++) {
		r[w++] = pkt4(setup[k][0], 1);
		r[w++] = setup[k][1];
	}
	r[w++] = pkt4(0x2100, 1);
	r[w++] = 0x86000000;
	r[w++] = pkt4(0x2180, 1);
	r[w++] = 0x86000000;
	r[w++] = pkt4(0xe140, 1);
	r[w++] = 1U << 17;
	u32 info = g_fmt, size = g_pitch >> 6 | ((g_array ? g_array : g_pitch * g_h) >> 6) << 16;
	for (u32 l = 0; l < lines; l++) {
		u64 s = src + (u64)l * line, d = dst + (u64)l * line;
		r[w++] = pkt7(CP_SET_RENDER_MODE, 1);
		r[w++] = g_mode;
		r[w++] = pkt4(0x2107, 9);
		r[w++] = info;
		r[w++] = (u32)s;
		r[w++] = (u32)(s >> 32);
		r[w++] = size;
		for (int k = 0; k < 5; k++)
			r[w++] = g_unk ? g_unk + k : 0;	/* RB_2D_SRC 0x210b..0x210f */
		r[w++] = pkt4(0x2181, 1);
		r[w++] = info;
		r[w++] = pkt4(0x2110, 9);
		r[w++] = info;
		r[w++] = (u32)d;
		r[w++] = (u32)(d >> 32);
		r[w++] = size;
		for (int k = 0; k < 5; k++)
			r[w++] = g_unk ? g_unk + 0x10 + k : 0;	/* RB_2D_DST 0x2114..0x2118 */
		r[w++] = pkt4(0x2182, 1);
		r[w++] = info;
		r[w++] = pkt7(CP_BLIT, 5);
		r[w++] = g_op;
		r[w++] = 0;
		r[w++] = (g_w - 1) | (g_h - 1) << 16;
		r[w++] = 0;
		r[w++] = (g_w - 1) | (g_h - 1) << 16;
		r[w++] = pkt7(CP_SET_RENDER_MODE, 1);
		r[w++] = 8;
		r[w++] = pkt7(CP_WAIT_FOR_IDLE, 0);
	}
	r[w++] = pkt7(CP_EVENT_WRITE, 4);
	r[w++] = 0x1d;
	r[w++] = (u32)(done + 8);
	r[w++] = (u32)((done + 8) >> 32);
	r[w++] = 0;
	r[w++] = pkt7(CP_EVENT_WRITE, 4);
	r[w++] = 0x04;
	r[w++] = (u32)done;
	r[w++] = (u32)(done >> 32);
	r[w++] = 0xb117d0e5;
	return w;
}

static void submit(u32 ctx, u64 ib, u32 dwords)
{
	struct kgsl_command_object o = { .gpuaddr = ib, .size = dwords * 4, .flags = KGSL_CMDLIST_IB };
	struct kgsl_gpu_command c = {
		.cmdlist = (u64)&o, .cmdsize = sizeof(o), .numcmds = 1, .context_id = ctx,
	};
	long rc = sys(SYS_ioctl, fd, IOCTL_GPU_COMMAND, (long)&c, 0, 0, 0);
	putx("submit rc ", rc < 0 ? -rc : rc);
	putx(" timestamp ", c.timestamp);
	struct kgsl_wait wt = { ctx, c.timestamp, 3000 };
	rc = sys(SYS_ioctl, fd, IOCTL_WAIT_CTXTID, (long)&wt, 0, 0, 0);
	putx(", wait rc ", rc < 0 ? -rc : rc);
	puts1("\n");
}

void _start(void)
{
	fd = sys(SYS_openat, -100, (long)"/dev/kgsl-3d0", 2, 0, 0, 0);
	if (fd < 0) {
		putx("open /dev/kgsl-3d0 failed ", -fd);
		puts1("\n");
		sys(SYS_exit, 1, 0, 0, 0, 0, 0);
	}
	struct kgsl_drawctxt_create cc = { .flags = KGSL_CONTEXT_PREAMBLE | KGSL_CONTEXT_NO_GMEM_ALLOC };
	long rc = sys(SYS_ioctl, fd, IOCTL_DRAWCTXT_CREATE, (long)&cc, 0, 0, 0);
	putx("context rc ", rc < 0 ? -rc : rc);
	putx(" id ", cc.drawctxt_id);
	puts1("\n");

	enum { BYTES = 8640, LINES = 4 };
	u64 gsrc, gdst, gcmd, gdone;
	volatile u32 *src = gpu_alloc(BYTES * LINES, &gsrc);
	volatile u32 *dst = gpu_alloc(BYTES * LINES, &gdst);
	volatile u32 *cmd = gpu_alloc(16384, &gcmd);
	volatile u32 *done = gpu_alloc(4096, &gdone);
	putx("src ", gsrc);
	putx(" dst ", gdst);
	putx(" cmd ", gcmd);
	putx(" done ", gdone);
	puts1("\n");
	u32 n = BYTES * LINES / 4;
	for (u32 k = 0; k < n; k++) {
		src[k] = 0x01020304 * (k + 1);
		dst[k] = 0xffff00ff;
	}
	done[0] = done[1] = 0;

	/* 1: CP_MEM_WRITE */
	u32 w = 0;
	cmd[w++] = pkt7(CP_MEM_WRITE, 3);
	cmd[w++] = (u32)gdone;
	cmd[w++] = (u32)(gdone >> 32);
	cmd[w++] = 0xa9c0ffee;
	submit(cc.drawctxt_id, gcmd, w);
	putx("mem write: done[0] ", done[0]);
	puts1(done[0] == 0xa9c0ffee ? "  OK\n" : "  FAILED\n");

	/*
	 * 2: blits. fmt RB5_R8_UNORM 0x03 / R8G8B8A8_UNORM 0x30; op COPY 1 / SCALE 3;
	 * mode BLIT2D 5 / BLIT2DSCALE 7; w, h in pixels, pitch in bytes, array
	 * pitch (0: pitch * h); bytes covered
	 */
	/* ... and a marker for the 5 unknown dwords after RB_2D_SRC/DST_SIZE (0: zeros) */
	static const u32 var[][9] = {
		{ 0x30, 3, 7, 64, 16, 256, 0, 256 * 16, 0 },		/* scale op: writes 0s */
		{ 0x30, 0, 7, 64, 16, 256, 0, 256 * 16, 0x11223300 },	/* fill */
		{ 0x30, 0, 5, 64, 16, 256, 0, 256 * 16, 0x11223300 },	/* fill, BLIT2D */
		{ 0x30, 3, 5, 64, 16, 256, 0, 256 * 16, 0 },		/* scale op, BLIT2D */
		{ 0x30, 1, 7, 64, 16, 256, 0, 256 * 16, 0 },		/* copy op, BLIT2DSCALE */
		{ 0x30, 3, 7, 64, 16, 256, 0, 256 * 16, 0x11223300 },	/* scale + markers */
	};
	for (u32 v = 0; v < sizeof(var) / sizeof(var[0]); v++) {
		g_fmt = var[v][0], g_op = var[v][1], g_mode = var[v][2], g_w = var[v][3];
		g_h = var[v][4], g_pitch = var[v][5], g_array = var[v][6], g_unk = var[v][8];
		u32 m = var[v][7] / 4;
		for (u32 k = 0; k < n; k++)
			dst[k] = 0xffff00ff;
		done[1] = 0;
		w = blit_emit(cmd, 0, gsrc, gdst, BYTES, BYTES, 1, gdone + 4);
		putx("variant ", v);
		puts1(": ");
		submit(cc.drawctxt_id, gcmd, w);
		u32 bad = 0, untouched = 0;
		for (u32 k = 0; k < m; k++) {
			bad += src[k] != dst[k];
			untouched += dst[k] == 0xffff00ff;
		}
		putx("  done ", done[1]);
		putx(", differing ", bad);
		putx(", untouched ", untouched);
		putx(" of ", m);
		puts1(!bad ? "  BLIT WORKS\n" : untouched < m ? "  GPU WROTE SOMETHING\n" : "  nothing\n");
		if (bad && untouched < m)
			for (u32 k = 0; k < 4; k++) {
				putx("    src ", src[k]);
				putx(" dst ", dst[k]);
				putx("   src[64] ", src[64 + k]);
				putx(" dst[64] ", dst[64 + k]);
				puts1("\n");
			}
	}
	sys(SYS_exit, 0, 0, 0, 0, 0, 0);
}
