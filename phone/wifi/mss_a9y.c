/*
 * The Galaxy A9 2018's (sdm660) modem boot from userland (/dev/pmem). TZ refuses PAS for this modem
 * (PAS_IS_SUPPORTED 4 -> EOPNOTSUPP), so as Linux pil-msa.c + pil-q6v5.c for qcom,pil-q6v55-mss,
 * qcom,pil-self-auth, qcom,qdsp6v62-1-5, steps 1-2 first proven in the a9y18qlte probe shim (mss_probe):
 *   1. mba.mbn (as it is: the PBL parses it) in pil_mba_region, handed to the modem's VM; MSS clocks,
 *      MSS reset pulse, Q6 power-up; the PBL (RMB_PBL_STATUS 1), then the MBA (RMB_MBA_STATUS 1/2).
 *   2. modem.mdt (ELF headers + hash segment) to the MBA: META_DATA_READY -> status 3.
 *   3. modem_fw_region to HLOS + modem, every loadable segment in at its address with the MBA told
 *      each one (CODE_START + LOAD_READY once, then CODE_LENGTH grows by its memsz), the region to the
 *      modem only, then status 4 (AUTH_COMPLETE): the modem runs.
 * Linux's proxy votes (vdd_cx/vdd_mx turbo through the RPM) are left out: the modem votes for itself
 * once it runs, and the MBA came up on the levels ABL leaves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "board.h"
#include "mss.h"
#include "pmem.h"
#include "scm.h"

#define FW_DIR			FW_ROOT "/modem"

#define MSS_QDSP6		0x04080000ull
#define MSS_RMB			0x04180000ull
#define MSS_HALT_Q6		0x01f63000ull
#define MSS_HALT_MODEM		0x01f65000ull
#define MSS_HALT_NC		0x01f64000ull
#define GCC			0x00100000ull
#define GCC_MSS_RESTART		(GCC + 0x79000)
#define GCC_MSS_CFG_AHB_CBCR	(GCC + 0x8a000)
#define GCC_MSS_MNOC_BIMC_CBCR	(GCC + 0x8a004)
#define GCC_MSS_SNOC_AXI_CBCR	(GCC + 0x8a03c)
#define GCC_BIMC_MSS_Q6_CBCR	(GCC + 0x4401c)
#define GCC_VOTE_0		(GCC + 0x52004)	/* bit 10: boot rom ahb */
#define GCC_VOTE_1		(GCC + 0x5200c)	/* bit 2: gpll0_out_msscc */

#define Q6_RESET		0x014
#define Q6_DBG_CFG		0x018
#define Q6_GFMUX_CTL		0x020
#define Q6_PWR_CTL		0x030
#define Q6_V6_MEM_PWR_CTL	0x034
#define Q6_XO_CBCR		0x038
#define Q6_V62_BHS_STATUS	0x0c4

#define RMB_MBA_IMAGE		0x00
#define RMB_PBL_STATUS		0x04
#define RMB_MBA_COMMAND		0x08
#define RMB_MBA_STATUS		0x0c
#define RMB_PMI_META_DATA	0x10
#define RMB_PMI_CODE_START	0x14
#define RMB_PMI_CODE_LENGTH	0x18
#define CMD_META_DATA_READY	1
#define CMD_LOAD_READY		2
#define CMD_PILFAIL_NFY_MBA	0xffffdead
#define STATUS_META_AUTH	3
#define STATUS_AUTH_COMPLETE	4

#define MBA_PA			0x94800000ull	/* pil_mba_region: MBA in the first MB, as Linux */
#define MBA_SIZE		0x100000ull
#define META_PA			0x94900000ull
#define MPSS_PA			0x8ac00000ull	/* modem_fw_region */
#define MPSS_SIZE		0x07e00000ull

