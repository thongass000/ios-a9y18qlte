/*
 * a9y18qlte (Galaxy A9 2018, SDM660) bare-metal probe shim.
 *
 * Stage 1 of an ios-flame style XNU bring-up. The bootloader enters at EL1
 * with the MMU and caches off and x0 = DTB. The shim logs what an XNU port
 * needs to know (CPU features, PSCI, GIC, display scanout, PMIC), shows the
 * log on the boot splash framebuffer, and keeps a copy in the ramoops
 * console zone. It then chain-loads the Linux Image that build.sh appended
 * at PAYLOAD_OFF, with the DTB and initrd the bootloader prepared, so the
 * phone boots on as usual and Linux shows the shim's log as the previous
 * console, /sys/fs/pstore/console-ramoops-0.
 *
 * The shim never resets the phone itself: Samsung's bootloader only enters
 * recovery through the misc partition or the keys, so a reset from a shim in
 * the recovery slot loops. The shim goes in the boot slot; when anything
 * fails it stops with a message, and Power + Vol Up + Bixby reaches OrangeFox.
 * Only an XNU try (build.sh --xnu) arms the watchdog, and the boot after its
 * reset goes to Linux (TRY_MARK).
 *
 * No pointer tables anywhere: the image runs unrelocated wherever the
 * bootloader put it (build.sh rejects absolute relocations).
 */
#include <stdarg.h>
#include <stdint.h>
#include "font.h"

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

/* Addresses from the stock DTB (sdm660-sec-a9y18qlte-eur-r06). */
#define RAMOOPS_CONSOLE		0x84d40000UL	/* ramoops@84d00000: dmesg, console, ftrace, pmsg */
#define RAMOOPS_ZONE		0x40000UL
#define PRZ_SIG			0x43474244U	/* "DBGC" */
#define SPLASH_BASE		0x9d400000UL
#define SPLASH_SIZE		0x23ff000UL
#define MDSS_BASE		0x0c900000UL	/* mdss_mdp@c900000: pipe offsets count from here */
#define MDP_BASE		0x0c901000UL	/* + qcom,mdss-mdp-reg-offset: MDP top (hw rev) */
#define WDT_BASE		0x17817000UL
#define GICD_BASE		0x17a00000UL
#define SPMI_CORE		0x0800f000UL
#define SPMI_OBSRVR		0x09400000UL
#define SPMI_CNFG		0x0800a000UL

/* PM660 (SID 0) PON block at 0x800 */
#define PON_PS_HOLD_RST_CTL	0x85a
#define PON_PS_HOLD_RST_CTL2	0x85b
#define PON_SOFT_RB_SPARE	0x88f
#define PON_INT_RT_STS		0x810
#define PON_RESIN_ON		0x02	/* RESIN = Vol Down (qcom,pon_2, KEY_VOLUMEDOWN) */

#define PSCI_VERSION		0x84000000UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define PANEL_W			1080
#define PANEL_H			2220
#define HOLD_MS			8000	/* log stays on screen this long */

/* build.sh: Linux Image at this offset, its file size in our header's res2 */
#define PAYLOAD_OFF		0x200000UL
#define PAYLOAD_SIZE_AT		0x20
/* build.sh --xnu: the XNU payload follows the LOS ramdisk in the boot image's ramdisk */
#define XNU_OFF_AT		0x28	/* res3: payload offset from initrd-start, 0 = none */
#define LOS_RD_SIZE_AT		0x30	/* res4: the LOS ramdisk's own size */

/*
 * One XNU try per boot: the marker is set before XNU starts and lives in the
 * last 16 bytes of the ramoops console zone, which survive a warm reset. The
 * kernel pets the watchdog the shim arms (SweetPlatformExpert); when it hangs
 * or panics the bite resets the phone, the shim finds the marker, clears it,
 * copies XNU's console mirror into ramoops and boots Linux.
 */
#define TRY_MARK		(RAMOOPS_CONSOLE + RAMOOPS_ZONE - 16)
#define TRY_MAGIC		0x595254554e583941UL	/* "A9XNUTRY" */
#define XNU_MIRROR		0x1fd400000UL		/* bsd/dev/sweet_ufs.c SWEET_MIRROR_PA */
#define XNU_WDT_S		30

#define rsr(r) ({ u64 __v; __asm__ volatile("mrs %0, " r : "=r"(__v)); __v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((u64)(v)) : "memory")

static inline u32 rd32(uintptr_t a) { return *(volatile u32 *)a; }
/* start.S: a read that may abort; *fault = 1 if it did */
u32 probe_rd32(uintptr_t a, int *fault);
extern char probe_rd32_insn[];
extern int *volatile probe_fault;
static inline void wr32(uintptr_t a, u32 v) { *(volatile u32 *)a = v; }
static inline void wr8(uintptr_t a, u8 v) { *(volatile u8 *)a = v; }

static u64 ticks(void)
{
	__asm__ volatile("isb");
	return rsr("cntpct_el0");
}

static void delay_us(u64 us)
{
	u64 end = ticks() + us * (rsr("cntfrq_el0") / 1000000);
	while (ticks() < end)
		;
}

struct smc_ret { u64 a0, a1, a2, a3; };

static struct smc_ret smc(u64 fn, u64 a1, u64 a2, u64 a3)
{
	register u64 x0 __asm__("x0") = fn;
	register u64 x1 __asm__("x1") = a1;
	register u64 x2 __asm__("x2") = a2;
	register u64 x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "x4", "x5", "x6",
			 "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
			 "memory");
	return (struct smc_ret){ x0, x1, x2, x3 };
}

/* ---- framebuffer console ---- */

#define SCALE	2
#define CH_W	(8 * SCALE)
#define CH_H	(16 * SCALE)
#define MARGIN	16
#define TOP	96
#define COLS	((PANEL_W - 2 * MARGIN) / CH_W)
#define ROWS	((PANEL_H - TOP - MARGIN) / CH_H)

static uintptr_t fb;
static u32 fb_stride, fb_cpp;
static int con_row, con_col;
static u32 con_fg = 0xffffff;

static void put_px(int x, int y, u32 rgb)
{
	uintptr_t p = fb + (uintptr_t)y * fb_stride + (uintptr_t)x * fb_cpp;
	if (fb_cpp == 4) {
		wr32(p, rgb);
	} else {
		wr8(p, rgb & 0xff);
		wr8(p + 1, (rgb >> 8) & 0xff);
		wr8(p + 2, rgb >> 16);
	}
}

static void con_clear(void)
{
	if (!fb)
		return;
	for (int y = 0; y < PANEL_H; y++)
		for (int x = 0; x < PANEL_W; x++)
			put_px(x, y, 0);
	con_row = con_col = 0;
}

static void con_putc(char ch)
{
	if (!fb)
		return;
	if (ch == '\n' || con_col >= COLS) {
		con_col = 0;
		if (++con_row >= ROWS)
			con_row = 0;	/* wrap to the top: newest lines overwrite oldest */
		if (ch == '\n')
			return;
	}
	if (con_col == 0)	/* blank the row we start on */
		for (int y = TOP + con_row * CH_H; y < TOP + (con_row + 1) * CH_H; y++)
			for (int x = MARGIN; x < MARGIN + COLS * CH_W; x++)
				put_px(x, y, 0);
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + con_col * CH_W, y0 = TOP + con_row * CH_H;
	for (int gy = 0; gy < 16 * SCALE; gy++)
		for (int gx = 0; gx < 8 * SCALE; gx++)
			put_px(x0 + gx, y0 + gy,
			       (g[gy / SCALE] & (0x80 >> (gx / SCALE))) ? con_fg : 0);
	con_col++;
}

/* ---- log: RAM buffer mirrored into the ramoops console zone ---- */

static char log_buf[32768];
static u32 log_len, rz_len;

/* persistent_ram_buffer: sig, start, size, data[]; the zone's last 16 bytes are TRY_MARK */
static void ramoops_putc(char c)
{
	if (rz_len >= RAMOOPS_ZONE - 12 - 16)
		return;
	wr8(RAMOOPS_CONSOLE + 12 + rz_len++, c);
	wr32(RAMOOPS_CONSOLE + 4, rz_len);
	wr32(RAMOOPS_CONSOLE + 8, rz_len);
}

static void log_putc(char c)
{
	if (log_len >= sizeof(log_buf))
		return;
	log_buf[log_len++] = c;
	ramoops_putc(c);
	con_putc(c);
}

static void log_puts(const char *s)
{
	while (*s)
		log_putc(*s++);
}

static void log_num(u64 v, unsigned base, int width, char pad)
{
	char tmp[24];
	int n = 0;
	do {
		unsigned d = v % base;
		tmp[n++] = d < 10 ? '0' + d : 'a' + d - 10;
		v /= base;
	} while (v);
	while (n < width--)
		log_putc(pad);
	while (n)
		log_putc(tmp[--n]);
}

/* %s %c %d %u %x %p, optional 0/width and l */
static void log_printf(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	for (; *f; f++) {
		if (*f != '%') {
			log_putc(*f);
			continue;
		}
		f++;
		char pad = ' ';
		int width = 0, lng = 0;
		if (*f == '0')
			pad = '0', f++;
		while (*f >= '0' && *f <= '9')
			width = width * 10 + *f++ - '0';
		while (*f == 'l')
			lng = 1, f++;
		switch (*f) {
		case 's': log_puts(va_arg(ap, const char *)); break;
		case 'c': log_putc((char)va_arg(ap, int)); break;
		case 'd': {
			long v = lng ? va_arg(ap, long) : va_arg(ap, int);
			if (v < 0)
				log_putc('-'), v = -v;
			log_num((u64)v, 10, width, pad);
			break;
		}
		case 'u': log_num(lng ? va_arg(ap, u64) : va_arg(ap, u32), 10, width, pad); break;
		case 'x': log_num(lng ? va_arg(ap, u64) : va_arg(ap, u32), 16, width, pad); break;
		case 'p': log_puts("0x"); log_num((u64)va_arg(ap, void *), 16, 0, ' '); break;
		default: log_putc(*f); break;
		}
	}
	va_end(ap);
}

static void ramoops_init(void)
{
	wr32(RAMOOPS_CONSOLE + 4, 0);
	wr32(RAMOOPS_CONSOLE + 8, 0);
	wr32(RAMOOPS_CONSOLE, PRZ_SIG);
}

/* ---- display: find the pipe scanning out the splash buffer ---- */

/* Pipe registers: SRC_SIZE, SRC0_ADDR, YSTRIDE0, FORMAT, UNPACK, OP_MODE. */
#define NPIPES	5
static const u32 pipe_off[NPIPES] = { 0x5000, 0x7000, 0x25000, 0x27000, 0x29000 };
static const u32 pipe_reg[6] = { 0x00, 0x14, 0x24, 0x30, 0x34, 0x38 };
static u32 pipe_val[NPIPES][6];
static int pipe_fault[NPIPES];	/* clock-gated pipes abort on access */

static void fb_find(void)
{
	for (unsigned i = 0; i < NPIPES; i++) {
		for (unsigned r = 0; r < 6 && !pipe_fault[i]; r++)
			pipe_val[i][r] = probe_rd32(MDSS_BASE + pipe_off[i] + pipe_reg[r], &pipe_fault[i]);
		if (pipe_fault[i] || fb)
			continue;
		u32 size = pipe_val[i][0], addr = pipe_val[i][1], stride = pipe_val[i][2] & 0xffff;
		u32 w = size & 0xffff, h = size >> 16, cpp = ((pipe_val[i][3] >> 9) & 3) + 1;
		if (addr >= SPLASH_BASE && addr < SPLASH_BASE + SPLASH_SIZE && w >= PANEL_W &&
		    h >= PANEL_H && stride >= w * cpp && (cpp == 3 || cpp == 4))
			fb = addr, fb_stride = stride, fb_cpp = cpp;
	}
}

