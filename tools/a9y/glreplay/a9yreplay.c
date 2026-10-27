/*
 * a9yreplay: replay one Mesa freedreno (fd5) submit on the Adreno 512 through the SweetAccelerator v2 user client.
 *
 * capture.h comes from a host run of Mesa's GLES driver as FD512 under the noop drm-shim (tri.c: clear + one RGB
 * triangle into a 64x64 RGBA8 FBO), dumped with FD_RD_DUMP=full and turned into C by rd2c.py. The shim hands out
 * GPU addresses inside the first v2 window (0x4_0000_0000 on a9y18qlte), so every buffer is mapped at its captured address and
 * the command stream runs unchanged. Afterwards the render target is read back: clear colour outside, the
 * interpolated triangle inside.
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
extern kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *input,
    size_t input_size, void *output, size_t *output_size);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memory_type, task_t into_task,
    mach_vm_address_t *at_address, mach_vm_size_t *of_size, uint32_t options);

#define kIOMapAnywhere  1

#include "capture.h"

#define NBUF (sizeof(cap_bufs) / sizeof(cap_bufs[0]))

/* SweetGPUABI.h (NativeAdreno ABI v1), the parts used here */
#define V2_OPEN_DIRECT          2
#define M_BO_ALLOC              2
#define M_BO_MAP                3
#define M_SUBMIT                5
#define M_WAIT_FENCE            6
#define M_GET_STATUS            8
#define M_BO_CPU_MAP            13
#define M_GET_DEVICE_INFO       15
#define MAX_SUBMIT_BUFFERS      16

struct dev_info {
	uint32_t chip_id, gmem_bytes;
	uint64_t gmem_base, uche_trap_base, va_start, va_size;
	uint32_t highest_bank_bit, ubwc_swizzle, macrotile_mode, ubwc_version, flags, reserved;
};
struct bo_alloc { uint64_t bytes; uint32_t align_log2, contiguous; };
struct bo_map { uint64_t handle, iova; uint32_t permissions, writeback, shareability, reserved; };
struct bo_cpu_map_reply { uint32_t token, reserved; uint64_t bytes; };
struct submit_buffer { uint64_t handle, iova, bytes; };
struct submit {
	uint32_t buffer_count, reserved;
	uint64_t queue, wait_fence, wait_timeout_ns;
	struct submit_buffer buffers[MAX_SUBMIT_BUFFERS];
};
struct wait_fence { uint64_t fence, timeout_ns; };
struct status { uint64_t faults, suspends; };