static const uint32_t hlos[] = { VMID_HLOS }, msa[] = { VMID_MSS_MSA }, both[] = { VMID_HLOS, VMID_MSS_MSA };

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = calloc(1, (*len + 3) & ~(size_t)3);	/* /dev/pmem: whole words to device memory */
	if (b && fread(b, 1, *len, f) != *len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

static void zero_fill(uint64_t pa, uint64_t len)
{
	static const uint8_t zeros[0x10000];
	for (uint64_t o = 0; o < len; o += sizeof(zeros))
		pm_wr(pa + o, zeros, len - o < sizeof(zeros) ? (len - o + 3) & ~3ull : sizeof(zeros));
}

/* REG until non-zero, or (WANT != 0) until WANT or negative, for up to MS: the last value */
static uint32_t poll(uint64_t reg, uint32_t want, unsigned ms)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < ms * 10; i++) {
		v = rd32(reg);
		if (want ? (v == want || (int32_t)v < 0) : v != 0)
			break;
		usleep(100);
	}
	return v;
}

static int branch_on(uint64_t cbcr)
{
	wr32(cbcr, rd32(cbcr) | 1);
	for (int i = 0; i < 100; i++) {
		if (!(rd32(cbcr) >> 31))	/* CLK_OFF clear */
			return 0;
		usleep(10);
	}
	return -1;
}

static void log_rmb(const char *when)
{
	uint32_t v[9];
	for (int i = 0; i < 9; i++)
		v[i] = rd32(MSS_RMB + 4 * i);
	printf("mss: rmb %s: image %x pbl %x cmd %x status %x meta %x start %x len %x ver %x dbg %x\n", when, v[0], v[1],
	       v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
}

/* __pil_q6v55_reset for qdsp6v62-1-5 */
static int q6_power_up(void)
{
	const uint64_t q = MSS_QDSP6;
	uint32_t val = rd32(q + Q6_RESET);
	wr32(q + Q6_RESET, val | 1u << 1 | 1u << 2 | 1u << 0);	/* CORE_ARES | BUS_ARES_ENA | STOP_CORE */
	if (branch_on(q + Q6_XO_CBCR))
		return -1;
	val = rd32(q + Q6_PWR_CTL) | 1u << 24;			/* BHS_ON */
	wr32(q + Q6_PWR_CTL, val);
	usleep(1);
	int i;
	for (i = 200; i > 0 && !(rd32(q + Q6_V62_BHS_STATUS) & 1); i--)	/* BHS_EN_REST_ACK */
		usleep(1);
	if (!i)
		return -2;
	val |= 1u << 25;					/* LDO_BYP */
	wr32(q + Q6_PWR_CTL, val);
	val = rd32(q + Q6_PWR_CTL) & ~(1u << 22);		/* CLAMP_QMC_MEM off */
	wr32(q + Q6_PWR_CTL, val);
	val |= 1u << 18 | 1u << 19;				/* L2DATA_STBY_N | SLP_RET_N */
	wr32(q + Q6_PWR_CTL, val);
	val = rd32(q + Q6_V6_MEM_PWR_CTL);
	for (i = 29; i >= 0; i--) {				/* L1, L2, ETB, JU memories one at a time */
		val |= 1u << i;
		wr32(q + Q6_V6_MEM_PWR_CTL, val);
		usleep(1);
	}
	val = rd32(q + Q6_PWR_CTL) & ~(1u << 21);		/* CLAMP_WL off */
	wr32(q + Q6_PWR_CTL, val);
	val &= ~(1u << 20);					/* CLAMP_IO off */
	wr32(q + Q6_PWR_CTL, val);
	wr32(q + Q6_RESET, rd32(q + Q6_RESET) & ~(1u << 1 | 1u << 0));	/* out of reset, core running */
	wr32(q + Q6_GFMUX_CTL, rd32(q + Q6_GFMUX_CTL) | 1u << 1);	/* core clock on */
	return 0;
}

static void restart_pulse(void)
{
	wr32(GCC_MSS_RESTART, 1);
	usleep(200);
	wr32(GCC_MSS_RESTART, 0);
	usleep(200);
}

static void halt_axi(uint64_t halt)
{
	wr32(halt + 0, 1);			/* HALTREQ */
	uint32_t ack = poll(halt + 4, 0, 100);
	printf("mss: axi halt %llx: ack %x idle %x\n", (unsigned long long)halt, ack, rd32(halt + 8));
	wr32(halt + 0, 0);
}

/* pil_mss_shutdown: the modem stopped and in reset (memory stays wherever it was handed) */
void mss_shutdown(void)
{
	halt_axi(MSS_HALT_Q6);
	halt_axi(MSS_HALT_MODEM);
	halt_axi(MSS_HALT_NC);
	restart_pulse();
}

struct phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };

