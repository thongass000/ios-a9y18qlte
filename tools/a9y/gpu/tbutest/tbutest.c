// SPDX-License-Identifier: GPL-2.0
/*
 * Replays the a9y18qlte shim's Adreno 512 power-up on a GPU that kgsl has
 * power-collapsed, then tries a global TLB sync on the GPU SMMU, which
 * stalls forever in the shim. Under Linux the write is posted, so a stuck
 * TBU shows as sTLBGSTATUS.GSACTIVE staying set. step= adds Linux's extra
 * setup one piece at a time. Always fails to load.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/delay.h>

static int step;
module_param(step, int, 0);

#define GCC		0x00100000
#define GPUCC		0x05065000
#define SMMU		0x05040000

static void __iomem *gcc, *cc, *smmu;

static int branch_on(u32 off, void __iomem *base)
{
	int i;

	writel(readl(base + off) | 1, base + off);
	for (i = 0; i < 1000; i++) {
		if (!(readl(base + off) >> 31))
			return 0;
		udelay(1);
	}
	return -1;
}

static int poll_bit(void __iomem *a, u32 bit, int set)
{
	int i;

	for (i = 0; i < 2000; i++) {
		if (!!(readl(a) & bit) == set)
			return 0;
		udelay(1);
	}
	return -1;
}

static int __init tbutest_init(void)
{
	int rc = 0, i;
	u32 v;

	gcc = ioremap(GCC, 0x94000);
	cc = ioremap(GPUCC, 0x2000);
	smmu = ioremap(SMMU, 0x10000);
	if (!gcc || !cc || !smmu)
		goto out;
	v = readl(cc + 0x1004);
	pr_info("tbutest: step %d, cx gdsc %x gx gdsc %x\n", step, v, readl(cc + 0x1094));
	if (v >> 31) {
		pr_info("tbutest: GPU is on (kgsl active), not testing\n");
		goto out;
	}
	/* the shim's sequence */
	rc |= branch_on(0x71004, gcc) | branch_on(0x71010, gcc) | branch_on(0x7106c, gcc);
	rc |= branch_on(0x1020, cc) << 1;
	writel(0, cc + 0x10b4);			/* rbbmtimer RCG: XO */
	writel(readl(cc + 0x10b0) | 1, cc + 0x10b0);
	poll_bit(cc + 0x10b0, 1, 0);
	rc |= branch_on(0x10d0, cc) << 2;
	if (step >= 1) {			/* Linux: GPLL0 to the GPU CC, gfx3d off PLL1 as Linux */
		writel(readl(gcc + 0x5200c) | 1 << 4, gcc + 0x5200c);
		writel(0x303, cc + 0x1074);
	} else {
		writel(0, cc + 0x1074);
	}
	writel(readl(cc + 0x1070) | 3, cc + 0x1070);
	poll_bit(cc + 0x1070, 1, 0);
	writel(readl(cc + 0x1004) & ~1U, cc + 0x1004);	/* CX GDSC */
	rc |= poll_bit(cc + 0x1004, 1U << 31, 1) << 3;
	writel(readl(cc + 0x1090) | 1, cc + 0x1090);	/* GX: BCR, GMEM reset, clamp, power */
	udelay(1);
	writel(readl(cc + 0x1090) & ~1U, cc + 0x1090);
	writel(readl(cc + 0x130) | 1 << 4, cc + 0x130);
	udelay(1);
	writel(readl(cc + 0x130) & ~(1U << 4), cc + 0x130);
	writel(readl(cc + 0x130) & ~1U, cc + 0x130);
	writel(readl(cc + 0x1094) & ~1U, cc + 0x1094);
	rc |= poll_bit(cc + 0x1094, 1U << 31, 1) << 4;
	rc |= branch_on(0x1098, cc) << 5;
	pr_info("tbutest: power rc %x cx %x gx %x gfx3d cbcr %x rcg cfg %x misc %x\n", rc,
		readl(cc + 0x1004), readl(cc + 0x1094), readl(cc + 0x1098), readl(cc + 0x1074),
		readl(cc + 0x130));
	pr_info("tbutest: smmu cr0 %x smr0 %x s2cr0 %x micro %x tlbgstatus %x\n", readl(smmu),
		readl(smmu + 0x800), readl(smmu + 0xc00), readl(smmu + 0x6000), readl(smmu + 0x74));
	writel(~0U, smmu + 0x68);		/* TLBIALLNSNH */
	writel(0, smmu + 0x70);			/* sTLBGSYNC */
	for (i = 0; i < 10000 && (readl(smmu + 0x74) & 1); i++)
		udelay(1);
	pr_info("tbutest: TLB SYNC %s after %d us (tlbgstatus %x)\n",
		i < 10000 ? "COMPLETED" : "STUCK", i, readl(smmu + 0x74));
	/* off again, as the shim */
	writel(readl(cc + 0x1098) & ~1U, cc + 0x1098);
	writel(readl(cc + 0x1094) | 1, cc + 0x1094);
	writel(readl(cc + 0x130) | 1, cc + 0x130);
	writel(readl(cc + 0x1004) | 1, cc + 0x1004);
	writel(readl(cc + 0x10d0) & ~1U, cc + 0x10d0);
out:
	if (smmu)
		iounmap(smmu);
	if (cc)
		iounmap(cc);
	if (gcc)
		iounmap(gcc);
	return -EAGAIN;
}
module_init(tbutest_init);
MODULE_LICENSE("GPL");