static void fb_log(void)
{
	for (unsigned i = 0; i < NPIPES; i++) {
		u32 *v = pipe_val[i];
		if (pipe_fault[i])
			log_printf("mdp pipe %x: abort (clock off)\n", pipe_off[i]);
		else
			log_printf("mdp pipe %x: %ux%u addr %x stride %x fmt %x unpack %x op %x\n",
				   pipe_off[i], v[0] & 0xffff, v[0] >> 16, v[1], v[2], v[3], v[4], v[5]);
	}
	int f = 0;
	u32 rev = probe_rd32(MDP_BASE, &f);
	log_printf("mdp hw rev %x%s, console fb %lx stride %u cpp %u\n", rev, f ? " (abort)" : "",
		   (u64)fb, fb_stride, fb_cpp);
}

/* ---- SPMI PMIC arbiter v2/v3 (drivers/spmi/spmi-pmic-arb.c), EE 0 ---- */

static int spmi_apid(u8 sid, u16 addr, u32 *apid, u32 *owner)
{
	u32 ppid = ((u32)sid << 8) | (addr >> 8);
	for (u32 n = 0; 0x800 + 4 * n < 0x1000; n++) {
		u32 v = rd32(SPMI_CORE + 0x800 + 4 * n);
		if (v && ((v >> 8) & 0xfff) == ppid) {
			*apid = n;
			*owner = rd32(SPMI_CNFG + 0x700 + 4 * n) & 7;
			return 0;
		}
	}
	return -1;
}

static int spmi_wait(uintptr_t st)
{
	for (int i = 0; i < 10000; i++) {
		u32 s = rd32(st);
		if (s & 1)
			return (s & 0xe) ? -(int)s : 0;
		delay_us(1);
	}
	return -1000;
}

static int spmi_read(u8 sid, u16 addr, u8 *val)
{
	u32 apid, owner;
	if (spmi_apid(sid, addr, &apid, &owner))
		return -2000;
	uintptr_t b = SPMI_OBSRVR + 0x8000 * apid;
	wr32(b + 0x00, (1U << 27) | ((addr & 0xff) << 4));	/* EXT_READL, 1 byte */
	int rc = spmi_wait(b + 0x08);
	if (!rc)
		*val = rd32(b + 0x18) & 0xff;
	return rc;
}

/* ---- probes ---- */

static void log_cpu(u64 dtb)
{
	u64 isar0 = rsr("id_aa64isar0_el1"), pfr0 = rsr("id_aa64pfr0_el1");
	u64 mmfr0 = rsr("id_aa64mmfr0_el1");
	log_printf("cpu: el %lu midr %lx mpidr %lx dtb %lx\n", rsr("currentel") >> 2,
		   rsr("midr_el1"), rsr("mpidr_el1"), dtb);
	log_printf("cpu: isar0 %lx isar1 %lx pfr0 %lx\n", isar0, rsr("id_aa64isar1_el1"), pfr0);
	log_printf("cpu: mmfr0 %lx mmfr1 %lx mmfr2 %lx\n", mmfr0, rsr("id_aa64mmfr1_el1"),
		   rsr("id_aa64mmfr2_el1"));
	/* what the iOS userland and XNU care about */
	log_printf("cpu: LSE atomics %s, RDM %s, FP16 %s, SHA3 %s\n",
		   ((isar0 >> 20) & 0xf) >= 2 ? "yes" : "NO", ((isar0 >> 28) & 0xf) ? "yes" : "NO",
		   ((pfr0 >> 16) & 0xf) == 1 ? "yes" : "NO", ((isar0 >> 32) & 0xf) ? "yes" : "NO");
	log_printf("cpu: 16K pages %s, PA bits code %lu\n",
		   ((mmfr0 >> 20) & 0xf) ? "yes" : "NO", mmfr0 & 0xf);
	log_printf("cpu: sctlr %lx cntfrq %lu\n", rsr("sctlr_el1"), rsr("cntfrq_el0"));
}

static void log_psci(void)
{
	log_printf("psci: version %lx smccc %lx\n", smc(PSCI_VERSION, 0, 0, 0).a0,
		   smc(SMCCC_VERSION, 0, 0, 0).a0);
	log_printf("psci: features cpu_on %lx reset %lx reset2 %lx\n",
		   smc(PSCI_FEATURES, PSCI_CPU_ON, 0, 0).a0,
		   smc(PSCI_FEATURES, PSCI_SYSTEM_RESET, 0, 0).a0,
		   smc(PSCI_FEATURES, PSCI_SYSTEM_RESET2, 0, 0).a0);
}

static void log_gic(void)
{
	log_printf("gic: ctlr %x typer %x iidr %x\n", rd32(GICD_BASE), rd32(GICD_BASE + 4),
		   rd32(GICD_BASE + 8));
}

static void wdt_off(void)
{
	log_printf("wdt: en %x sts %x bark %x bite %x -> off\n", rd32(WDT_BASE + 0x08),
		   rd32(WDT_BASE + 0x0c), rd32(WDT_BASE + 0x10), rd32(WDT_BASE + 0x14));
	wr32(WDT_BASE + 0x08, 0);
}

/*
 * ---- touch probe: Zinitix ZT7548 (0x20) on BLSP1 QUP4 I2C ----
 * i2c@c178000 (qcom,i2c-msm-v2), SDA/SCL GPIO 14/15 (function 2 blsp_i2c4,
 * 2 mA, no pull), IRQ GPIO 67 (low while a report waits), AVDD s2dos03 LDO3
 * (regulator-boot-on). QUP in FIFO mode with v1 tags, polled, as Linux's
 * i2c-qup.c drives it; 19.2 MHz in, 400 kHz out. Register addresses go out
 * low byte first (zinitix_zt75xx_ts.c).
 */
#define GCC_BASE		0x00100000UL
#define GCC_BLSP1_AHB_VOTE	(GCC_BASE + 0x52004)	/* bit 17 */
#define GCC_BLSP1_QUP4_I2C_CBCR	(GCC_BASE + 0x1f008)
#define TLMM_CFG(n)		(0x03900000UL + 0x1000UL * (n))	/* NORTH tile */
#define TLMM_IO(n)		(TLMM_CFG(n) + 4)
#define QUP4			0x0c178000UL
#define QUP_CONFIG		0x000
#define QUP_STATE		0x004
#define QUP_IO_MODE		0x008
#define QUP_SW_RESET		0x00c
#define QUP_OPERATIONAL		0x018
#define QUP_ERROR_FLAGS		0x01c
#define QUP_HW_VERSION		0x030
#define QUP_OUT_FIFO		0x110
#define QUP_MX_WRITE_CNT	0x150
#define QUP_MX_READ_CNT		0x208
#define QUP_IN_FIFO		0x218
#define QUP_I2C_CLK_CTL		0x400
#define QUP_I2C_STATUS		0x404
#define QUP_STATE_VALID		0x4
#define QUP_RESET		0
#define QUP_RUN			1
#define QUP_PAUSE		3
#define QUP_OUT_NOT_EMPTY	(1U << 4)
#define QUP_IN_NOT_EMPTY	(1U << 5)
#define QUP_MX_OUTPUT_DONE	(1U << 10)
#define I2C_STATUS_ERROR_MASK	0x38000fcU
#define I2C_STATUS_BUS_ACTIVE	(1U << 8)
#define TAG_START		(1U << 8)
#define TAG_DATA		(2U << 8)
#define TAG_STOP		(3U << 8)
#define TAG_REC			(4U << 8)
#define ZT_ADDR			0x20

static int qup_set_state(u32 st)
{
	for (int i = 0; i < 1000 && !(rd32(QUP4 + QUP_STATE) & QUP_STATE_VALID); i++)
		delay_us(1);
	wr32(QUP4 + QUP_STATE, st);
	for (int i = 0; i < 1000; i++) {
		u32 s = rd32(QUP4 + QUP_STATE);
		if ((s & QUP_STATE_VALID) && (s & 3) == st)
			return 0;
		delay_us(1);
	}
	return -1;
}

/* One I2C message, START..STOP: write n bytes of buf, or read n bytes into it. */
static int qup_msg(int rd, u8 *buf, u32 n)
{
	wr32(QUP4 + QUP_SW_RESET, 1);
	if (qup_set_state(QUP_RESET))
		return -1;
	wr32(QUP4 + QUP_CONFIG, (2U << 8) | 15);	/* I2C mini core, 16-bit words */
	wr32(QUP4 + QUP_IO_MODE, (1U << 14) | (1U << 15));	/* FIFO, pack/unpack */
	if (rd)
		wr32(QUP4 + QUP_MX_READ_CNT, n);
	else
		wr32(QUP4 + QUP_MX_WRITE_CNT, n + 1);
	if (qup_set_state(QUP_RUN))
		return -2;
	wr32(QUP4 + QUP_I2C_CLK_CTL, (3U << 8) | 21);	/* 19.2 MHz / 2 / (21 + 3) = 400 kHz */
	if (qup_set_state(QUP_PAUSE))
		return -3;
	if (rd) {
		wr32(QUP4 + QUP_OUT_FIFO, (TAG_START | (ZT_ADDR << 1) | 1) | ((TAG_REC | n) << 16));
	} else {
		u32 val = TAG_START | (ZT_ADDR << 1);
		for (u32 i = 0, idx = 1; i < n; i++, idx++) {
			u32 t = (i == n - 1 ? TAG_STOP : TAG_DATA) | buf[i];
			val = (idx & 1) ? val | (t << 16) : t;
			if ((idx & 1) || i == n - 1)
				wr32(QUP4 + QUP_OUT_FIFO, val);
		}
	}
	if (qup_set_state(QUP_RUN))
		return -4;
	u32 got = 0, word = 0;
	for (int t = 0; t < 20000; t++) {
		u32 st = rd32(QUP4 + QUP_I2C_STATUS), op = rd32(QUP4 + QUP_OPERATIONAL);
		if (st & I2C_STATUS_ERROR_MASK) {
			log_printf("qup: i2c status %x op %x err %x\n", st, op,
				   rd32(QUP4 + QUP_ERROR_FLAGS));
			qup_set_state(QUP_RESET);
			return -5;
		}
		if (rd) {
			while (got < n && (rd32(QUP4 + QUP_OPERATIONAL) & QUP_IN_NOT_EMPTY)) {
				word = rd32(QUP4 + QUP_IN_FIFO);
				buf[got++] = word & 0xff;
				if (got < n)
					buf[got++] = (word >> 16) & 0xff;
			}
			if (got == n)
				break;
		} else if (!(op & QUP_OUT_NOT_EMPTY) && !(st & I2C_STATUS_BUS_ACTIVE)) {
			break;
		}
		delay_us(5);
	}
	qup_set_state(QUP_RESET);
	return rd && got != n ? -6 : 0;
}

static int zt_write(u16 reg, const u8 *v, u32 n)
{
	u8 pkt[8] = { reg & 0xff, reg >> 8 };
	for (u32 i = 0; i < n && i < 6; i++)
		pkt[2 + i] = v[i];
	return qup_msg(0, pkt, n + 2);
}

static int zt_read(u16 reg, u8 *v, u32 n)
{
	u8 r[2] = { reg & 0xff, reg >> 8 };
	int rc = qup_msg(0, r, 2);
	delay_us(50);
	return rc ? rc : qup_msg(1, v, n);
}

/*
 * s2dos03 (0x60) on the bit-banged i2c@22: SDA GPIO 66, SCL GPIO 70 (i2c-gpio,
 * 2 us). Open drain by hand: a line is driven low with OE set, released with
 * OE clear. LDO3 (touch AVDD, 3.0 V under Linux) is REG_REG_EN 0x03 bit 2,
 * voltage in REG_LDO3_CFG 0x12 bits 6:0 (0.6 V + 25 mV steps).
 */
#define BB_SDA			66
#define BB_SCL			70
#define TLMM_OE			(1U << 9)
#define S2DOS03			0x60

static void bb_line(int pin, int high)
{
	wr32(TLMM_IO(pin), 0);				/* output value 0 when driven */
	wr32(TLMM_CFG(pin), high ? 0 : TLMM_OE);	/* gpio function, OE = pull low */
	delay_us(5);
}