static void say(const char *fmt, ...)
{
	char line[256] = "IOService:/a9ygputest ";
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

static kern_return_t call(io_connect_t c, uint32_t method, const void *in, size_t isz, void *out, size_t osz)
{
	size_t n = osz;
	return IOConnectCallStructMethod(c, method, in, isz, out, out ? &n : NULL);
}

/* one character per 2x4 pixel cell: '.' clear colour, 'R'/'G'/'B' by the strongest channel, '?' anything else */
static void show(const volatile uint8_t *rt)
{
	for (int y = CAP_RT_H - 4; y >= 0; y -= 4) {
		char row[CAP_RT_W / 2 + 1];
		for (int x = 0; x < CAP_RT_W; x += 2) {
			const volatile uint8_t *p = rt + y * CAP_RT_PITCH + x * 4;
			char ch = '?';
			if (p[0] == 0x11 && p[1] == 0x22 && p[2] == 0x33)
				ch = '.';
			else if (p[3] == 0xff)
				ch = p[0] >= p[1] && p[0] >= p[2] ? 'R' : p[1] >= p[2] ? 'G' : 'B';
			row[x / 2] = ch;
		}
		row[CAP_RT_W / 2] = 0;
		say("rt %2d |%s|", y, row);
	}
}

int main(void)
{
	say("=== a9yreplay: Mesa fd5 triangle on the Adreno 512 ===");
	io_service_t s = IOServiceGetMatchingService(0, IOServiceMatching("SweetAccelerator"));
	if (!s) {
		say("FAIL: no SweetAccelerator");
		return 1;
	}
	io_connect_t c = 0;
	kern_return_t kr = IOServiceOpen(s, mach_task_self_, V2_OPEN_DIRECT, &c);
	if (kr) {
		say("FAIL: v2 open: kr 0x%x", kr);
		return 1;
	}

	struct dev_info di = { 0 };
	kr = call(c, M_GET_DEVICE_INFO, NULL, 0, &di, sizeof(di));
	say("device: kr 0x%x chip 0x%x va 0x%llx+0x%llx", kr, di.chip_id, (unsigned long long)di.va_start,
	    (unsigned long long)di.va_size);
	if (kr || CAP_IB_IOVA < di.va_start || CAP_IB_IOVA >= di.va_start + di.va_size) {
		say("FAIL: capture addresses are outside this client's GPU window (another v2 client open?)");
		IOServiceClose(c);
		return 1;
	}

	uint64_t handle[NBUF];
	volatile uint32_t *cpu[NBUF];
	for (unsigned b = 0; b < NBUF; b++) {
		struct bo_alloc ba = { cap_bufs[b].bytes, 12, 0 };
		kr = call(c, M_BO_ALLOC, &ba, sizeof(ba), &handle[b], sizeof(handle[b]));
		if (kr) {
			say("FAIL: alloc buffer %u (0x%x bytes): kr 0x%x", b, cap_bufs[b].bytes, kr);
			return 1;
		}
		struct bo_map bm = { handle[b], cap_bufs[b].iova, 3, 0, 0, 0 };
		uint64_t iova = 0;
		kr = call(c, M_BO_MAP, &bm, sizeof(bm), &iova, sizeof(iova));
		if (kr || iova != cap_bufs[b].iova) {
			say("FAIL: map buffer %u at 0x%llx: kr 0x%x", b, (unsigned long long)cap_bufs[b].iova, kr);
			return 1;
		}
		struct bo_cpu_map_reply cm = { 0 };
		kr = call(c, M_BO_CPU_MAP, &handle[b], sizeof(handle[b]), &cm, sizeof(cm));
		mach_vm_address_t at = 0;
		mach_vm_size_t sz = 0;
		if (!kr)
			kr = IOConnectMapMemory64(c, cm.token, mach_task_self_, &at, &sz, kIOMapAnywhere);
		if (kr) {
			say("FAIL: CPU map buffer %u: kr 0x%x", b, kr);
			return 1;
		}
		cpu[b] = (volatile uint32_t *)at;
		/* fresh pages are not promised to be zero: clear, then lay the captured runs over */
		for (uint32_t i = 0; i < cap_bufs[b].bytes / 4; i++)
			cpu[b][i] = 0;
		for (uint32_t r = cap_bufs[b].run; r < cap_bufs[b].run + cap_bufs[b].nruns; r++)
			for (uint32_t i = 0; i < cap_runs[r].count; i++)
				cpu[b][cap_runs[r].dword + i] = cap_data[cap_runs[r].data + i];
	}
	say("%u buffers mapped at their captured addresses", (unsigned)NBUF);

	unsigned ib = NBUF, rt = NBUF;
	for (unsigned b = 0; b < NBUF; b++) {
		if (cap_bufs[b].iova == CAP_IB_IOVA)
			ib = b;
		if (cap_bufs[b].iova == CAP_RT_IOVA)
			rt = b;
	}
	if (ib == NBUF || rt == NBUF) {
		say("FAIL: capture has no IB or render target buffer");
		return 1;
	}
	/* a canary in the render target: whatever the GPU leaves there is its own work */
	for (uint32_t i = 0; i < CAP_RT_PITCH * CAP_RT_H / 4; i++)
		cpu[rt][i] = 0xdeadbeef;

	struct status st0 = { 0 }, st1 = { 0 };
	call(c, M_GET_STATUS, NULL, 0, &st0, sizeof(st0));
	struct submit sb = { 0 };
	sb.buffer_count = 1;
	sb.buffers[0] = (struct submit_buffer){ handle[ib], CAP_IB_IOVA, CAP_IB_DWORDS * 4 };
	uint64_t fence = 0;
	kr = call(c, M_SUBMIT, &sb, sizeof(sb), &fence, sizeof(fence));
	say("submit: kr 0x%x fence %llu", kr, (unsigned long long)fence);
	if (kr)
		return 1;
	struct wait_fence wf = { fence, 2000000000ull };
	kr = call(c, M_WAIT_FENCE, &wf, sizeof(wf), NULL, 0);
	call(c, M_GET_STATUS, NULL, 0, &st1, sizeof(st1));
	say("wait: kr 0x%x, GPU faults %llu -> %llu", kr, (unsigned long long)st0.faults, (unsigned long long)st1.faults);

	const volatile uint8_t *px = (const volatile uint8_t *)cpu[rt];
	unsigned clear = 0, canary = 0, other = 0;
	for (int y = 0; y < CAP_RT_H; y++)
		for (int x = 0; x < CAP_RT_W; x++) {
			uint32_t v = *(const volatile uint32_t *)(px + y * CAP_RT_PITCH + x * 4);
			if (v == 0xff332211)
				clear++;
			else if (v == 0xdeadbeef)
				canary++;
			else
				other++;
		}
	uint32_t centre = *(const volatile uint32_t *)(px + CAP_RT_H / 2 * CAP_RT_PITCH + CAP_RT_W / 2 * 4);
	say("rt: %u clear, %u untouched, %u drawn; centre 0x%08x", clear, canary, other, centre);
	show(px);

	int ok = kr == 0 && st1.faults == st0.faults && canary == 0 && clear > 0 && other > 0;
	say("=== a9yreplay: %s ===", ok ? "PASS: THE ADRENO 512 DREW MESA'S TRIANGLE" : "FAIL");
	IOServiceClose(c);
	return ok ? 0 : 1;
}
