// SPDX-License-Identifier: GPL-2.0
/*
 * Dump the Adreno 512 SMMU (arm,smmu-kgsl@5040000) as Linux's kgsl left it,
 * for the a9y18qlte shim's GPU bring-up. Load with the GPU forced on
 * (kgsl-3d0 force_clk_on/force_rail_on/force_bus_on); fails to load on purpose.
 */
#include <linux/module.h>
#include <linux/io.h>

#define SMMU_BASE	0x05040000
#define GPU_BASE	0x05000000

static int __init smmudump_init(void)
{
	void __iomem *s = ioremap(SMMU_BASE, 0x10000);
	void __iomem *g = ioremap(GPU_BASE, 0x40000);
	int i;

	if (!s || !g)
		return -ENOMEM;
	pr_info("smmudump: cr0 %x cr2 %x acr %x idr0 %x idr1 %x idr2 %x gfsr %x\n",
		readl(s), readl(s + 8), readl(s + 0x10), readl(s + 0x20), readl(s + 0x24),
		readl(s + 0x28), readl(s + 0x48));
	for (i = 0; i < 3; i++)
		pr_info("smmudump: smr%d %x s2cr%d %x\n", i, readl(s + 0x800 + 4 * i), i,
			readl(s + 0xc00 + 4 * i));
	for (i = 0; i < 5; i++) {
		void __iomem *cb = s + 0x8000 + 0x1000 * i;

		pr_info("smmudump: cb%d cbar %x cba2r %x cbfrsynra %x sctlr %x actlr %x tcr %x tcr2 %x ttbr0 %x:%x ttbr1 %x:%x mair0 %x fsr %x\n",
			i, readl(s + 0x1000 + 4 * i), readl(s + 0x1800 + 4 * i),
			readl(s + 0x1400 + 4 * i), readl(cb), readl(cb + 4), readl(cb + 0x30),
			readl(cb + 0x10), readl(cb + 0x24), readl(cb + 0x20), readl(cb + 0x2c),
			readl(cb + 0x28), readl(cb + 0x38), readl(cb + 0x58));
	}
	for (i = 0x2000; i < 0x2010; i += 4)
		pr_info("smmudump: impl %x = %x\n", i, readl(s + i));
	pr_info("smmudump: gpu trust cntl %x rbbm status %x cp rb base %x:%x rptr %x wptr %x me cntl %x\n",
		readl(g + 0xf400 * 4), readl(g + 0x4f5 * 4), readl(g + 0x801 * 4),
		readl(g + 0x800 * 4), readl(g + 0x806 * 4), readl(g + 0x807 * 4),
		readl(g + 0x832 * 4));
	{
		void __iomem *cc = ioremap(0x05065000, 0x2000);
		void __iomem *gcc = ioremap(0x00171000, 0x100);

		static const u16 ccr[] = { 0x0, 0x4, 0x8, 0xc, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24,
			0x40, 0x1004, 0x1008, 0x1020, 0x1030, 0x1034, 0x1054, 0x1070, 0x1074,
			0x1090, 0x1094, 0x1098, 0x10b0, 0x10b4, 0x10d0, 0x130 };
		static const u32 gccr[] = { 0x71004, 0x71010, 0x7106c, 0x5200c, 0x52004 };
		void __iomem *gcc0 = ioremap(0x00100000, 0x94000);

		for (i = 0; cc && i < ARRAY_SIZE(ccr); i++)
			pr_info("smmudump: gpucc %x = %x\n", ccr[i], readl(cc + ccr[i]));
		for (i = 0; gcc0 && i < ARRAY_SIZE(gccr); i++)
			pr_info("smmudump: gcc %x = %x\n", gccr[i], readl(gcc0 + gccr[i]));
		if (gcc0)
			iounmap(gcc0);
		{
			static const u16 impl[] = { 0x6000, 0x6060, 0x678c, 0x6794, 0x6800, 0x6900,
				0x6924, 0x6928, 0x6930, 0x6960, 0x6b64, 0x6b68 };

			for (i = 0; i < ARRAY_SIZE(impl); i++)
				pr_info("smmudump: impl %x = %x\n", impl[i], readl(s + impl[i]));
		}
		pr_info("smmudump: micro mmu ctrl %x tlbgstatus %x\n", readl(s + 0x6000),
			readl(s + 0x74));
		if (cc)
			iounmap(cc);
		if (gcc)
			iounmap(gcc);
	}
	iounmap(g);
	iounmap(s);
	return -EAGAIN;
}
module_init(smmudump_init);
MODULE_LICENSE("GPL");