static int bb_in(int pin)
{
	return rd32(TLMM_IO(pin)) & 1;
}

static void bb_start(void)
{
	bb_line(BB_SDA, 1);
	bb_line(BB_SCL, 1);
	bb_line(BB_SDA, 0);
	bb_line(BB_SCL, 0);
}

static void bb_stop(void)
{
	bb_line(BB_SDA, 0);
	bb_line(BB_SCL, 1);
	bb_line(BB_SDA, 1);
}

/* returns 1 if the byte was acked */
static int bb_write(u8 b)
{
	for (int i = 7; i >= 0; i--) {
		bb_line(BB_SDA, (b >> i) & 1);
		bb_line(BB_SCL, 1);
		bb_line(BB_SCL, 0);
	}
	bb_line(BB_SDA, 1);
	bb_line(BB_SCL, 1);
	int ack = !bb_in(BB_SDA);
	bb_line(BB_SCL, 0);
	return ack;
}

static u8 bb_read(int ack)
{
	u8 b = 0;
	bb_line(BB_SDA, 1);
	for (int i = 0; i < 8; i++) {
		bb_line(BB_SCL, 1);
		b = (u8)(b << 1 | bb_in(BB_SDA));
		bb_line(BB_SCL, 0);
	}
	bb_line(BB_SDA, !ack);
	bb_line(BB_SCL, 1);
	bb_line(BB_SCL, 0);
	bb_line(BB_SDA, 1);
	return b;
}

static int s2dos_read(u8 reg, u8 *v)
{
	bb_start();
	int ok = bb_write(S2DOS03 << 1) && bb_write(reg);
	bb_start();
	ok = ok && bb_write(S2DOS03 << 1 | 1);
	if (ok)
		*v = bb_read(0);
	bb_stop();
	return ok ? 0 : -1;
}

static int s2dos_write(u8 reg, u8 v)
{
	bb_start();
	int ok = bb_write(S2DOS03 << 1) && bb_write(reg) && bb_write(v);
	bb_stop();
	return ok ? 0 : -1;
}

static void touch_power(void)
{
	u8 id = 0, en = 0, cfg = 0, stat = 0;
	int rc = s2dos_read(0x00, &id) | s2dos_read(0x03, &en) | s2dos_read(0x12, &cfg) |
		 s2dos_read(0x02, &stat);
	log_printf("s2dos03: rc %d id %x reg_en %x ldo3_cfg %x stat %x\n", rc, id, en, cfg, stat);
	if (rc)
		return;
	if ((cfg & 0x7f) != 96)
		s2dos_write(0x12, (u8)((cfg & 0x80) | 96));	/* 3.0 V */
	if (!(en & 0x04))
		s2dos_write(0x03, en | 0x04);
	delay_us(50000);	/* CHIP_ON_DELAY */
	s2dos_read(0x03, &en);
	s2dos_read(0x12, &cfg);
	s2dos_read(0x02, &stat);
	log_printf("s2dos03: now reg_en %x ldo3_cfg %x stat %x (ldo3 pok bit 2)\n", en, cfg, stat);
}

static void touch_probe(void)
{
	touch_power();
	log_printf("touch: gcc vote %x qup4 cbcr %x, gpio14 %x gpio15 %x gpio67 cfg %x in %x\n",
		   rd32(GCC_BLSP1_AHB_VOTE), rd32(GCC_BLSP1_QUP4_I2C_CBCR), rd32(TLMM_CFG(14)),
		   rd32(TLMM_CFG(15)), rd32(TLMM_CFG(67)), rd32(TLMM_IO(67)) & 1);
	wr32(GCC_BLSP1_AHB_VOTE, rd32(GCC_BLSP1_AHB_VOTE) | (1U << 17));
	wr32(GCC_BLSP1_QUP4_I2C_CBCR, rd32(GCC_BLSP1_QUP4_I2C_CBCR) | 1);
	for (int i = 0; i < 1000 && (rd32(GCC_BLSP1_QUP4_I2C_CBCR) >> 31); i++)
		delay_us(1);
	wr32(TLMM_CFG(14), 2U << 2);	/* blsp_i2c4, 2 mA, no pull */
	wr32(TLMM_CFG(15), 2U << 2);
	wr32(TLMM_CFG(67), 0);		/* gpio input, 2 mA, no pull */
	int f = 0;
	u32 ver = probe_rd32(QUP4 + QUP_HW_VERSION, &f);
	log_printf("touch: qup4 cbcr %x, hw version %x%s\n", rd32(GCC_BLSP1_QUP4_I2C_CBCR), ver,
		   f ? " (abort)" : "");
	if (f)
		return;
	u8 one[2] = { 1, 0 }, code[2] = { 0 }, rev[2] = { 0 }, fw[2] = { 0 };
	int rc = zt_write(0xc000, one, 2);	/* vendor command enable */
	int rc2 = zt_read(0xcc00, code, 2);
	log_printf("touch: vendor enable rc %d, chip code rc %d = %x (ZT7548 e548)\n", rc, rc2,
		   code[0] | code[1] << 8);
	/* rest of bt532_power_sequence(): clear INTN, init NVM, start the firmware */
	u8 c004[2] = { 0x04, 0xc0 };
	qup_msg(0, c004, 2);
	delay_us(10);
	zt_write(0xc002, one, 2);
	delay_us(2000);
	zt_write(0xc001, one, 2);
	delay_us(150000);	/* FIRMWARE_ON_DELAY */
	int rc3 = zt_read(0x0011, rev, 2);	/* chip revision (firmware running) */
	int rc4 = zt_read(0x0012, fw, 2);	/* firmware version */
	log_printf("touch: revision rc %d = %x, firmware rc %d = %x, irq gpio67 %u\n", rc3,
		   rev[0] | rev[1] << 8, rc4, fw[0] | fw[1] << 8, rd32(TLMM_IO(67)) & 1);
	/*
	 * mini_init_touch(): reset, point mode 0, cover open (COVER_OPEN 0x0200:
	 * 0 reads as a closed flip cover, touch off), optional mode 0,
	 * count/down/move/up/palm/palm-reject interrupts, LPM and AOD off
	 */
	u8 zero[2] = { 0, 0 }, cover[2] = { 0x00, 0x02 }, mask[2] = { 0x3f, 0 }, rst[2] = { 0, 0 },
	   clr[2] = { 0x03, 0 };
	qup_msg(0, rst, 2);
	zt_write(0x0010, zero, 2);
	zt_write(0x023e, cover, 2);
	zt_write(0x0116, zero, 2);
	int rc5 = zt_write(0x00f0, mask, 2);
	for (u16 r = 0x00ab; r <= 0x00af; r++)
		zt_write(r, zero, 2);
	for (int i = 0; i < 10; i++)
		qup_msg(0, clr, 2);
	log_printf("touch: init rc %d, irq gpio67 %u: touch the screen now\n", rc5,
		   rd32(TLMM_IO(67)) & 1);
}

/*
 * ---- GPU probe: Adreno 512 (kgsl-3d0@5000000, chip id 0x05010200) ----
 * Power-up as the 4.4 kernel's clock and GDSC drivers do it, every clock
 * from XO (19.2 MHz; the GPU PLL is not needed to see the GPU alive):
 *   GCC gpu_cfg_ahb 0x71004, gpu_bimc_gfx 0x71010, bimc_gfx 0x7106c
 *   GPUCC cxo 0x1020, rbbmtimer RCG 0x10b0 + CBCR 0x10d0, gfx3d RCG 0x1070 + CBCR 0x1098
 *   GPU CX GDSC 0x5066004; GX: BCR 0x5066090 pulse, domain misc 0x5065130
 *   (GMEM reset pulse, clamp off), GDSC 0x5066094. GX runs in BHS mode off
 *   the CX rail (pm660l_s3, always on): no regulator to program.
 * Proof of life: RBBM_ALWAYSON_COUNTER (0x4d2) advances. Then all off again
 * for Linux's kgsl. Writes to an unclocked block can hang the bus (only
 * reads are fault tolerant here), so it runs only while Vol Down is held.
 */
#define GPU_GCC_AHB		(GCC_BASE + 0x71004)
#define GPU_GCC_BIMC		(GCC_BASE + 0x71010)
#define GPU_GCC_BIMC_GFX	(GCC_BASE + 0x7106c)
#define GPUCC			0x05065000UL
#define GPUCC_CXO_CBCR		(GPUCC + 0x1020)
#define GPUCC_GFX3D_RCGR	(GPUCC + 0x1070)
#define GPUCC_GFX3D_CBCR	(GPUCC + 0x1098)
#define GPUCC_RBBMTIMER_RCGR	(GPUCC + 0x10b0)
#define GPUCC_RBBMTIMER_CBCR	(GPUCC + 0x10d0)
#define GPU_GX_DOMAIN_MISC	0x05065130UL
#define GPU_CX_GDSC		0x05066004UL
#define GPU_GX_BCR		0x05066090UL
#define GPU_GX_GDSC		0x05066094UL
#define GPU_BASE		0x05000000UL
#define A5XX_ALWAYSON_LO	(GPU_BASE + 0x4d2 * 4)
#define A5XX_ALWAYSON_HI	(GPU_BASE + 0x4d3 * 4)
#define A5XX_RBBM_STATUS	(GPU_BASE + 0x4f5 * 4)

static int branch_on(uintptr_t cbcr)
{
	wr32(cbcr, rd32(cbcr) | 1);
	for (int i = 0; i < 1000; i++) {
		if (!(rd32(cbcr) >> 31))	/* CLK_OFF clear */
			return 0;
		delay_us(1);
	}
	return -1;
}

/* RCG to XO, divider 1 (CFG 0), latch with UPDATE; root forced on if asked */
static int rcg_xo(uintptr_t rcgr, int force_root)
{
	wr32(rcgr + 4, 0);
	wr32(rcgr, rd32(rcgr) | 1 | (force_root ? 2 : 0));
	for (int i = 0; i < 1000; i++) {
		if (!(rd32(rcgr) & 1))
			return 0;
		delay_us(1);
	}
	return -1;
}

static int gdsc_on(uintptr_t gdscr)
{
	wr32(gdscr, rd32(gdscr) & ~1U);
	for (int i = 0; i < 2000; i++) {
		if (rd32(gdscr) >> 31)		/* PWR_ON */
			return 0;
		delay_us(1);
	}
	return -1;
}

/*
 * ---- first GPU command: microcode, ring, CP_ME_INIT, CP_MEM_WRITE ----
 * As mainline Linux's a5xx_hw_init()/a5xx_me_init() for this test only: the
 * GPU comes up secure (RBBM_SECVID_TRUST_CNTL 1, and a non-secure write to
 * it stalls the bus), so ME_INIT runs secure, then TZ loads the a512_zap
 * shader and the ring leaves secure mode with CP_SET_SECURE_MODE. No
 * UCHE/GMEM setup, buffers at their physical addresses (the GPU SMMU must
 * be in bypass for this to work). Firmware from build.sh's blob at 1 MiB.
 */
#define GPUFW_OFF		0x100000UL
extern char _head[];
static int streq(uintptr_t a, const char *b);
#define GR(r)			(GPU_BASE + (uintptr_t)(r) * 4)
#define CP_ME_INIT		0x48
#define CP_MEM_WRITE		0x3d
#define CP_SET_SECURE_MODE	0x66
#define GPU_MAGIC		0xa9c0ffeeU

static u32 gpu_pm4[0x5000 / 4] __attribute__((aligned(4096)));
static u32 gpu_pfp[0x5000 / 4] __attribute__((aligned(4096)));
static u32 gpu_ring[8192] __attribute__((aligned(4096)));
static volatile u32 gpu_scratch[4] __attribute__((aligned(4096)));

