/*
 * a9ygputest: drives the Adreno 512 from iOS userland through
 * SweetAccelerator's v1 user client (iokit/Kernel/arm/SweetAccelerator.cpp,
 * boot-arg sweetmtl=3 publishes it without a Metal plugin): buffers, an IB
 * with a CP_MEM_WRITE, a submit and its fence, the result read back.
 * Prints to stdout (the launchd job sends it to /dev/console) and also looks
 * every line up as an IORegistry path: the kernel's IOKit trace (boot-arg
 * sweetiolog=a9ygputest) logs it as "IOK a9ygputest: path ...", which lands
 * in XNU's on-disk console log whatever happens to stdout. Built on Linux by
 * build.sh: no SDK, the few IOKit and libSystem calls are declared here.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

typedef int kern_return_t;
typedef unsigned int mach_port_t;
typedef mach_port_t io_object_t, io_service_t, io_connect_t, task_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t;
typedef const void *CFDictionaryRef;

extern mach_port_t mach_task_self_;
extern int printf(const char *, ...);
extern int vsnprintf(char *, size_t, const char *, va_list);
extern CFDictionaryRef IOServiceMatching(const char *name);
extern io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern kern_return_t IOObjectRelease(io_object_t object);
extern io_service_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_t owning_task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOConnectCallScalarMethod(mach_port_t connection, uint32_t selector, const uint64_t *input,
    uint32_t input_cnt, uint64_t *output, uint32_t *output_cnt);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memory_type, task_t into_task,
    mach_vm_address_t *at_address, mach_vm_size_t *of_size, uint32_t options);

#define kIOMapAnywhere  1

/* one line to stdout and, as a path lookup that matches nothing, to the IOKit trace */
static void say(const char *fmt, ...)
{
	char line[200] = "IOService:/a9ygputest ";
	size_t pre = sizeof("IOService:/a9ygputest ") - 1;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + pre, sizeof(line) - pre, fmt, ap);
	va_end(ap);
	printf("%s\n", line + pre);
	io_object_t o = IORegistryEntryFromPath(0, line);
	if (o)
		IOObjectRelease(o);
}

static uint32_t pm4_parity(uint32_t v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^
				  (v >> 24) ^ (v >> 28)))) & 1;
}

static uint32_t pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | pm4_parity(cnt) << 15 | op << 16 | pm4_parity(op) << 23;
}

struct buf {
	uint64_t handle, gva, size;
	volatile uint32_t *cpu;
};

static int buf_new(io_connect_t c, uint64_t bytes, struct buf *b)
{
	uint64_t in[1] = { bytes }, out[3] = { 0 };
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallScalarMethod(c, 1, in, 1, out, &n);
	if (kr) {
		say("alloc %llu: kr %x", (unsigned long long)bytes, kr);
		return -1;
	}
	b->handle = out[0];
	b->gva = out[1];
	b->size = out[2];
	mach_vm_address_t at = 0;
	mach_vm_size_t sz = 0;
	kr = IOConnectMapMemory64(c, (uint32_t)b->handle, mach_task_self_, &at, &sz, kIOMapAnywhere);
	if (kr) {
		say("map handle %llu: kr %x", (unsigned long long)b->handle, kr);
		return -1;
	}
	b->cpu = (volatile uint32_t *)at;
	say("buffer %llu: %llu bytes, GPU 0x%llx, CPU 0x%llx", (unsigned long long)b->handle,
	       (unsigned long long)b->size, (unsigned long long)b->gva, (unsigned long long)at);
	return 0;
}

/* clean + invalidate through the kernel's mapping: the GPU does not snoop */
static void buf_flush(io_connect_t c, const struct buf *b)
{
	uint64_t in[3] = { b->handle, 0, b->size };
	uint32_t n = 0;
	IOConnectCallScalarMethod(c, 5, in, 3, NULL, &n);
}

int main(void)
{
	say("start");
	io_service_t s = IOServiceGetMatchingService(0, IOServiceMatching("SweetAccelerator"));
	if (!s) {
		say("no SweetAccelerator (boot-arg sweetmtl=3?)");
		return 1;
	}
	io_connect_t c = 0;
	kern_return_t kr = IOServiceOpen(s, mach_task_self_, 0, &c);
	if (kr) {
		say("IOServiceOpen: kr %x", kr);
		return 1;
	}
	uint64_t info[3] = { 0 };
	uint32_t n = 3;
	kr = IOConnectCallScalarMethod(c, 0, NULL, 0, info, &n);
	say("info kr %x: gpu %llu chip 0x%llx ready %llu", kr, (unsigned long long)info[0],
	       (unsigned long long)info[1], (unsigned long long)info[2]);
	if (kr || !info[2])
		return 1;

	struct buf ib, dst;
	if (buf_new(c, 4096, &ib) || buf_new(c, 4096, &dst))
		return 1;
	for (int i = 0; i < 16; i++)
		dst.cpu[i] = 0xdeadbeef;
	uint32_t w = 0;
	for (int i = 0; i < 4; i++) {			/* four words, four packets */
		uint64_t a = dst.gva + 4 * (uint64_t)i;
		ib.cpu[w++] = pkt7(0x3d, 3);		/* CP_MEM_WRITE */
		ib.cpu[w++] = (uint32_t)a;
		ib.cpu[w++] = (uint32_t)(a >> 32);
		ib.cpu[w++] = 0xa9c00000u | (uint32_t)i;
	}
	buf_flush(c, &ib);
	buf_flush(c, &dst);

	uint64_t in[3] = { ib.gva, w, 1000 }, fence = 0;
	say("submit IB 0x%llx, %u dwords", (unsigned long long)ib.gva, w);
	n = 1;
	kr = IOConnectCallScalarMethod(c, 4, in, 3, &fence, &n);
	buf_flush(c, &dst);
	int ok = 1;
	for (int i = 0; i < 4; i++)
		ok &= dst.cpu[i] == (0xa9c00000u | (uint32_t)i);
	say("submit kr %x fence %llu: %x %x %x %x, next %x: %s", kr, (unsigned long long)fence,
	       dst.cpu[0], dst.cpu[1], dst.cpu[2], dst.cpu[3], dst.cpu[4],
	       ok && !kr ? "USERLAND DROVE THE GPU" : "FAILED");
	IOServiceClose(c);
	return ok && !kr ? 0 : 1;
}
