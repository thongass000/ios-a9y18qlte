/*
 * Userland side of /dev/pmem (kernel: work/dxb/xnu/bsd/dev/sweet_pmem.c).
 * Offsets are physical addresses. MMIO and carve-outs: 32-bit aligned
 * accesses only. Managed DRAM (pm_alloc buffers): any size, kept coherent
 * with devices by the kernel (cache clean+invalidate around each access).
 */
#ifndef PMEM_H
#define PMEM_H
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

struct pmem_alloc {
	uint64_t size;
	uint64_t align;
	uint64_t pa;
};
struct pmem_smc {
	uint64_t x[8];
};
#define PMEM_LIST_MAX	64
struct pmem_list {
	uint32_t count;
	uint32_t pad;
	struct {
		uint64_t pa, size;
	} ent[PMEM_LIST_MAX];
};
#define PMEM_ALLOC	_IOWR('P', 1, struct pmem_alloc)
#define PMEM_FREE	_IOW('P', 2, uint64_t)
#define PMEM_SMC	_IOWR('P', 3, struct pmem_smc)
#define PMEM_LIST	_IOR('P', 4, struct pmem_list)
struct pmem_rpmh {
	char res[16];
	uint32_t n;
	uint32_t offset[4];
	uint32_t value[4];
};
#define PMEM_RPMH	_IOW('P', 5, struct pmem_rpmh)
/* sdm660's RPM (a9y18qlte kernels): one {key, value} for a resource, set 0 active / 1 sleep */
struct pmem_rpm {
	uint32_t set, type, id, key, value;
};
#define PMEM_RPM	_IOW('P', 6, struct pmem_rpm)
#define RPM_WORD(s)	((uint32_t)(s)[0] | (uint32_t)(s)[1] << 8 | (uint32_t)(s)[2] << 16 | (uint32_t)(s)[3] << 24)

void pm_init(void);			/* exits if /dev/pmem is missing */
uint32_t rd32(uint64_t pa);
void wr32(uint64_t pa, uint32_t v);
void wr64(uint64_t pa, uint64_t v);	/* two 32-bit stores, low first */
int pm_rd(uint64_t pa, void *buf, size_t len);
int pm_wr(uint64_t pa, const void *buf, size_t len);
uint64_t pm_alloc(uint64_t size, uint64_t align);	/* 0 on failure */
int pm_free(uint64_t pa);
void pm_smc(uint64_t x[8]);		/* x0..x7 in, x0..x3 out */
int pm_list(struct pmem_list *l);
int pm_rpmh(const char *res, unsigned n, const uint32_t *off, const uint32_t *val);	/* errno or 0 */
int pm_rpm(uint32_t set, const char type[4], uint32_t id, const char key[4], uint32_t value);	/* errno or 0 */
void delay_ms(unsigned ms);
int wait_bit(uint64_t pa, uint32_t mask, int set, unsigned ms);	/* 0 = ok */

#endif