/* firmware file NAME in the blob: its address, *size; 0 if missing */
static uintptr_t gpufw_find(const char *name, u32 *size)
{
	uintptr_t blob = (uintptr_t)_head + GPUFW_OFF;
	if (rd32(blob) != 0x47593941 || rd32(blob + 4) != 0x57465550)	/* "A9YG" "PUFW" */
		return 0;
	u32 n = rd32(blob + 8);
	for (u32 i = 0; i < n && i < 16; i++) {
		uintptr_t e = blob + 12 + 24 * i;
		if (streq(e, name)) {
			*size = rd32(e + 20);
			return blob + rd32(e + 16);
		}
	}
	return 0;
}

/* first byte after the blob's last file */
static uintptr_t gpufw_end(void)
{
	uintptr_t blob = (uintptr_t)_head + GPUFW_OFF, end = blob;
	u32 n = rd32(blob + 8);
	for (u32 i = 0; i < n && i < 16; i++) {
		uintptr_t e = blob + 12 + 24 * i;
		if (blob + rd32(e + 16) + rd32(e + 20) > end)
			end = blob + rd32(e + 16) + rd32(e + 20);
	}
	return end;
}

/* firmware file NAME from the blob, copied without its first dword into DST */
static int gpufw_copy(const char *name, u32 *dst, u32 max)
{
	u32 size = 0;
	uintptr_t f = gpufw_find(name, &size);
	if (!f)
		return -3;
	if (size < 8 || size - 4 > max)
		return -2;
	for (u32 w = 0; w < (size - 4) / 4; w++)
		dst[w] = rd32(f + 4 + 4 * w);
	return (int)(size - 4);
}

/*
 * TZ peripheral authentication (SCM service 2, ARMv8 SMC convention as
 * Linux's qcom_scm-smc.c): x0 = SMC64 SIP | svc << 8 | cmd, x1 = arginfo,
 * x2.. = args, x6 = 0. Busy (-12) is retried.
 */
#define PAS_INIT_IMAGE		1
#define PAS_MEM_SETUP		2
#define PAS_AUTH_AND_RESET	5
#define PAS_SHUTDOWN		6
#define GPU_PAS_ID		13

static long scm_pas(u32 cmd, u64 arginfo, u64 a, u64 b, u64 c)
{
	register u64 x0 __asm__("x0");
	for (int tries = 0; tries < 100; tries++) {
		register u64 x1 __asm__("x1") = arginfo;
		register u64 x2 __asm__("x2") = a;
		register u64 x3 __asm__("x3") = b;
		register u64 x4 __asm__("x4") = c;
		register u64 x5 __asm__("x5") = 0;
		register u64 x6 __asm__("x6") = 0;
		x0 = 0x42000000UL | 2 << 8 | cmd;
		__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5),
				 "+r"(x6) : : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
				 "x15", "x16", "x17", "memory");
		if ((long)x0 != -12)
			break;
		delay_us(100);
	}
	return (long)x0;
}

/*
 * a512_zap: metadata = the .mdt (ELF headers + hash segment), one
 * relocatable LOAD segment (.b02) put 1 MiB aligned in the free gap
 * between the blob and Linux at 2 MiB. 0 when TZ has taken it.
 */
static int gpu_zap_load(void)
{
	u32 mdt_size = 0, seg_size = 0;
	uintptr_t mdt = gpufw_find("a512_zap.mdt", &mdt_size);
	uintptr_t seg = gpufw_find("a512_zap.b02", &seg_size);
	uintptr_t base = (gpufw_end() + 0xfffff) & ~0xfffffUL;
	u32 region = (seg_size + 0xfff) & ~0xfffU;
	log_printf("gpu: zap mdt %u bytes, segment %u bytes, region %lx+%x\n", mdt_size, seg_size,
		   base, region);
	if (!mdt || !seg || base + region > (uintptr_t)_head + 0x200000)
		return -1;
	long rc = scm_pas(PAS_INIT_IMAGE, 2 | 2 << 6, GPU_PAS_ID, mdt, 0);
	log_printf("gpu: pas init image rc %ld\n", rc);
	if (rc)
		return -2;
	rc = scm_pas(PAS_MEM_SETUP, 3, GPU_PAS_ID, base, region);
	log_printf("gpu: pas mem setup rc %ld\n", rc);
	if (rc)
		goto fail;
	for (u32 w = 0; w < region / 4; w++)
		wr32(base + 4 * w, 4 * w < seg_size ? rd32(seg + 4 * w) : 0);
	__asm__ volatile("dsb sy" ::: "memory");
	rc = scm_pas(PAS_AUTH_AND_RESET, 1, GPU_PAS_ID, 0, 0);
	log_printf("gpu: pas auth and reset rc %ld\n", rc);
	if (!rc)
		return 0;
fail:
	log_printf("gpu: pas shutdown rc %ld\n", scm_pas(PAS_SHUTDOWN, 1, GPU_PAS_ID, 0, 0));
	return -3;
}

static u32 pm4_parity(u32 v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^
				  (v >> 24) ^ (v >> 28)))) & 1;
}

static u32 pkt7(u32 op, u32 cnt)
{
	return 0x70000000U | cnt | pm4_parity(cnt) << 15 | op << 16 | pm4_parity(op) << 23;
}

/* type 4: CNT register writes from REG on */
static u32 pkt4(u32 reg, u32 cnt)
{
	return 0x40000000U | cnt | pm4_parity(cnt) << 7 | reg << 8 | pm4_parity(reg) << 27;
}

/*
 * ---- first GPU drawing: the 2D engine on the screen ----
 * As freedreno's fd5_blitter.c (emit_setup + emit_blit_buffer), tried on
 * the a512 through Android's kgsl first (~/a9-ios/kmod/kgsltest): COPY and
 * FILL write nothing at all there either, BLIT_OP_SCALE in BLIT2DSCALE mode
 * writes every destination pixel but as 0 (the source, read through the
 * texture pipe, comes back as zeros). So for now the GPU draws black: the
 * bottom GPU_BLIT_ROWS rows of the console framebuffer, as an RGBA8 surface
 * of two-row lines (the panel's 4320-byte stride is not a multiple of 64,
 * 8640 = 64 * 135 is). Ends with CCU flush and CACHE_FLUSH_TS writing
 * GPU_BLIT_DONE.
 */
#define CP_WAIT_FOR_IDLE	0x26
#define CP_EVENT_WRITE		0x46
#define CP_SET_RENDER_MODE	0x6c
#define CP_BLIT			0x2c
#define GPU_BLIT_ROWS		600
#define GPU_BLIT_DONE		0xb117d0e5U

/* one RGBA8 rectangle WIDTH x HEIGHT pixels, PITCH bytes per line, src to dst */
static u32 gpu_blit_emit(u32 *r, u32 w, u64 src, u64 dst, u32 width, u32 height, u32 pitch,
			 u64 done)
{
	/* fd5_emit_restore(): BYPASS render mode (full packet), UCHE invalidate, WFI */
	r[w++] = pkt7(CP_SET_RENDER_MODE, 5);
	r[w++] = 1;			/* BYPASS */
	for (int k = 0; k < 4; k++)
		r[w++] = 0;
	r[w++] = pkt4(0xe91, 5);	/* UCHE_CACHE_INVALIDATE_MIN/MAX, INVALIDATE */
	for (int k = 0; k < 4; k++)
		r[w++] = 0;
	r[w++] = 0x12;
	r[w++] = pkt7(CP_WAIT_FOR_IDLE, 0);
	/* fd5_emit_sysmem_prep(): CCU invalidate, PC/VFD power (2 SP cores), CCU in BYPASS */
	r[w++] = pkt7(0x1d, 1);		/* CP_SKIP_IB2_ENABLE_GLOBAL */
	r[w++] = 0;
	r[w++] = pkt7(CP_EVENT_WRITE, 1);
	r[w++] = 0x19;			/* PC_CCU_INVALIDATE_COLOR */
	r[w++] = pkt4(0xe3b0, 1);	/* PC_POWER_CNTL */
	r[w++] = 1;
	r[w++] = pkt4(0xe4f0, 1);	/* VFD_POWER_CNTL */
	r[w++] = 1;
	r[w++] = pkt7(CP_WAIT_FOR_IDLE, 0);
	r[w++] = pkt4(0x0cc7, 1);	/* RB_CCU_CNTL: BYPASS */
	r[w++] = 0x10000000;
	/* emit_setup(): 2D mode for RB/GRAS, bypass rendering */
	static const u16 setup[][2] = {
		{ 0xe141, 0x8 },		/* RB_RENDER_CNTL */
		{ 0x0cc6, 0x4 },		/* RB_MODE_CNTL */
		{ 0x0ec2, 0xc },		/* SP_MODE_CNTL */
		{ 0x0f02, 0x344 },		/* TPL1_MODE_CNTL */
		{ 0x0e06, 0x2 },		/* HLSQ_MODE_CNTL */
		{ 0xe000, 0x181 },		/* GRAS_CL_CNTL */
		{ 0x2184, 0x9 },		/* UNKNOWN_2184 */
	};
	for (unsigned k = 0; k < sizeof(setup) / sizeof(setup[0]); k++) {
		r[w++] = pkt4(setup[k][0], 1);
		r[w++] = setup[k][1];
	}
	r[w++] = pkt4(0x2100, 1);	/* RB_2D_BLIT_CNTL */
	r[w++] = 0x86000000;
	r[w++] = pkt4(0x2180, 1);	/* GRAS_2D_BLIT_CNTL */
	r[w++] = 0x86000000;
	r[w++] = pkt4(0xe140, 1);	/* RB_CNTL: BYPASS */
	r[w++] = 1U << 17;
	/* RB5_R8G8B8A8_UNORM, TILE5_LINEAR, WZYX; pitch and array pitch in 64-byte units */
	u32 info = 0x30, size = pitch >> 6 | ((pitch * height) >> 6) << 16;
	r[w++] = pkt7(CP_SET_RENDER_MODE, 1);
	r[w++] = 7;			/* BLIT2DSCALE */
	r[w++] = pkt4(0x2107, 9);	/* RB_2D_SRC_INFO, SRC, SRC_SIZE, 5 unknown */
	r[w++] = info;
	r[w++] = (u32)src;
	r[w++] = (u32)(src >> 32);
	r[w++] = size;
	for (int k = 0; k < 5; k++)
		r[w++] = 0;
	r[w++] = pkt4(0x2181, 1);	/* GRAS_2D_SRC_INFO */
	r[w++] = info;
	r[w++] = pkt4(0x2110, 9);	/* RB_2D_DST_INFO, DST, DST_SIZE, 5 unknown */
	r[w++] = info;
	r[w++] = (u32)dst;
	r[w++] = (u32)(dst >> 32);
	r[w++] = size;
	for (int k = 0; k < 5; k++)
		r[w++] = 0;
	r[w++] = pkt4(0x2182, 1);	/* GRAS_2D_DST_INFO */
	r[w++] = info;
	r[w++] = pkt7(CP_BLIT, 5);
	r[w++] = 3;			/* BLIT_OP_SCALE: the one op that writes */
	r[w++] = 0;			/* SRC x1, y1 */
	r[w++] = (width - 1) | (height - 1) << 16;
	r[w++] = 0;			/* DST x1, y1 */
	r[w++] = (width - 1) | (height - 1) << 16;
	r[w++] = pkt7(CP_SET_RENDER_MODE, 1);
	r[w++] = 8;			/* END2D */
	r[w++] = pkt7(CP_WAIT_FOR_IDLE, 0);
	r[w++] = pkt7(CP_EVENT_WRITE, 4);
	r[w++] = 0x1d;			/* PC_CCU_FLUSH_COLOR_TS */
	r[w++] = (u32)(done + 8);
	r[w++] = (u32)((done + 8) >> 32);
	r[w++] = 0;
	r[w++] = pkt7(CP_EVENT_WRITE, 4);
	r[w++] = 0x04;			/* CACHE_FLUSH_TS */
	r[w++] = (u32)done;
	r[w++] = (u32)(done >> 32);
	r[w++] = GPU_BLIT_DONE;
	return w;
}