int mss_boot(void)
{
	size_t mba_len, mdt_len;
	uint8_t *mba = slurp(FW_DIR "/mba.mbn", &mba_len), *mdt = slurp(FW_DIR "/modem.mdt", &mdt_len);
	if (!mba || !mdt || mba_len > MBA_SIZE || mdt_len < 0x34) {
		fprintf(stderr, "mss: %s/mba.mbn or modem.mdt missing\n", FW_DIR);
		return -1;
	}
	uint32_t phoff;
	uint16_t phnum;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	if (phoff + 32ull * phnum > mdt_len) {
		fprintf(stderr, "mss: modem.mdt: program headers outside the file\n");
		return -1;
	}
	const struct phdr *ph = (const struct phdr *)(mdt + phoff);
	log_rmb("before");

	/* 1. the MBA */
	zero_fill(MBA_PA, MBA_SIZE);
	pm_wr(MBA_PA, mba, (mba_len + 3) & ~(size_t)3);
	if (hyp_assign(MBA_PA, MBA_SIZE, hlos, 1, msa, 1))
		return -1;
	wr32(GCC_VOTE_0, rd32(GCC_VOTE_0) | 1u << 10);
	wr32(GCC_VOTE_1, rd32(GCC_VOTE_1) | 1u << 2);
	int rc = branch_on(GCC_MSS_CFG_AHB_CBCR) | branch_on(GCC_BIMC_MSS_Q6_CBCR) << 1 |
		 branch_on(GCC_MSS_SNOC_AXI_CBCR) << 2 | branch_on(GCC_MSS_MNOC_BIMC_CBCR) << 3;
	uint32_t dbg = rd32(MSS_QDSP6 + Q6_DBG_CFG);
	printf("mss: clocks rc %x; q6 reset %x pwr %x dbg %x\n", rc, rd32(MSS_QDSP6 + Q6_RESET),
	       rd32(MSS_QDSP6 + Q6_PWR_CTL), dbg);
	restart_pulse();
	wr32(MSS_QDSP6 + Q6_DBG_CFG, dbg);
	wr32(MSS_RMB + RMB_MBA_IMAGE, (uint32_t)MBA_PA);
	wr32(MSS_RMB + RMB_PMI_CODE_START, 0);
	wr32(MSS_RMB + RMB_PMI_CODE_LENGTH, 0);
	if ((rc = q6_power_up())) {
		printf("mss: q6 power up failed (%d)\n", rc);
		goto fail;
	}
	uint32_t pbl = poll(MSS_RMB + RMB_PBL_STATUS, 0, 1000);
	uint32_t st = pbl == 1 ? poll(MSS_RMB + RMB_MBA_STATUS, 0, 1000) : 0;
	printf("mss: pbl status %x, mba status %x\n", pbl, st);
	if (pbl != 1 || (st != 1 && st != 2))
		goto fail;

	/* 2. the headers */
	uint32_t meta_size = (uint32_t)((mdt_len + 0xfff) & ~(size_t)0xfff);
	zero_fill(META_PA, meta_size);
	pm_wr(META_PA, mdt, (mdt_len + 3) & ~(size_t)3);
	if (hyp_assign(META_PA, meta_size, hlos, 1, msa, 1))
		goto fail;
	wr32(MSS_RMB + RMB_PMI_CODE_LENGTH, 0);
	wr32(MSS_RMB + RMB_PMI_META_DATA, (uint32_t)META_PA);
	wr32(MSS_RMB + RMB_MBA_COMMAND, CMD_META_DATA_READY);
	st = poll(MSS_RMB + RMB_MBA_STATUS, STATUS_META_AUTH, 10000);
	hyp_assign(META_PA, meta_size, msa, 1, hlos, 1);
	printf("mss: headers -> status %d\n", (int32_t)st);
	if (st != STATUS_META_AUTH)
		goto fail_mba;

	/* 3. the segments */
	if (hyp_assign(MPSS_PA, MPSS_SIZE, hlos, 1, both, 2))
		goto fail_mba;
	uint64_t len = 0, total = 0;
	for (unsigned i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || ((ph[i].flags >> 24) & 7) == 2 || !ph[i].memsz)
			continue;	/* PT_LOAD, not the hash segment, not empty: Linux segment_is_loadable */
		uint64_t pa = ph[i].paddr;
		if (pa < MPSS_PA || pa + ph[i].memsz > MPSS_PA + MPSS_SIZE) {
			printf("mss: segment %u at %llx outside modem_fw_region\n", i, (unsigned long long)pa);
			goto fail_region;
		}
		if (ph[i].filesz) {
			char path[128];
			size_t sl;
			snprintf(path, sizeof(path), FW_DIR "/modem.b%02u", i);
			uint8_t *seg = slurp(path, &sl);
			if (!seg || sl < ph[i].filesz) {
				printf("mss: %s missing or short\n", path);
				free(seg);
				goto fail_region;
			}
			pm_wr(pa, seg, (ph[i].filesz + 3) & ~3u);
			free(seg);
			total += ph[i].filesz;
		}
		uint64_t z = (ph[i].filesz + 3) & ~3ull;
		if (z < ph[i].memsz)
			zero_fill(pa + z, ph[i].memsz - z);
		/* pil_msa_mba_verify_blob */
		if (!len) {
			wr32(MSS_RMB + RMB_PMI_CODE_START, (uint32_t)pa);
			wr32(MSS_RMB + RMB_MBA_COMMAND, CMD_LOAD_READY);
		}
		len += ph[i].memsz;
		wr32(MSS_RMB + RMB_PMI_CODE_LENGTH, (uint32_t)len);
		st = rd32(MSS_RMB + RMB_MBA_STATUS);
		if ((int32_t)st < 0) {
			printf("mss: MBA error %d after segment %u\n", (int32_t)st, i);
			goto fail_region;
		}
	}
	printf("mss: %llu bytes of segments loaded, %llu announced\n", (unsigned long long)total, (unsigned long long)len);
	{
		const uint32_t src[] = { VMID_HLOS, VMID_MSS_MSA };	/* pil_reclaim_mem: the modem's alone */
		if (hyp_assign(MPSS_PA, MPSS_SIZE, src, 2, msa, 1))
			goto fail_region;
	}
	st = poll(MSS_RMB + RMB_MBA_STATUS, STATUS_AUTH_COMPLETE, 10000);
	log_rmb("auth");
	if (st != STATUS_AUTH_COMPLETE) {
		printf("mss: image authentication -> status %d\n", (int32_t)st);
		hyp_assign(MPSS_PA, MPSS_SIZE, msa, 1, hlos, 1);
		goto fail_mba;
	}
	hyp_assign(MBA_PA, MBA_SIZE, msa, 1, hlos, 1);	/* the MBA's work is done */
	printf("mss: MODEM AUTHENTICATED AND RUNNING\n");
	free(mba);
	free(mdt);
	return 0;

fail_region:
	hyp_assign(MPSS_PA, MPSS_SIZE, both, 2, hlos, 1);
fail_mba:
	wr32(MSS_RMB + RMB_MBA_COMMAND, CMD_PILFAIL_NFY_MBA);
	printf("mss: pilfail notify -> status %d\n", (int32_t)poll(MSS_RMB + RMB_MBA_STATUS, 6, 1000));
fail:
	log_rmb("failed");
	mss_shutdown();
	hyp_assign(MBA_PA, MBA_SIZE, msa, 1, hlos, 1);
	free(mba);
	free(mdt);
	return -1;
}