/*
 * GPU SMMU (arm,smmu-kgsl@5040000, ARM SMMUv2, qcom,skip-init: its global
 * state is set up before Linux). The CP reaches memory through it with
 * stream 0 (gfx3d_user; 2 is gfx3d_secure). Unmatched streams fault, and
 * the hypervisor turns a bypass S2CR into fault, so the streams get a
 * stage-1 context bank like kgsl's, with an identity map of RAM.
 * Returns 0 when physical addresses work for the CP.
 */
#define GPU_SMMU		0x05040000UL
#define SMMU_GR1		(GPU_SMMU + 0x1000)
#define SMMU_CB(n)		(GPU_SMMU + 0x8000 + (n) * 0x1000)	/* 8 4K global pages */

/*
 * As Linux leaves it: SMR n matches stream gpu_sid[n] exactly (the
 * hypervisor refuses a mask that spans both) and routes it to context
 * bank n. Stream 2 carries the CP's fetches while it is still secure.
 */
#define GPU_NSTREAM		2
static const u32 gpu_sid[GPU_NSTREAM] = { 0, 2 };
static int gpu_smr_taken;
static u32 gpu_s2cr_old[GPU_NSTREAM], gpu_cbar_old[GPU_NSTREAM], gpu_cba2r_old[GPU_NSTREAM];
/* level 1, 4K granule, 32-bit VA: four 1 GiB blocks, RAM from 2 GiB identity mapped */
static u64 gpu_pt[512] __attribute__((aligned(4096)));

/* the SMRs and context banks taken by gpu_smmu_bypass() back, for Linux's arm-smmu */
static void gpu_smmu_restore(void)
{
	if (!gpu_smr_taken)
		return;
	log_printf("smmu: gfsr %x gfsynr0 %x gfsynr1 %x\n", rd32(GPU_SMMU + 0x48),
		   rd32(GPU_SMMU + 0x50), rd32(GPU_SMMU + 0x54));
	for (u32 n = 0; n < GPU_NSTREAM; n++) {
		log_printf("smmu: cb %u fsr %x far %x synr %x\n", n, rd32(SMMU_CB(n) + 0x58),
			   rd32(SMMU_CB(n) + 0x60), rd32(SMMU_CB(n) + 0x68));
		wr32(GPU_SMMU + 0x800 + 4 * n, 0);
		wr32(GPU_SMMU + 0xc00 + 4 * n, gpu_s2cr_old[n]);
		wr32(SMMU_CB(n), 0);
		wr32(SMMU_CB(n) + 0x58, 0xffffffff);
		wr32(SMMU_GR1 + 4 * n, gpu_cbar_old[n]);
		wr32(SMMU_GR1 + 0x800 + 4 * n, gpu_cba2r_old[n]);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	log_printf("smmu: smrs freed, gfsr %x\n", rd32(GPU_SMMU + 0x48));
	gpu_smr_taken = 0;
}

/*
 * As Linux 4.19's arm_smmu_device_reset() + qsmmuv2_device_reset() with
 * qcom,skip-init: TLBs invalidated and synced, sCR0 fault reporting,
 * private TLB maintenance, NSH bypass; then the DT's attach-impl-defs
 * written with the micro MMU halted.
 */
static const u32 gpu_smmu_impl[][2] = {
	{ 0x6000, 0x2378 }, { 0x6060, 0x1055 }, { 0x678c, 0x08 }, { 0x6794, 0x28 },
	{ 0x6800, 0x06 }, { 0x6900, 0x3ff }, { 0x6924, 0x204 }, { 0x6928, 0x11000 },
	{ 0x6930, 0x800 }, { 0x6960, 0xffffffff }, { 0x6b64, 0x1a5551 }, { 0x6b68, 0x9a82a382 },
};

static void gpu_smmu_reset(void)
{
	int i, f = 0;
	/* boot values of the impl-defs, against Linux's (6800 = 2, 6b68 = a80008a) */
	log_printf("smmu: impl at boot:");
	for (unsigned k = 0; k < sizeof(gpu_smmu_impl) / sizeof(gpu_smmu_impl[0]); k++)
		log_printf(" %x", probe_rd32(GPU_SMMU + gpu_smmu_impl[k][0], &f));
	log_printf("%s\n", f ? " (abort)" : "");
	/* micro MMU halted, impl-defs written, resumed: before any TLB maintenance */
	log_printf("smmu: halt ... ");
	wr32(GPU_SMMU + 0x6000, rd32(GPU_SMMU + 0x6000) | 1U << 2);	/* LOCAL_HALT_REQ */
	for (i = 0; i < 1000 && !(rd32(GPU_SMMU + 0x6000) & 1U << 3); i++)	/* IDLE */
		delay_us(1);
	log_printf("%s; impl defs ... ", i < 1000 ? "idle" : "NOT IDLE");
	for (unsigned k = 0; k < sizeof(gpu_smmu_impl) / sizeof(gpu_smmu_impl[0]); k++)
		wr32(GPU_SMMU + gpu_smmu_impl[k][0], gpu_smmu_impl[k][1]);
	wr32(GPU_SMMU + 0x6000, rd32(GPU_SMMU + 0x6000) & ~(1U << 2));
	log_printf("resumed, micro mmu ctrl %x 6800 %x\n", rd32(GPU_SMMU + 0x6000),
		   rd32(GPU_SMMU + 0x6800));
	log_printf("smmu: micro mmu ctrl %x tlbgstatus %x; gfsr ", rd32(GPU_SMMU + 0x6000),
		   rd32(GPU_SMMU + 0x74));
	wr32(GPU_SMMU + 0x48, rd32(GPU_SMMU + 0x48));	/* sGFSR clear */
	log_printf("ok, tlbiallnsnh ");
	wr32(GPU_SMMU + 0x68, 0xffffffff);		/* TLBIALLNSNH */
	log_printf("ok, tlbiallh ");
	wr32(GPU_SMMU + 0x6c, 0xffffffff);		/* TLBIALLH */
	/*
	 * sTLBGSYNC never completes for the shim (with or without the MMU, GX
	 * on or off): the write itself stalls. The TLBs are empty this early,
	 * so it is left out.
	 */
	log_printf("ok, tlbgsync skipped ");
	i = 0;
	u32 cr0 = rd32(GPU_SMMU);
	cr0 |= 1U << 1 | 1U << 2 | 1U << 4 | 1U << 5;	/* GFRE GFIE GCFGFRE GCFGFIE */
	cr0 |= 1U << 11 | 1U << 12;			/* VMIDPNE PTM */
	cr0 &= ~(1U | 1U << 13 | 3U << 14);		/* CLIENTPD FB BSU */
	cr0 |= 3U << 22;				/* SHCFG NSH */
	wr32(GPU_SMMU, cr0);
	log_printf("%s, cr0 %x\n", i < 1000 ? "ok" : "TIMEOUT", rd32(GPU_SMMU));
}

/* context bank n: AArch64 stage 1 over gpu_pt, stage 2 bypass */
static void gpu_smmu_cb(u32 n)
{
	int f = 0;
	uintptr_t cb = SMMU_CB(n);
	for (u64 i = 2; i < 4; i++)	/* block, AttrIndx 0, AP RW any EL, AF */
		gpu_pt[i] = i << 30 | 1 | 1U << 6 | 1U << 10;
	__asm__ volatile("dsb sy" ::: "memory");
	gpu_cbar_old[n] = rd32(SMMU_GR1 + 4 * n);
	gpu_cba2r_old[n] = rd32(SMMU_GR1 + 0x800 + 4 * n);
	wr32(cb, 0);
	wr32(SMMU_GR1 + 0x800 + 4 * n, 1);		/* CBA2R: VA64 */
	/* CBAR: S1 + S2 bypass, NSH, WB, VMID cb + 1 as Linux's arm-smmu */
	wr32(SMMU_GR1 + 4 * n, 1U << 16 | 3U << 8 | 0xfU << 12 | (n + 1));
	u64 pt = (uintptr_t)gpu_pt;
	wr32(cb + 0x20, (u32)pt);			/* TTBR0, ASID 0 */
	wr32(cb + 0x24, (u32)(pt >> 32));
	wr32(cb + 0x10, 2 | 7U << 15);			/* TCR2: 40-bit PA, SEP upstream (else bit 31 picks TTBR1) */
	wr32(cb + 0x30, 32 | 1U << 23);			/* TCR: T0SZ 32, 4K, non-cacheable walks, no TTBR1 */
	wr32(cb + 0x38, 0x44);				/* MAIR0: attr 0 normal non-cacheable */
	wr32(cb + 4, 0x70000000);			/* ACTLR: Qualcomm ISH/OSH/NSH as Linux */
	wr32(cb + 0x58, 0xffffffff);			/* FSR clear */
	wr32(cb + 0x618, 0);				/* TLBIALL; no TLBSYNC, see gpu_smmu_reset() */
	wr32(cb, 1 | 1U << 1 | 1U << 2 | 1U << 5);	/* SCTLR: M, TRE, AFE, CFRE */
	__asm__ volatile("dsb sy; isb" ::: "memory");
	log_printf("smmu: cb %u sctlr %x cbar %x cba2r %x ttbr0 %x tcr %x tcr2 %x%s\n", n,
		   probe_rd32(cb, &f), probe_rd32(SMMU_GR1 + 4 * n, &f),
		   probe_rd32(SMMU_GR1 + 0x800 + 4 * n, &f), probe_rd32(cb + 0x20, &f),
		   probe_rd32(cb + 0x30, &f), probe_rd32(cb + 0x10, &f), f ? " (abort)" : "");
}

static int gpu_smmu_bypass(void)
{
	int f = 0;
	u32 cr0 = probe_rd32(GPU_SMMU, &f), idr0 = probe_rd32(GPU_SMMU + 0x20, &f);
	u32 idr1 = probe_rd32(GPU_SMMU + 0x24, &f), gfsr = probe_rd32(GPU_SMMU + 0x48, &f);
	log_printf("smmu: cr0 %x idr0 %x idr1 %x gfsr %x%s\n", cr0, idr0, idr1, gfsr,
		   f ? " (abort)" : "");
	if (f)
		return -1;
	if (cr0 & 1) {
		log_printf("smmu: CLIENTPD: everything bypasses\n");
		return 0;
	}
	u32 nsmr = idr0 & 0xff, s2cr = 0, idx = ~0U;
	for (u32 n = 0; n < nsmr && n < 128; n++) {
		u32 smr = probe_rd32(GPU_SMMU + 0x800 + 4 * n, &f);
		if (!(smr >> 31))
			continue;
		u32 id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
		u32 s = probe_rd32(GPU_SMMU + 0xc00 + 4 * n, &f);
		log_printf("smmu: smr %u id %x mask %x s2cr %x\n", n, id, mask, s);
		if (idx == ~0U && !(id & ~mask)) {	/* matches stream 0 */
			idx = n;
			s2cr = s;
		}
	}
	if (idx == ~0U) {
		log_printf("smmu: no SMR for stream 0, unmatched -> %s\n",
			   (cr0 >> 10) & 1 ? "fault" : "bypass");
		if (!((cr0 >> 10) & 1))
			return 0;
		/*
		 * SMR/S2CR/context bank n for stream gpu_sid[n] (the SMRs are all
		 * free at boot), SMR before S2CR as Linux's arm-smmu;
		 * gpu_smmu_restore() frees them
		 */
		if (nsmr < GPU_NSTREAM)
			return -2;
		gpu_smmu_reset();
		int rc = 0;
		for (u32 n = 0; n < GPU_NSTREAM; n++) {
			gpu_smr_taken = 1;
			gpu_s2cr_old[n] = probe_rd32(GPU_SMMU + 0xc00 + 4 * n, &f);
			gpu_smmu_cb(n);
			wr32(GPU_SMMU + 0x800 + 4 * n, 1U << 31 | gpu_sid[n]);
			__asm__ volatile("dsb sy; isb" ::: "memory");
			wr32(GPU_SMMU + 0xc00 + 4 * n, 3U << 8 | n);	/* translate, SHCFG NSH */
			__asm__ volatile("dsb sy; isb" ::: "memory");
			u32 smr = probe_rd32(GPU_SMMU + 0x800 + 4 * n, &f);
			u32 s = probe_rd32(GPU_SMMU + 0xc00 + 4 * n, &f);
			log_printf("smmu: stream %u: smr %u = %x, s2cr %x (was %x)%s\n", gpu_sid[n], n, smr,
				   s, gpu_s2cr_old[n], f ? " (abort)" : "");
			if (!(smr >> 31) || (s & 0x300ff) != n)
				rc = n ? 2 : -3;	/* only stream 2 missing: go on without it */
		}
		return rc < 0 ? rc : 0;
	}
	if (((s2cr >> 16) & 3) == 1)
		return 0;
	if (((s2cr >> 16) & 3) == 0) {
		u32 pgsz = (idr1 >> 31) ? 0x10000 : 0x1000, npage = 1U << (((idr1 >> 28) & 7) + 1);
		uintptr_t cb = GPU_SMMU + (uintptr_t)npage * pgsz + (s2cr & 0xff) * pgsz;
		log_printf("smmu: stream 0 -> cb %u: sctlr %x ttbr0 %x fsr %x\n", s2cr & 0xff,
			   probe_rd32(cb, &f), probe_rd32(cb + 0x20, &f), probe_rd32(cb + 0x58, &f));
	}
	wr32(GPU_SMMU + 0xc00 + 4 * idx, (s2cr & ~(3U << 16)) | 1U << 16);
	__asm__ volatile("dsb sy; isb" ::: "memory");
	u32 now = probe_rd32(GPU_SMMU + 0xc00 + 4 * idx, &f);
	log_printf("smmu: s2cr %u set to bypass: reads %x\n", idx, now);
	return ((now >> 16) & 3) == 1 ? 0 : -3;
}

static int gpu_smmu_rc = -9;	/* gpu_smmu_bypass(), run by gpu_probe() before GX is up */

static void gpu_cp_test(void)
{
	int npm4 = gpufw_copy("a530_pm4.fw", gpu_pm4, sizeof(gpu_pm4));
	int npfp = gpufw_copy("a530_pfp.fw", gpu_pfp, sizeof(gpu_pfp));
	log_printf("gpu: microcode pm4 %d pfp %d bytes\n", npm4, npfp);
	if (npm4 <= 0 || npfp <= 0)
		return;
	u64 pm4 = (uintptr_t)gpu_pm4, pfp = (uintptr_t)gpu_pfp, ring = (uintptr_t)gpu_ring;
	u64 scratch = (uintptr_t)gpu_scratch;
	gpu_scratch[0] = 0;

	/* SError unmasked: an aborted register write lands in exc_handler, not silence */
	__asm__ volatile("msr daifclr, #4; isb");
	static const u16 regs_cp[] = { 0x860, 0x838, 0x839, 0x835, 0x836, 0x800, 0x801, 0x802, 0x807 };
	u32 vals[] = { 1, (u32)pm4, (u32)(pm4 >> 32), (u32)pfp, (u32)(pfp >> 32), (u32)ring,
		       (u32)(ring >> 32), 12 | 2U << 8 | 1U << 27, 0 };
	/* CP_ADDR_MODE_CNTL 64 bit, PM4/PFP_INSTR_BASE, RB_BASE, RB_CNTL (32 KiB, no rptr update), WPTR */
	for (unsigned k = 0; k < sizeof(regs_cp) / sizeof(regs_cp[0]); k++)
		wr32(GR(regs_cp[k]), vals[k]);
	/* secure-video TSB: 64 bit, no trusted range (SECVID_TRUST_CNTL itself is TZ's) */
	wr32(GR(0xf810), 1);
	wr32(GR(0xf800), 0);
	wr32(GR(0xf801), 0);
	wr32(GR(0xf802), 0);
	/* kgsl a5xx_start() for the a512: VBIF QoS, AHB, UCHE, CP fetch queue sizes */
	static const u16 a512_start[][2] = {
		{ 0x3049, 0x3 },			/* VBIF_ROUND_ROBIN_QOS_ARB */
		{ 0x93, 0x1 },				/* RBBM_AHB_CNTL0 */
		{ 0xe96, 0x2 },				/* UCHE_CACHE_WAYS */
		{ 0x825, 0x40 },			/* CP_MEQ_THRESHOLDS */
		{ 0x826, 0x400 },			/* CP_MERCIU_SIZE */
		{ 0xe81, 0 },				/* UCHE_MODE_CNTL: BIT(29) below */
	};
	for (unsigned k = 0; k < sizeof(a512_start) / sizeof(a512_start[0]); k++)
		wr32(GR(a512_start[k][0]), a512_start[k][1]);
	wr32(GR(0x820), 0x80000060);		/* CP_ROQ_THRESHOLDS_2 */
	wr32(GR(0x81f), 0x40201b16);		/* CP_ROQ_THRESHOLDS_1 */
	wr32(GR(0xe89), 0xffff0000);		/* UCHE_TRAP_BASE */
	wr32(GR(0xe8a), 0x0001ffff);
	wr32(GR(0xe87), 0xffff0000);		/* UCHE_WRITE_THRU_BASE */
	wr32(GR(0xe88), 0x0001ffff);
	wr32(GR(0xe81), 1U << 29);		/* UCHE_MODE_CNTL */
	wr32(GR(0x833), 0x02000000);		/* CP_CHICKEN_DBG */
	wr32(GR(0x94), 0xa6ffffff);		/* RBBM_AHB_CNTL1 */
	wr32(GR(0x95), 0x3f);			/* RBBM_AHB_CNTL2 */
	wr32(GR(0x302a), 0x9);			/* VBIF_GATE_OFF_WRREQ_EN (a512 uses a540's VBIF list) */
	/* UCHE GMEM range: base ALIGN(gmem 272 KiB, 1 MiB) as kgsl */
	wr32(GR(0xe8b), 0x100000);
	wr32(GR(0xe8c), 0);
	wr32(GR(0xe8d), 0x100000 + 0x44000 - 1);
	wr32(GR(0xe8e), 0);
	log_printf("gpu: rbbm clock cntl %x -> 0 (hw clock gating off)\n", rd32(GR(0x67)));
	wr32(GR(0x67), 0);			/* RBBM_CLOCK_CNTL, as a5xx_hwcg_set(false) */
	log_printf("gpu: cp registers set (rb base %x, roq %x meq %x), trust cntl %x\n",
		   rd32(GR(0x800)), rd32(GR(0x81f)), rd32(GR(0x825)), rd32(GR(0xf400)));

	u32 *r = gpu_ring, w = 0;
	r[w++] = pkt7(CP_ME_INIT, 8);
	r[w++] = 0x0000002f;
	r[w++] = 0x00000003;		/* multiple hardware contexts */
	r[w++] = 0x20000000;		/* error detection */
	r[w++] = 0;
	r[w++] = 0;
	r[w++] = 0;
	r[w++] = 0;
	r[w++] = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	if (gpu_smmu_rc) {
		log_printf("gpu: no SMMU mapping for stream 0 (%d), CP not started\n", gpu_smmu_rc);
		__asm__ volatile("msr daifset, #4; isb");
		return;
	}
	log_printf("gpu: starting the CP (secure) ... ");
	wr32(GR(0x832), 0);		/* CP_ME_CNTL: run ME and PFP */
	wr32(GR(0x807), w);		/* CP_RB_WPTR */
	int i;
	for (i = 0; i < 200 && rd32(GR(0x806)) != w; i++)
		delay_us(500);
	log_printf("me init rptr %u/%u after %d ms, rbbm status %x\n", rd32(GR(0x806)), w, i / 2,
		   rd32(GR(0x4f5)));

	int zap = gpu_zap_load();
	log_printf("gpu: zap %s, trust cntl %x\n", zap ? "FAILED" : "loaded", rd32(GR(0xf400)));
	if (!zap) {
		r[w++] = pkt7(CP_SET_SECURE_MODE, 1);
		r[w++] = 0;
	}
	r[w++] = pkt7(CP_MEM_WRITE, 3);
	r[w++] = (u32)scratch;
	r[w++] = (u32)(scratch >> 32);
	r[w++] = GPU_MAGIC;
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(GR(0x807), w);
	for (i = 0; i < 200 && gpu_scratch[0] != GPU_MAGIC; i++)
		delay_us(500);
	log_printf("gpu: cp rptr %u/%u, scratch %x after %d ms: %s\n", rd32(GR(0x806)), w,
		   gpu_scratch[0], i / 2, gpu_scratch[0] == GPU_MAGIC ? "THE GPU RAN OUR COMMANDS" :
		   "no write");
	log_printf("gpu: rbbm status %x status3 %x int0 %x cp int %x hw fault %x\n",
		   rd32(GR(0x4f5)), rd32(GR(0x530)), rd32(GR(0x4e1)), rd32(GR(0xb18)), rd32(GR(0xb1a)));
	/* PFP and ME state: stat word 0 of each (their program counters) */
	wr32(GR(0x808), 0);
	u32 pfp0 = rd32(GR(0x809));
	wr32(GR(0x81a), 0);
	log_printf("gpu: pfp stat0 %x me stat0 %x me cntl %x\n", pfp0, rd32(GR(0xb14)),
		   rd32(GR(0x832)));

	u32 pitch = fb_stride % 64 ? 2 * fb_stride : fb_stride;
	if (gpu_scratch[0] == GPU_MAGIC && fb && pitch % 64 == 0) {
		uintptr_t src = fb, dst = fb + (uintptr_t)(2220 - GPU_BLIT_ROWS) * fb_stride;
		u32 lines = GPU_BLIT_ROWS * fb_stride / pitch, n = GPU_BLIT_ROWS * fb_stride / 4;
		/* magenta under the copy: words still magenta are ones the GPU never wrote */
		for (u32 k = 0; k < n; k++)
			wr32(dst + 4 * k, 0xffff00ff);
		gpu_scratch[1] = 0;
		w = gpu_blit_emit(gpu_ring, w, src, dst, pitch / 4, lines, pitch,
				  (uintptr_t)&gpu_scratch[1]);
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(GR(0x807), w);
		for (i = 0; i < 2000 && gpu_scratch[1] != GPU_BLIT_DONE; i++)
			delay_us(500);
		u32 black = 0, untouched = 0;
		for (u32 k = 0; k < n; k++) {
			u32 d = rd32(dst + 4 * k);
			black += d == 0;
			untouched += d == 0xffff00ff;
		}
		log_printf("gpu: 2D %ux%u RGBA8 rect, rptr %u/%u done %x after %d ms: %u of %u words black, %u untouched\n",
			   pitch / 4, lines, rd32(GR(0x806)), w, gpu_scratch[1], i / 2, black, n,
			   untouched);
		log_printf("gpu: %s\n", gpu_scratch[1] == GPU_BLIT_DONE && black == n ?
			   "THE GPU DREW ON THE SCREEN (the magenta block is now black)" : "2D draw failed");
		log_printf("gpu: rbbm status %x hw fault %x int0 %x\n", rd32(GR(0x4f5)), rd32(GR(0xb1a)),
			   rd32(GR(0x4e1)));
		delay_us(5000000);	/* time to look at the copy before Linux takes the screen */
	}
	/* TZ lets go of the zap again, so Linux's kgsl can load it afresh */
	if (!zap)
		log_printf("gpu: trust cntl %x, pas shutdown rc %ld\n", rd32(GR(0xf400)),
			   scm_pas(PAS_SHUTDOWN, 1, GPU_PAS_ID, 0, 0));
	__asm__ volatile("msr daifset, #4; isb");
	/* the GDSCs collapse next (gpu_probe), which stops the CP with everything else */
}

/*
 * A minimal EL1 MMU for the GPU probe, so MMIO is Device-nGnRE as under
 * Linux instead of the MMU-off Device-nGnRnE (the GPU SMMU's TLB sync
 * never completes for the shim, and completes at once for Linux). Identity
 * map in 1 GiB blocks: below 2 GiB device, RAM above normal non-cacheable,
 * so nothing needs cache maintenance; caches stay off. Off again before
 * Linux or XNU.
 */
static u64 mmu_l1[512] __attribute__((aligned(4096)));

static void mmu_on(void)
{
	for (u64 i = 0; i < 16; i++)	/* block, AF; AttrIndx 0 device (XN), 1 normal NC */
		mmu_l1[i] = i << 30 | 1 | 1U << 10 |
			    (i < 2 ? 0 | 3UL << 53 : 1U << 2);
	__asm__ volatile("dsb sy" ::: "memory");
	wsr("mair_el1", 0x4404);			/* attr0 Device-nGnRE, attr1 Normal NC */
	wsr("tcr_el1", 25 | 1UL << 23 | 2UL << 32);	/* 39-bit VA, 4K, NC walks, no TTBR1, 40-bit PA */
	wsr("ttbr0_el1", (uintptr_t)mmu_l1);
	__asm__ volatile("isb; tlbi vmalle1; dsb sy; isb" ::: "memory");
	wsr("sctlr_el1", rsr("sctlr_el1") | 1);
	__asm__ volatile("isb" ::: "memory");
	log_printf("mmu: on, sctlr %lx\n", rsr("sctlr_el1"));
}

static void mmu_off(void)
{
	wsr("sctlr_el1", rsr("sctlr_el1") & ~1UL);
	__asm__ volatile("isb; tlbi vmalle1; dsb sy; isb" ::: "memory");
	log_printf("mmu: off, sctlr %lx\n", rsr("sctlr_el1"));
}

static void gpu_probe(void)
{
	int f1 = 0, f2 = 0;
	log_printf("gpu: gcc ahb %x bimc %x bimc_gfx %x\n", rd32(GPU_GCC_AHB), rd32(GPU_GCC_BIMC),
		   rd32(GPU_GCC_BIMC_GFX));
	int rc = branch_on(GPU_GCC_AHB) | branch_on(GPU_GCC_BIMC) | branch_on(GPU_GCC_BIMC_GFX);
	u32 cx0 = probe_rd32(GPU_CX_GDSC, &f1), gx0 = probe_rd32(GPU_GX_GDSC, &f2);
	log_printf("gpu: gcc clocks rc %d; gpucc readable %s: cx gdsc %x gx gdsc %x\n", rc,
		   (f1 | f2) ? "NO (abort)" : "yes", cx0, gx0);
	if (rc || f1 || f2)
		return;
	rc = branch_on(GPUCC_CXO_CBCR);
	rc |= rcg_xo(GPUCC_RBBMTIMER_RCGR, 0) << 1;
	rc |= branch_on(GPUCC_RBBMTIMER_CBCR) << 2;
	rc |= rcg_xo(GPUCC_GFX3D_RCGR, 1) << 3;
	rc |= gdsc_on(GPU_CX_GDSC) << 4;
	/* the SMMU first, with only CX up and the GX TBU still off, as Linux's arm-smmu probe */
	__asm__ volatile("msr daifclr, #4; isb");
	gpu_smmu_rc = gpu_smmu_bypass();
	__asm__ volatile("msr daifset, #4; isb");
	/* GX: block reset pulse, GMEM reset pulse, clamp off, then power */
	wr32(GPU_GX_BCR, rd32(GPU_GX_BCR) | 1);
	delay_us(1);
	wr32(GPU_GX_BCR, rd32(GPU_GX_BCR) & ~1U);
	wr32(GPU_GX_DOMAIN_MISC, rd32(GPU_GX_DOMAIN_MISC) | (1U << 4));
	delay_us(1);
	wr32(GPU_GX_DOMAIN_MISC, rd32(GPU_GX_DOMAIN_MISC) & ~(1U << 4));
	wr32(GPU_GX_DOMAIN_MISC, rd32(GPU_GX_DOMAIN_MISC) & ~1U);
	rc |= gdsc_on(GPU_GX_GDSC) << 5;
	rc |= branch_on(GPUCC_GFX3D_CBCR) << 6;
	log_printf("gpu: power rc %x: cx gdsc %x gx gdsc %x gfx3d cbcr %x rbbmtimer cbcr %x\n", rc,
		   rd32(GPU_CX_GDSC), rd32(GPU_GX_GDSC), rd32(GPUCC_GFX3D_CBCR),
		   rd32(GPUCC_RBBMTIMER_CBCR));
	int fa = 0, fb = 0, fs = 0;
	u32 a = probe_rd32(A5XX_ALWAYSON_LO, &fa);
	delay_us(1000);
	u32 b = probe_rd32(A5XX_ALWAYSON_LO, &fb);
	u32 st = probe_rd32(A5XX_RBBM_STATUS, &fs);
	log_printf("gpu: alwayson %x -> %x after 1 ms%s, rbbm status %x%s\n", a, b,
		   (fa | fb) ? " (abort)" : "", st, fs ? " (abort)" : "");
	log_printf("gpu: %s\n", !(fa | fb) && b != a ? "ALIVE: the counter runs" : "no sign of life");
	if (!(fa | fb) && b != a)
		gpu_cp_test();
	gpu_smmu_restore();	/* while the CX domain (and the SMMU in it) is still up */
	/* all off again for Linux's kgsl, in reverse */
	wr32(GPUCC_GFX3D_CBCR, rd32(GPUCC_GFX3D_CBCR) & ~1U);
	wr32(GPU_GX_GDSC, rd32(GPU_GX_GDSC) | 1);
	wr32(GPU_GX_DOMAIN_MISC, rd32(GPU_GX_DOMAIN_MISC) | 1);
	wr32(GPU_CX_GDSC, rd32(GPU_CX_GDSC) | 1);
	wr32(GPUCC_RBBMTIMER_CBCR, rd32(GPUCC_RBBMTIMER_CBCR) & ~1U);
}

/* Vol Down held at some point in the next ms (PM660 PON RESIN) */
static int vol_down_within(u32 ms)
{
	u64 end = ticks() + (u64)ms * (rsr("cntfrq_el0") / 1000);
	while (ticks() < end) {
		u8 sts = 0;
		if (spmi_read(0, PON_INT_RT_STS, &sts) == 0 && (sts & PON_RESIN_ON))
			return 1;
		delay_us(20000);
	}
	return 0;
}

/* While the log is on screen: report what the panel sees (IRQ GPIO 67 low = report waiting). */
static void touch_poll(u32 ms)
{
	u64 end = ticks() + (u64)ms * (rsr("cntfrq_el0") / 1000);
	int events = 0;
	u8 clr[2] = { 0x03, 0 };
	u16 last = 0xffff;
	int lows = 0, polls = 0;
	while (ticks() < end && events < 24) {
		/* read every 20 ms whatever the IRQ line says; log what changed */
		int irq_low = !(rd32(TLMM_IO(67)) & 1);
		lows += irq_low;
		polls++;
		u8 p[34] = { 0 };
		if (zt_read(0x0080, p, sizeof(p)) == 0) {
			u16 status = p[0] | p[1] << 8;
			if (status != last) {
				log_printf("touch: status %x cnt %u irq %s\n", status, p[2],
					   irq_low ? "low" : "high");
				last = status;
				events++;
			}
			for (int f = 0; f < 5; f++) {
				u8 *c = p + 4 + 6 * f;
				if (c[5] & 1) {	/* SUB_BIT_EXIST */
					log_printf("touch: finger %d x %u y %u w %u sub %x\n", f,
						   c[0] | c[1] << 8, c[2] | c[3] << 8, c[4], c[5]);
					events++;
				}
			}
		}
		if (irq_low)
			qup_msg(0, clr, 2);
		delay_us(20000);
	}
	log_printf("touch: %d events, irq low in %d of %d polls\n", events, lows, polls);
}

/* sleep clock, 32768 Hz; the bark comes after the bite, so XNU never sees its interrupt */
static void wdt_arm(u32 secs)
{
	wr32(WDT_BASE + 0x08, 0);
	wr32(WDT_BASE + 0x10, 0xfffff);
	wr32(WDT_BASE + 0x14, secs * 32768);
	wr32(WDT_BASE + 0x04, 1);
	wr32(WDT_BASE + 0x08, 1);
	log_printf("wdt: bite in %u s unless XNU pets it\n", secs);
}

static void log_pon(void)
{
	u8 ctl = 0, ctl2 = 0, spare = 0;
	u32 apid = 0, owner = 0;
	int rc = spmi_apid(0, 0x800, &apid, &owner);
	log_printf("spmi: arb version %x, PON apid %u owner %u (rc %d)\n", rd32(SPMI_CORE), apid,
		   owner, rc);
	rc = spmi_read(0, PON_PS_HOLD_RST_CTL, &ctl);
	rc |= spmi_read(0, PON_PS_HOLD_RST_CTL2, &ctl2);
	rc |= spmi_read(0, PON_SOFT_RB_SPARE, &spare);
	log_printf("pon: ps_hold ctl %x ctl2 %x spare %x (rc %d)\n", ctl, ctl2, spare, rc);
}

/* ---- FDT: /chosen linux,initrd-start/end ---- */

static u32 be32(uintptr_t p)
{
	const volatile u8 *b = (const volatile u8 *)p;
	return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}

static int streq(uintptr_t a, const char *b)
{
	const volatile char *s = (const volatile char *)a;
	while (*s && *s == *b)
		s++, b++;
	return *s == *b;
}

/* address of /chosen's property NAME's value, 0 if none; *lenp = its length */
static uintptr_t fdt_chosen_prop(uintptr_t dtb, const char *name, u32 *lenp)
{
	if (!dtb || be32(dtb) != 0xd00dfeed)
		return 0;
	uintptr_t p = dtb + be32(dtb + 8), strs = dtb + be32(dtb + 12);
	int depth = 0, in_chosen = 0;
	for (;;) {
		u32 tok = be32(p);
		p += 4;
		if (tok == 1) {			/* BEGIN_NODE */
			depth++;
			if (depth == 2)
				in_chosen = streq(p, "chosen");
			while (*(volatile char *)p)
				p++;
			p = (p + 4) & ~3UL;
		} else if (tok == 2) {		/* END_NODE */
			if (depth-- == 2)
				in_chosen = 0;
		} else if (tok == 3) {		/* PROP */
			u32 len = be32(p), off = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && depth == 2 && streq(strs + off, name)) {
				*lenp = len;
				return val;
			}
			p = (val + len + 3) & ~3UL;
		} else if (tok != 4) {		/* not NOP: END or garbage */
			return 0;
		}
	}
}

static u64 fdt_chosen_u64(uintptr_t dtb, const char *name)
{
	u32 len = 0;
	uintptr_t val = fdt_chosen_prop(dtb, name, &len);
	if (!val)
		return 0;
	return len == 8 ? (u64)be32(val) << 32 | be32(val + 4) : len == 4 ? be32(val) : 0;
}

static void wr_be32(uintptr_t p, u32 v)
{
	for (int i = 0; i < 4; i++)
		wr8(p + i, v >> (24 - 8 * i));
}

/* rewrite an existing u32/u64 /chosen property in place */
static int fdt_chosen_set_u64(uintptr_t dtb, const char *name, u64 v)
{
	u32 len = 0;
	uintptr_t val = fdt_chosen_prop(dtb, name, &len);
	if (!val || (len != 8 && len != 4) || (len == 4 && v >> 32))
		return -1;
	if (len == 8) {
		wr_be32(val, v >> 32);
		val += 4;
	}
	wr_be32(val, (u32)v);
	return 0;
}

/* ---- leaving: chain-load the Linux Image appended at PAYLOAD_OFF ---- */

extern char _head[];
static u64 boot_dtb;

static void __attribute__((noreturn)) stop(const char *why)
{
	con_fg = 0xffff40;
	log_printf("%s\nhold Power + Vol Up + Bixby for OrangeFox\n", why);
	for (;;)
		__asm__ volatile("wfe");
}

static int overlaps(u64 a, u64 alen, u64 b, u64 blen)
{
	return a < b + blen && b < a + alen;
}

static void __attribute__((noreturn)) boot_linux(void)
{
	uintptr_t base = (uintptr_t)_head;
	u64 reserved = *(volatile u64 *)(base + 0x10);	/* our header's image_size */
	uintptr_t img = base + PAYLOAD_OFF;
	if (rd32(img + 0x38) != 0x644d5241)	/* "ARM\x64" */
		stop("no Linux Image at the payload offset");
	u64 text_offset = *(volatile u64 *)(img + 0x08);
	u64 isz = *(volatile u64 *)(img + 0x10);
	u64 fsz = *(volatile u64 *)(base + PAYLOAD_SIZE_AT);
	uintptr_t dst = ((img - text_offset + 0x1fffff) & ~0x1fffffUL) + text_offset;
	u64 rd_s = fdt_chosen_u64(boot_dtb, "linux,initrd-start");
	u64 rd_e = fdt_chosen_u64(boot_dtb, "linux,initrd-end");
	u64 los_rd = *(volatile u64 *)(base + LOS_RD_SIZE_AT);
	if (*(volatile u64 *)(base + XNU_OFF_AT) && rd_s && los_rd < rd_e - rd_s) {
		/* Linux gets only its own ramdisk, not the XNU payload behind it */
		if (fdt_chosen_set_u64(boot_dtb, "linux,initrd-end", rd_s + los_rd))
			stop("cannot trim linux,initrd-end");
		rd_e = rd_s + los_rd;
	}
	log_printf("linux: image %lx size %lx/%lx text_offset %lx -> %lx\n", (u64)img, fsz, isz,
		   text_offset, (u64)dst);
	log_printf("linux: reserved %lx-%lx, dtb %lx, initrd %lx-%lx\n", (u64)base,
		   (u64)base + reserved, boot_dtb, rd_s, rd_e);
	if (dst + isz > base + reserved)
		stop("Linux does not fit in the reserved area");
	if (overlaps(dst, isz, boot_dtb, 0x100000) || (rd_s && overlaps(dst, isz, rd_s, rd_e - rd_s)))
		stop("Linux would overlap the DTB or the initrd");
	if (dst != img) {			/* move up: copy from the end */
		volatile u64 *d = (volatile u64 *)dst, *s = (volatile u64 *)img;
		for (u64 i = (fsz + 7) / 8; i--;)
			d[i] = s[i];
	}
	log_printf("linux: jumping to %lx\n", (u64)dst);
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	register u64 x0 __asm__("x0") = boot_dtb;
	register u64 x1 __asm__("x1") = 0;
	register u64 x2 __asm__("x2") = 0;
	register u64 x3 __asm__("x3") = 0;
	register u64 x4 __asm__("x4") = dst;
	__asm__ volatile("br x4" : : "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4));
	__builtin_unreachable();
}

static volatile int in_exception;

void exc_handler(u64 slot, u64 *regs)
{
	/* data abort at the probe_rd32() load: flag it and step over the load */
	if (probe_fault && rsr("elr_el1") == (u64)probe_rd32_insn &&
	    (rsr("esr_el1") >> 26) == 0x25) {
		*(volatile int *)probe_fault = 1;
		regs[0] = 0;
		__asm__ volatile("msr elr_el1, %0" : : "r"((u64)probe_rd32_insn + 4));
		return;
	}
	if (in_exception++)
		for (;;)
			__asm__ volatile("wfe");
	con_fg = 0xff4040;
	log_printf("\n*** EXCEPTION slot %lu esr %lx elr %lx far %lx x0 %lx lr %lx\n", slot,
		   rsr("esr_el1"), rsr("elr_el1"), rsr("far_el1"), regs[0], regs[31]);
	boot_linux();
}

/*
 * ---- leaving for XNU: act as iBoot ----
 * After ios-flame's shim/loader.c (MIT, dipa-studio). tools/mkpayload.py has
 * already resolved the kernel's fixups and attached the ADT; the payload is
 * the boot image's ramdisk. Copy the segments to phys_base + (vmaddr -
 * virt_base), put the ADT and boot_args behind the kernel, and enter with
 * the MMU and caches off, x0 = boot_args (physical).
 */
#define BOOT_LINE_LENGTH	608
#define BA_ALIGN		0x4000UL

struct payload_seg {
	u64 vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	u32 version, nsegs;
	u64 virt_base, entry_va, adt_off, adt_size, kernel_end;
	u64 phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	u64 v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	u16 Revision, Version;
	u64 virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	u32 machineType;
	u64 deviceTreeP;
	u32 deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	u64 bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so aligned 8-byte words */
static void copy64(uintptr_t dst, uintptr_t src, u64 n)
{
	volatile u64 *d = (volatile u64 *)dst;
	const volatile u64 *s = (const volatile u64 *)src;
	for (u64 i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, u64 n)
{
	volatile u64 *d = (volatile u64 *)dst;
	for (u64 i = 0; i < n / 8; i++)
		d[i] = 0;
}

static const volatile struct payload *xnu_payload(void)
{
	u64 off = *(volatile u64 *)((uintptr_t)_head + XNU_OFF_AT);
	u64 rd = fdt_chosen_u64(boot_dtb, "linux,initrd-start");
	if (!off || !rd)
		return 0;
	rd += off;
	if (rd & 7)
		return 0;
	const volatile struct payload *p = (const volatile struct payload *)rd;
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (p->magic[i] != want[i])
			return 0;
	return p;
}

static void __attribute__((noreturn)) boot_xnu(const volatile struct payload *p)
{
	u64 rd_s = (uintptr_t)p, rd_e = fdt_chosen_u64(boot_dtb, "linux,initrd-end");
	if (p->version != 1 || (p->adt_off & 7))
		stop("bad XNU payload header");
	u64 pa_of_va = p->phys_base - p->virt_base;	/* pa = va + this */
	log_printf("xnu: payload %lx-%lx, phys %lx size %lx virt %lx entry %lx\n", rd_s, rd_e,
		   p->phys_base, p->mem_size, p->virt_base, p->entry_va);
	if (overlaps(rd_s, rd_e - rd_s, p->phys_base, p->mem_size))
		stop("XNU payload overlaps the kernel's RAM");

	for (u32 i = 0; i < p->nsegs; i++) {
		const volatile struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va;
		copy64(dst, rd_s + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}

	/* ADT, then boot_args, behind the kernel: everything below topOfKernelData is XNU's */
	uintptr_t adt_pa = p->kernel_end + pa_of_va;
	copy64(adt_pa, rd_s + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + BA_ALIGN - 1) & ~(BA_ALIGN - 1);
	zero64(ba_pa, BA_ALIGN);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = ba_pa + BA_ALIGN;
	ba->Video.v_baseAddr = fb;
	ba->Video.v_display = fb != 0;
	ba->Video.v_rowBytes = fb_stride;
	ba->Video.v_width = PANEL_W;
	ba->Video.v_height = PANEL_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va;	/* XNU reads it with the MMU on: a VA */
	ba->deviceTreeLength = (u32)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;

	uintptr_t entry = p->entry_va + pa_of_va;
	log_printf("xnu: %u segments, adt %lx, boot_args %lx, jumping to %lx\n", p->nsegs,
		   (u64)adt_pa, (u64)ba_pa, (u64)entry);
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	register u64 x0 __asm__("x0") = ba_pa;
	register u64 x4 __asm__("x4") = entry;
	__asm__ volatile("br x4" : : "r"(x0), "r"(x4));
	__builtin_unreachable();
}

/*
 * After a failed XNU try: XNU's console mirror (bsd/dev/sweet_ufs.c:
 * "SWEETMIR" | u64 total | u32 ring size, ring at +0x1000) into the ramoops
 * console zone, its last lines on screen too.
 */
#define RESCUE_SCREEN	2048

static void xnu_log_rescue(void)
{
	static const char want[] = "SWEETMIR";
	for (int i = 0; i < 8; i++)
		if (*(volatile char *)(XNU_MIRROR + i) != want[i]) {
			log_printf("xnu: no console mirror at %lx (died before the platform expert)\n",
				   XNU_MIRROR);
			return;
		}
	u64 total = *(volatile u64 *)(XNU_MIRROR + 8);
	u32 size = rd32(XNU_MIRROR + 16);
	if (!size || size > (16u << 20)) {
		log_printf("xnu: console mirror ring size %x?\n", size);
		return;
	}
	u64 room = RAMOOPS_ZONE - 12 - 16 - rz_len - 4096;	/* keep room for our own lines */
	u64 n = total < size ? total : size;
	if (n > room)
		n = room;
	log_printf("xnu: console mirror has %lu bytes, last %lu follow\n", total, n);
	ramoops_putc('\n');
	for (u64 i = total - n; i < total; i++) {
		char c = *(volatile char *)(XNU_MIRROR + 0x1000 + i % size);
		ramoops_putc(c);
		if (total - i <= RESCUE_SCREEN)
			con_putc(c);
	}
	log_printf("\nxnu: end of console mirror\n");
}

void shim_main(u64 dtb)
{
	boot_dtb = dtb;
	ramoops_init();
	fb_find();
	con_clear();
	con_fg = 0x40ff40;
	log_printf("a9y18qlte probe shim (stage 1 of an XNU port)\n");
	con_fg = 0xffffff;
	log_printf("timer %lu ms since reset, shim at %lx\n", ticks() / (rsr("cntfrq_el0") / 1000),
		   (u64)_head);
	wdt_off();
	log_cpu(dtb);
	log_psci();
	log_gic();
	log_pon();
	fb_log();
	touch_probe();
	if (!xnu_payload()) {	/* XNU images use Vol Down to boot XNU */
		con_fg = 0xffff40;
		log_printf("hold Vol Down in the next 10 s to run the GPU probe\n");
		con_fg = 0xffffff;
		if (vol_down_within(10000)) {
			mmu_on();
			gpu_probe();
			mmu_off();
		} else
			log_printf("gpu: probe skipped\n");
	}
	const volatile struct payload *xnu = xnu_payload();
	volatile u64 *mark = (volatile u64 *)TRY_MARK;
#ifdef TEST_FALLBACK	/* SHIM_CFLAGS=-DTEST_FALLBACK build.sh --xnu: the way back, without XNU */
	*mark = TRY_MAGIC;
#endif
	if (xnu && *mark == TRY_MAGIC) {
		*mark = 0;
		con_fg = 0xffff40;
		log_printf("XNU already had its try this boot: back to Linux\n");
		con_fg = 0xffffff;
		xnu_log_rescue();
		xnu = 0;
	}
	/*
	 * XNU only on request: Vol Down held at some point in the hold time. A
	 * reset that loses TRY_MARK (the first try on the phone did) then still
	 * comes back to Linux instead of looping.
	 */
	con_fg = 0x40ff40;
	if (xnu)
		log_printf("probe done: hold Vol Down in the next %u s for XNU, else Linux\n",
			   HOLD_MS / 1000);
	else
		log_printf("probe done, Linux in %u s\n", HOLD_MS / 1000);
	con_fg = 0xffffff;
	if (!xnu)
		touch_poll(HOLD_MS);
	int want_xnu = 0;
	u64 end = ticks() + (u64)HOLD_MS * (rsr("cntfrq_el0") / 1000);
	while (ticks() < end) {
		u8 sts = 0;
		if (xnu && !want_xnu && spmi_read(0, PON_INT_RT_STS, &sts) == 0 &&
		    (sts & PON_RESIN_ON)) {
			want_xnu = 1;
			con_fg = 0x40ff40;
			log_printf("Vol Down: XNU\n");
			con_fg = 0xffffff;
		}
		delay_us(20000);
	}
	if (xnu && want_xnu) {
		*mark = TRY_MAGIC;
		wr32(XNU_MIRROR, 0);	/* a mirror found after the reset is this try's */
		wdt_arm(XNU_WDT_S);
		boot_xnu(xnu);
	}
	boot_linux();
}
