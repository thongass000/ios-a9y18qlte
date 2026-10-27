/*
 * SPDX-License-Identifier: MIT
 *
 * libdrm stand-in for Mesa's freedreno "msm" backend on a9y18qlte (Adreno 512), over the SweetAccelerator
 * v2 user client (NativeAdreno ABI v1, xnu iokit/Kernel/arm/SweetGPUABI.h).
 *
 * The approach and the libdrm surface come from Bergamota's gpu/freedreno-sweet/sweet_drm.c (MIT,
 * github.com/ry-diffusion/Bergamota): Mesa 25.0.7's msm backend only calls drmCommandWriteRead/Write,
 * drmIoctl(GEM_CLOSE), drmGetVersion, drmGetCap and mmap(fd, GEM_INFO(GET_OFFSET)), so those are provided
 * here under the libdrm names and no Mesa source changes beyond Bergamota's Mesa patch are needed. Unlike
 * Bergamota's kernel, ours does not speak msm: it allocates buffers, maps them at a GPU address the client
 * picks inside its window, and runs lists of IBs. So the msm objects live here:
 *
 *   - GEM_NEW allocates a BO, gives it a GPU address from this connection's window and maps it there;
 *     GET_IOVA answers that address (msm 1.4: Mesa's "sp" submit path, no relocations);
 *   - GEM_SUBMIT turns the command buffers into IBs (bo address + offset, up to 16 per kernel submit) and
 *     records the fence on every BO of the submit; GEM_CPU_PREP waits for a BO's last fence;
 *   - a closed BO's GPU range is reused only after its last fence (the kernel unmaps after the fence too);
 *   - "device fds" are dups of /dev/null and all mean THE connection (one per process);
 *   - mmap of a BO is IOConnectMapMemory64 with the BO's CPU-map token (the buffers are write-combined for
 *     the CPU and uncached for the GPU, so there is no cache maintenance).
 *
 * IOSurfaces become BOs too (sweet_drm_surface_bo, for Bergamota's sweet_hw.c): the kernel pins the surface's own
 * pages (BoImport) and they get a GPU address like any BO; mmap answers the surface's base address. Those pages
 * stay cacheable for the CPU while the GPU is not coherent, so sweet_drm_bo_sync cleans and invalidates them
 * (dc civac from EL0: the kernel sets SCTLR_EL1.UCI).
 *
 * Syncobjs, PRIME and fence fds are -ENOSYS (drmGetCap reports none of them; Gallium does not need them).
 */
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "drm-uapi/drm.h"
#include "drm-uapi/msm_drm.h"

#define EXPORT __attribute__((visibility("default")))
#define DRM_IOCTL_NR(n) ((unsigned)(n) & 0xff)  /* xf86drm.h's */

/* ---- SweetGPUABI.h (NativeAdreno ABI v1): the parts used here ---- */
#define NA_SERVICE              "SweetAccelerator"
#define NA_OPEN_DIRECT          2
#define NA_BO_ALLOC             2
#define NA_BO_MAP               3
#define NA_BO_FREE              4
#define NA_SUBMIT               5
#define NA_WAIT_FENCE           6
#define NA_GET_TIMESTAMP        7
#define NA_GET_STATUS           8
#define NA_BO_CPU_MAP           13
#define NA_GET_DEVICE_INFO      15
#define NA_BO_IMPORT            16
#define NA_MAX_SUBMIT_BUFFERS   16

struct na_device_info {
	uint32_t chip_id, gmem_bytes;
	uint64_t gmem_base, uche_trap_base, va_start, va_size;
	uint32_t highest_bank_bit, ubwc_swizzle, macrotile_mode, ubwc_version, flags, reserved;
};
struct na_bo_alloc { uint64_t bytes; uint32_t align_log2, contiguous; };
struct na_bo_map { uint64_t handle, iova; uint32_t permissions, writeback, shareability, reserved; };
struct na_bo_cpu_map_reply { uint32_t token, reserved; uint64_t bytes; };
struct na_submit_buffer { uint64_t handle, iova, bytes; };
struct na_submit {
	uint32_t buffer_count, reserved;
	uint64_t queue, wait_fence, wait_timeout_ns;
	struct na_submit_buffer buffers[NA_MAX_SUBMIT_BUFFERS];
};
struct na_wait_fence { uint64_t fence, timeout_ns; };
struct na_status { uint64_t faults, suspends; };

/* ---- A512 facts the kernel does not report (kgsl adreno-gpulist.h a512, sweet_a5xx.c UCHE_GMEM_RANGE) ---- */
#define A512_GPU_ID             512
#define A512_GMEM_BYTES         (256 * 1024 + 16 * 1024)
#define A512_GMEM_BASE          0x100000
#define A512_MAX_FREQ           850000000

typedef struct _drmVersion {
	int   version_major, version_minor, version_patchlevel;
	int   name_len;
	char *name;
	int   date_len;
	char *date;
	int   desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static io_connect_t g_conn;
static dev_t g_null_rdev;
static int g_trace;
static struct na_device_info g_dev;

/* ---- BOs: msm handle = the kernel's handle (slot + 1, small) ---- */
#define MAX_BOS 65536
static struct bo {
	uint64_t size;          /* 0: free slot */
	uint64_t iova;
	uint32_t fence;         /* last submit that used it */
	uint32_t token;         /* CPU-map token (clientMemoryForType), 0 until mapped once */
	void *cpu;              /* imported IOSurface: its base address (the BO's CPU mapping) */
	IOSurfaceRef surf;      /* imported IOSurface, kept until GEM_CLOSE */
} g_bo[MAX_BOS];

/* ---- GPU address ranges of the window: free list, first fit, coalesced; closed ranges wait for a fence ---- */
#define MAX_RANGES 16384
static struct range { uint64_t start, size; } g_free[MAX_RANGES];
static unsigned g_nfree;
static struct { uint64_t start, size; uint32_t fence; } g_dead[MAX_RANGES];
static unsigned g_ndead;

static void
trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
trace(const char *fmt, ...)
{
	if (!g_trace)
		return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "a9y_drm: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static int
io_errno(kern_return_t kr)
{
	switch (kr) {
	case kIOReturnSuccess: return 0;
	case kIOReturnNoMemory: case kIOReturnNoSpace: case kIOReturnNoResources: return ENOMEM;
	case kIOReturnBadArgument: return EINVAL;
	case kIOReturnTimeout: return ETIMEDOUT;
	case kIOReturnBusy: return EBUSY;
	case kIOReturnNotReady: case kIOReturnNotFound: return ENODEV;
	case kIOReturnUnsupported: return ENOSYS;
	case kIOReturnNotPermitted: return EPERM;
	default: return EIO;
	}
}

static kern_return_t
na_call(uint32_t method, const void *in, size_t isz, void *out, size_t osz)
{
	size_t n = osz;
	kern_return_t kr = IOConnectCallStructMethod(g_conn, method, in, isz, out, out ? &n : NULL);
	if (kr != KERN_SUCCESS && method != NA_WAIT_FENCE)
		trace("method %u -> %#x", method, kr);
	return kr;
}

static int
connect_once(void)
{
	int ok;

	pthread_mutex_lock(&g_lock);
	if (g_conn == IO_OBJECT_NULL) {
		struct stat st;
		if (stat("/dev/null", &st) == 0)
			g_null_rdev = st.st_rdev;
		g_trace = getenv("A9Y_DRM_TRACE") != NULL;
		io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(NA_SERVICE));
		if (svc != IO_OBJECT_NULL) {
			if (IOServiceOpen(svc, mach_task_self(), NA_OPEN_DIRECT, &g_conn) != KERN_SUCCESS)
				g_conn = IO_OBJECT_NULL;
			IOObjectRelease(svc);
		}
		if (g_conn != IO_OBJECT_NULL &&
		    na_call(NA_GET_DEVICE_INFO, NULL, 0, &g_dev, sizeof(g_dev)) == KERN_SUCCESS && g_dev.va_size) {
			/* keep the first 64 KiB of the window unused: a GPU address of 0 never means a BO */
			g_free[0].start = g_dev.va_start + 0x10000;
			g_free[0].size = g_dev.va_size - 0x10000;
			g_nfree = 1;
			trace("window %#llx+%#llx chip %#x", (unsigned long long)g_dev.va_start,
			    (unsigned long long)g_dev.va_size, g_dev.chip_id);
		} else if (g_conn != IO_OBJECT_NULL) {
			IOServiceClose(g_conn);
			g_conn = IO_OBJECT_NULL;
		}
	}
	ok = g_conn != IO_OBJECT_NULL;
	pthread_mutex_unlock(&g_lock);
	return ok;
}

static int
is_ours(int fd)
{
	struct stat st;

	if (g_conn == IO_OBJECT_NULL || fd < 0 || fstat(fd, &st) != 0)
		return 0;
	return S_ISCHR(st.st_mode) && st.st_rdev == g_null_rdev;
}

static int
fence_done(uint32_t fence)
{
	struct na_wait_fence w = { fence, 0 };
	return fence == 0 || na_call(NA_WAIT_FENCE, &w, sizeof(w), NULL, 0) == KERN_SUCCESS;
}

/* The last failed fence wait, for Chlorophyll's fence-timeout report (sweet_drm_take_wait_error) */
static char g_wait_err[160];

/* Wait for a fence until the absolute CLOCK_MONOTONIC time t (msm timeouts are absolute, the kernel's relative). */
static int
fence_wait_until(uint32_t fence, const struct drm_msm_timespec *t)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t ns = (t->tv_sec - now.tv_sec) * 1000000000ll + (t->tv_nsec - now.tv_nsec);
	struct na_wait_fence w = { fence, ns < 0 ? 0 : (uint64_t)ns };
	if (fence == 0)
		return 0;
	kern_return_t kr = na_call(NA_WAIT_FENCE, &w, sizeof(w), NULL, 0);
	if (kr != KERN_SUCCESS) {
		struct na_status st = { 0, 0 };
		na_call(NA_GET_STATUS, NULL, 0, &st, sizeof(st));
		snprintf(g_wait_err, sizeof(g_wait_err), "WAIT_FENCE fence %u: kr %#x after a %lld ms timeout; GPU faults %llu",
		    fence, kr, (long long)(w.timeout_ns / 1000000), (unsigned long long)st.faults);
	}
	return -io_errno(kr);
}

/* Bergamota's stand-in API: the last failed fence wait into buf, then forgotten (empty: no wait failed) */
EXPORT void
sweet_drm_take_wait_error(char *buf, size_t size)
{
	if (size == 0)
		return;
	snprintf(buf, size, "%s", g_wait_err);
	g_wait_err[0] = 0;
}

/* ---- GPU address ranges (g_lock held) ---- */

static void
range_free_now(uint64_t start, uint64_t size)
{
	unsigned i = 0;
	while (i < g_nfree && g_free[i].start < start)
		i++;
	if (i > 0 && g_free[i - 1].start + g_free[i - 1].size == start) {
		g_free[i - 1].size += size;
		if (i < g_nfree && start + size == g_free[i].start) {
			g_free[i - 1].size += g_free[i].size;
			memmove(&g_free[i], &g_free[i + 1], (g_nfree - i - 1) * sizeof(g_free[0]));
			g_nfree--;
		}
	} else if (i < g_nfree && start + size == g_free[i].start) {
		g_free[i].start = start;
		g_free[i].size += size;
	} else if (g_nfree < MAX_RANGES) {
		memmove(&g_free[i + 1], &g_free[i], (g_nfree - i) * sizeof(g_free[0]));
		g_free[i].start = start;
		g_free[i].size = size;
		g_nfree++;
	}       /* else: the range is lost (16384 holes; never seen) */
}

static void
range_reap(void)
{
	for (unsigned i = 0; i < g_ndead;) {
		if (fence_done(g_dead[i].fence)) {
			range_free_now(g_dead[i].start, g_dead[i].size);
			g_dead[i] = g_dead[--g_ndead];
		} else {
			i++;
		}
	}
}

static uint64_t
range_alloc(uint64_t size, uint64_t align)
{
	for (int pass = 0; pass < 2; pass++) {
		for (unsigned i = 0; i < g_nfree; i++) {
			uint64_t s = (g_free[i].start + align - 1) & ~(align - 1), end = g_free[i].start + g_free[i].size;
			if (s + size > end)
				continue;
			uint64_t head = s - g_free[i].start, tail = end - (s + size);
			if (head && tail) {
				g_free[i].size = head;
				range_free_now(s + size, tail);
			} else if (head) {
				g_free[i].size = head;
			} else if (tail) {
				g_free[i].start = s + size;
				g_free[i].size = tail;
			} else {
				memmove(&g_free[i], &g_free[i + 1], (g_nfree - i - 1) * sizeof(g_free[0]));
				g_nfree--;
			}
			return s;
		}
		range_reap();
	}
	return 0;
}

/* ---- msm requests ---- */

/* A kernel BO (allocated or imported) gets a GPU address in the window and its slot; on failure it is freed. */
static int
bo_place(uint64_t handle, uint64_t size, void *cpu, IOSurfaceRef surf)
{
	kern_return_t kr;

	if (handle == 0 || handle >= MAX_BOS) {
		struct { uint64_t handle; } f = { handle };
		na_call(NA_BO_FREE, &f, sizeof(f), NULL, 0);
		return -ENOMEM;
	}
	pthread_mutex_lock(&g_lock);
	/* big buffers on a 64 KiB boundary (fewer page-table pages touched), the rest on 4 KiB */
	uint64_t iova = range_alloc(size, size >= 0x10000 ? 0x10000 : 0x1000);
	pthread_mutex_unlock(&g_lock);
	struct na_bo_map m = { handle, iova, 3, 0, 0, 0 };
	uint64_t got = 0;
	kr = iova ? na_call(NA_BO_MAP, &m, sizeof(m), &got, sizeof(got)) : (kern_return_t)kIOReturnNoSpace;
	if (kr != KERN_SUCCESS) {
		struct { uint64_t handle; } f = { handle };
		na_call(NA_BO_FREE, &f, sizeof(f), NULL, 0);
		if (iova) {
			pthread_mutex_lock(&g_lock);
			range_free_now(iova, size);
			pthread_mutex_unlock(&g_lock);
		}
		return -io_errno(kr);
	}
	pthread_mutex_lock(&g_lock);
	g_bo[handle] = (struct bo){ size, iova, 0, 0, cpu, surf };
	pthread_mutex_unlock(&g_lock);
	trace("%s h=%llu size=%#llx iova=%#llx", surf ? "import" : "new", (unsigned long long)handle,
	    (unsigned long long)size, (unsigned long long)iova);
	return 0;
}

static int
gem_new(struct drm_msm_gem_new *req)
{
	uint64_t size = (req->size + 4095) & ~4095ull, handle = 0;
	struct na_bo_alloc a = { size, 12, 0 };
	kern_return_t kr;

	if (size == 0)
		return -EINVAL;
	kr = na_call(NA_BO_ALLOC, &a, sizeof(a), &handle, sizeof(handle));
	if (kr != KERN_SUCCESS)
		return -io_errno(kr);
	int r = bo_place(handle, size, NULL, NULL);
	if (r == 0)
		req->handle = (uint32_t)handle;
	return r;
}

static int
gem_close(uint32_t handle)
{
	struct { uint64_t handle; } f = { handle };

	pthread_mutex_lock(&g_lock);
	if (handle == 0 || handle >= MAX_BOS || g_bo[handle].size == 0) {
		pthread_mutex_unlock(&g_lock);
		return -EINVAL;
	}
	struct bo b = g_bo[handle];
	g_bo[handle].size = 0;
	if (fence_done(b.fence))
		range_free_now(b.iova, b.size);
	else if (g_ndead < MAX_RANGES)
		g_dead[g_ndead++] = (typeof(g_dead[0])){ b.iova, b.size, b.fence };
	pthread_mutex_unlock(&g_lock);
	int r = -io_errno(na_call(NA_BO_FREE, &f, sizeof(f), NULL, 0));
	if (b.surf)
		CFRelease(b.surf);      /* the kernel unpinned its pages at BO_FREE */
	return r;
}

static struct bo *
bo_get(uint32_t handle)
{
	return handle && handle < MAX_BOS && g_bo[handle].size ? &g_bo[handle] : NULL;
}

static int
gem_info(struct drm_msm_gem_info *req)
{
	pthread_mutex_lock(&g_lock);
	struct bo *b = bo_get(req->handle);
	uint64_t iova = b ? b->iova : 0;
	pthread_mutex_unlock(&g_lock);
	if (!b)
		return -ENOENT;
	switch (req->info) {
	case MSM_INFO_GET_OFFSET:
		req->value = req->handle;       /* the mmap "offset" is the handle (sweet_drm_mmap) */
		return 0;
	case MSM_INFO_GET_IOVA:
		req->value = iova;
		return 0;
	case MSM_INFO_SET_NAME:
		return 0;
	case MSM_INFO_GET_FLAGS:
		req->value = 0;
		return 0;
	default:
		return -EINVAL;
	}
}

static int
gem_cpu_prep(struct drm_msm_gem_cpu_prep *req)
{
	pthread_mutex_lock(&g_lock);
	struct bo *b = bo_get(req->handle);
	uint32_t fence = b ? b->fence : 0;
	pthread_mutex_unlock(&g_lock);
	if (!b)
		return -ENOENT;
	if (req->op & MSM_PREP_NOSYNC)
		return fence_done(fence) ? 0 : -EBUSY;
	return fence_wait_until(fence, &req->timeout);
}

static int
gem_submit(struct drm_msm_gem_submit *req)
{
	const struct drm_msm_gem_submit_bo *bos = (const void *)(uintptr_t)req->bos;
	const struct drm_msm_gem_submit_cmd *cmds = (const void *)(uintptr_t)req->cmds;
	struct na_submit s;
	uint32_t fence = 0;

	if (req->flags & (MSM_SUBMIT_FENCE_FD_OUT | MSM_SUBMIT_SYNCOBJ_IN | MSM_SUBMIT_SYNCOBJ_OUT |
			  MSM_SUBMIT_FENCE_SN_IN))
		return -ENOSYS;
	memset(&s, 0, sizeof(s));
	/* the ring is in order, so an in fence fd (FENCE_FD_IN) needs nothing; it is never one of ours anyway */
	for (uint32_t i = 0; i <= req->nr_cmds; i++) {
		if (i < req->nr_cmds) {
			const struct drm_msm_gem_submit_cmd *c = &cmds[i];
			if (c->type != MSM_SUBMIT_CMD_BUF)
				continue;       /* IB targets are reached from the IBs; one context, nothing to restore */
			if (c->submit_idx >= req->nr_bos || c->nr_relocs)
				return -EINVAL;
			pthread_mutex_lock(&g_lock);
			struct bo *b = bo_get(bos[c->submit_idx].handle);
			int ok = b && c->submit_offset + (uint64_t)c->size <= b->size;
			struct na_submit_buffer nb = { bos[c->submit_idx].handle, ok ? b->iova + c->submit_offset : 0, c->size };
			pthread_mutex_unlock(&g_lock);
			if (!ok)
				return -EINVAL;
			s.buffers[s.buffer_count++] = nb;
		}
		if (s.buffer_count == NA_MAX_SUBMIT_BUFFERS || (i == req->nr_cmds && s.buffer_count)) {
			uint64_t f = 0;
			kern_return_t kr = na_call(NA_SUBMIT, &s, sizeof(s), &f, sizeof(f));
			if (kr != KERN_SUCCESS)
				return -io_errno(kr);
			fence = (uint32_t)f;
			s.buffer_count = 0;
		}
	}
	if (fence == 0) {
		/* no command buffer: answer the newest fence of this connection's BOs (all earlier work) */
		pthread_mutex_lock(&g_lock);
		for (uint32_t i = 0; i < req->nr_bos; i++) {
			struct bo *b = bo_get(bos[i].handle);
			if (b && (int32_t)(b->fence - fence) > 0)
				fence = b->fence;
		}
		pthread_mutex_unlock(&g_lock);
	}
	pthread_mutex_lock(&g_lock);
	for (uint32_t i = 0; i < req->nr_bos; i++) {
		struct bo *b = bo_get(bos[i].handle);
		if (b && fence)
			b->fence = fence;
	}
	pthread_mutex_unlock(&g_lock);
	req->fence = fence;
	req->fence_fd = -1;
	trace("submit cmds=%u bos=%u fence=%u", req->nr_cmds, req->nr_bos, fence);
	return 0;
}

static int
get_param(struct drm_msm_param *p)
{
	switch (p->param) {
	case MSM_PARAM_GPU_ID: p->value = A512_GPU_ID; return 0;
	case MSM_PARAM_GMEM_SIZE: p->value = A512_GMEM_BYTES; return 0;
	case MSM_PARAM_CHIP_ID: p->value = g_dev.chip_id; return 0;
	case MSM_PARAM_MAX_FREQ: p->value = A512_MAX_FREQ; return 0;
	case MSM_PARAM_GMEM_BASE: p->value = A512_GMEM_BASE; return 0;
	case MSM_PARAM_PRIORITIES: p->value = 1; return 0;
	case MSM_PARAM_PP_PGTABLE: p->value = 0; return 0;
	case MSM_PARAM_VA_START: p->value = g_dev.va_start; return 0;
	case MSM_PARAM_VA_SIZE: p->value = g_dev.va_size; return 0;
	case MSM_PARAM_TIMESTAMP: {
		uint64_t t = 0;
		kern_return_t kr = na_call(NA_GET_TIMESTAMP, NULL, 0, &t, sizeof(t));
		p->value = t;
		return -io_errno(kr);
	}
	case MSM_PARAM_FAULTS: {
		struct na_status st = { 0, 0 };
		kern_return_t kr = na_call(NA_GET_STATUS, NULL, 0, &st, sizeof(st));
		p->value = st.faults;
		return -io_errno(kr);
	}
	default:
		return -EINVAL;
	}
}

static int
msm_request(unsigned long idx, void *data)
{
	if (!connect_once())
		return -ENODEV;
	switch (idx) {
	case DRM_MSM_GET_PARAM: return get_param(data);
	case DRM_MSM_GEM_NEW: return gem_new(data);
	case DRM_MSM_GEM_INFO: return gem_info(data);
	case DRM_MSM_GEM_CPU_PREP: return gem_cpu_prep(data);
	case DRM_MSM_GEM_CPU_FINI: return 0;
	case DRM_MSM_GEM_SUBMIT: return gem_submit(data);
	case DRM_MSM_WAIT_FENCE: {
		struct drm_msm_wait_fence *w = data;
		return fence_wait_until(w->fence, &w->timeout);
	}
	case DRM_MSM_GEM_MADVISE:
		((struct drm_msm_gem_madvise *)data)->retained = 1;     /* nothing is ever purged */
		return 0;
	case DRM_MSM_SUBMITQUEUE_NEW:
		((struct drm_msm_submitqueue *)data)->id = 1;           /* one ring */
		return 0;
	case DRM_MSM_SUBMITQUEUE_CLOSE:
		return 0;
	default:
		trace("unsupported msm request %#lx", idx);
		return -ENOSYS;
	}
}

/* ---- libdrm API used by Mesa ---- */

EXPORT int
drmCommandWriteRead(int fd, unsigned long idx, void *data, unsigned long size)
{
	(void)fd; (void)size;
	return msm_request(idx, data);
}

EXPORT int
drmCommandWrite(int fd, unsigned long idx, void *data, unsigned long size)
{
	return drmCommandWriteRead(fd, idx, data, size);
}

EXPORT int
drmCommandRead(int fd, unsigned long idx, void *data, unsigned long size)
{
	return drmCommandWriteRead(fd, idx, data, size);
}

EXPORT int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int r;

	(void)fd;
	if (request == DRM_IOCTL_GEM_CLOSE)
		r = connect_once() ? gem_close(((struct drm_gem_close *)arg)->handle) : -ENODEV;
	else if (DRM_IOCTL_NR(request) >= DRM_COMMAND_BASE && DRM_IOCTL_NR(request) < DRM_COMMAND_END)
		r = msm_request(DRM_IOCTL_NR(request) - DRM_COMMAND_BASE, arg);
	else
		r = -ENOSYS;
	if (r) {
		errno = -r;
		return -1;
	}
	return 0;
}

EXPORT drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr v;

	if (!is_ours(fd) && !connect_once()) {
		errno = ENODEV;
		return NULL;
	}
	v = calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	v->version_major = 1;
	v->version_minor = 4;   /* FD_VERSION_SOFTPIN: the "sp" submit path (no relocations), see the top */
	v->name = strdup("msm");
	v->name_len = 3;
	v->date = strdup("20261027");
	v->date_len = 8;
	v->desc = strdup("a9y18qlte SweetAccelerator A512");
	v->desc_len = (int)strlen(v->desc);
	return v;
}

EXPORT void
drmFreeVersion(drmVersionPtr v)
{
	if (v) {
		free(v->name);
		free(v->date);
		free(v->desc);
		free(v);
	}
}

/* "msm" (or any name) opens the one GPU connection; the fd is a dup of /dev/null. */
EXPORT int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)name; (void)busid; (void)type;
	if (!connect_once()) {
		errno = ENODEV;
		return -1;
	}
	return open("/dev/null", O_RDWR | O_CLOEXEC);
}

EXPORT int
drmGetCap(int fd, uint64_t cap, uint64_t *value)
{
	(void)fd; (void)cap;
	*value = 0;             /* no syncobj, no prime */
	return 0;
}

EXPORT int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd; (void)prime_fd; (void)handle;
	errno = ENOSYS;
	return -1;
}

EXPORT int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd; (void)handle; (void)flags; (void)prime_fd;
	errno = ENOSYS;
	return -1;
}

EXPORT int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle) { (void)fd; (void)flags; (void)handle; return -ENOSYS; }
EXPORT int drmSyncobjDestroy(int fd, uint32_t handle) { (void)fd; (void)handle; return -ENOSYS; }
EXPORT int drmSyncobjReset(int fd, const uint32_t *h, uint32_t n) { (void)fd; (void)h; (void)n; return -ENOSYS; }
EXPORT int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle) { (void)fd; (void)obj_fd; (void)handle; return -ENOSYS; }
EXPORT int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd) { (void)fd; (void)handle; (void)sync_file_fd; return -ENOSYS; }
EXPORT int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd) { (void)fd; (void)handle; (void)sync_file_fd; return -ENOSYS; }

/* ---- device enumeration: one platform device whose render node is /dev/null ---- */

#define DRM_NODE_PRIMARY 0
#define DRM_NODE_RENDER  2
#define DRM_NODE_MAX     3
#define DRM_BUS_PLATFORM 2

typedef struct { char fullname[512]; } drmPlatformBusInfo;
typedef struct { char **compatible; } drmPlatformDeviceInfo;
typedef struct _drmDevice {
	char **nodes;
	int available_nodes;
	int bustype;
	union { drmPlatformBusInfo *platform; void *any; } businfo;
	union { drmPlatformDeviceInfo *platform; void *any; } deviceinfo;
} drmDevice, *drmDevicePtr;

static drmDevicePtr
a9y_device(void)
{
	struct blob {
		drmDevice dev;
		char *nodes[DRM_NODE_MAX];
		drmPlatformBusInfo bus;
		drmPlatformDeviceInfo info;
		char *compat[2];
	} *b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;
	b->nodes[DRM_NODE_PRIMARY] = "";
	b->nodes[1] = "";
	b->nodes[DRM_NODE_RENDER] = "/dev/null";
	b->dev.nodes = b->nodes;
	b->dev.available_nodes = 1 << DRM_NODE_RENDER;
	b->dev.bustype = DRM_BUS_PLATFORM;
	snprintf(b->bus.fullname, sizeof(b->bus.fullname), "/soc/qcom,kgsl-3d0@5000000");
	b->compat[0] = "qcom,adreno-512.0";
	b->info.compatible = b->compat;
	b->dev.businfo.platform = &b->bus;
	b->dev.deviceinfo.platform = &b->info;
	return &b->dev;
}

EXPORT int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	if (!connect_once())
		return 0;
	if (devices == NULL)
		return 1;
	if (max_devices < 1)
		return 0;
	devices[0] = a9y_device();
	return devices[0] ? 1 : -ENOMEM;
}

EXPORT int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)flags;
	if (!is_ours(fd) && !connect_once())
		return -ENODEV;
	*device = a9y_device();
	return *device ? 0 : -ENOMEM;
}

EXPORT void
drmFreeDevice(drmDevicePtr *device)
{
	if (device && *device) {
		free(*device);
		*device = NULL;
	}
}

EXPORT void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; devices && i < count; i++)
		drmFreeDevice(&devices[i]);
}

/* ---- mmap of a BO: offset = msm handle ---- */

static struct {
	void    *addr;
	uint32_t token;
} g_maps[16384];
static unsigned g_nmaps;

static void *
a9y_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	mach_vm_address_t a = 0;
	mach_vm_size_t s = 0;
	uint32_t token = 0;

	if (!is_ours(fd))
		return mmap(addr, len, prot, flags, fd, off);
	void *cpu = NULL;
	pthread_mutex_lock(&g_lock);
	struct bo *b = bo_get((uint32_t)off);
	if (b) {
		token = b->token;
		cpu = b->cpu;
	}
	if (cpu && g_nmaps < sizeof(g_maps) / sizeof(g_maps[0])) {
		/* an imported IOSurface is already mapped: its own memory (token 0 = never unmapped here) */
		g_maps[g_nmaps].addr = cpu;
		g_maps[g_nmaps].token = 0;
		g_nmaps++;
	}
	pthread_mutex_unlock(&g_lock);
	if (cpu)
		return cpu;
	if (!b) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	if (token == 0) {
		uint64_t h = (uint64_t)off;
		struct na_bo_cpu_map_reply r;
		kern_return_t kr = na_call(NA_BO_CPU_MAP, &h, sizeof(h), &r, sizeof(r));
		if (kr != KERN_SUCCESS) {
			errno = io_errno(kr);
			return MAP_FAILED;
		}
		token = r.token;
		pthread_mutex_lock(&g_lock);
		if ((b = bo_get((uint32_t)off)))
			b->token = token;
		pthread_mutex_unlock(&g_lock);
	}
	kern_return_t kr = IOConnectMapMemory64(g_conn, token, mach_task_self(), &a, &s, kIOMapAnywhere);
	if (kr != KERN_SUCCESS || s < len) {
		if (kr == KERN_SUCCESS)
			IOConnectUnmapMemory64(g_conn, token, mach_task_self(), a);
		errno = kr == KERN_SUCCESS ? EINVAL : io_errno(kr);
		return MAP_FAILED;
	}
	pthread_mutex_lock(&g_lock);
	if (g_nmaps < sizeof(g_maps) / sizeof(g_maps[0])) {
		g_maps[g_nmaps].addr = (void *)(uintptr_t)a;
		g_maps[g_nmaps].token = token;
		g_nmaps++;
	}
	pthread_mutex_unlock(&g_lock);
	return (void *)(uintptr_t)a;
}

static int
a9y_munmap(void *addr, size_t len)
{
	int found = 0;
	uint32_t token = 0;

	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < g_nmaps; i++) {
		if (g_maps[i].addr == addr) {
			token = g_maps[i].token;
			g_maps[i] = g_maps[--g_nmaps];
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&g_lock);
	if (found && token == 0)
		return 0;       /* an IOSurface's own memory */
	if (found)
		return IOConnectUnmapMemory64(g_conn, token, mach_task_self(), (mach_vm_address_t)(uintptr_t)addr) == KERN_SUCCESS ? 0 : -1;
	return munmap(addr, len);
}

/* ---- entry points of Bergamota's Mesa patch (freedreno_bo.c, targets/osmesa/sweet_hw.c) ---- */

EXPORT void *
sweet_drm_mmap(int fd, size_t len, uint64_t off)
{
	return a9y_mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);
}

EXPORT int
sweet_drm_munmap(void *addr, size_t len)
{
	return a9y_munmap(addr, len);
}

/* sweet_hw.c's struct sweet_gpu_info: abi_version, flags, gpu_id, chip_id, va_start, va_size, last_fence, abi_minor */
struct sweet_gpu_info_abi {
	uint32_t abi_version, flags, gpu_id, chip_id;
	uint64_t va_start, va_size;
	uint32_t last_fence, abi_minor;
};

EXPORT int
sweet_drm_info(int fd, struct sweet_gpu_info_abi *out)
{
	(void)fd;
	if (!connect_once())
		return -ENODEV;
	memset(out, 0, sizeof(*out));
	out->abi_version = 1;
	out->gpu_id = A512_GPU_ID;
	out->chip_id = g_dev.chip_id;
	out->va_start = g_dev.va_start;
	out->va_size = g_dev.va_size;
	return 0;               /* abi_minor 0: no IOSurface import */
}

/* sweet_hw.c's struct sweet_gpu_surface_bo (Bergamota xnu/gpu-abi/sweet_adreno_abi.h, minor 2 layout) */
struct sweet_gpu_surface_bo_abi {
	uint32_t surface_id, handle;
	uint64_t size, iova;
	uint32_t offset, bytes_per_row, width, height, pixel_format, planes;
	uint32_t flags, tiling, ubwc, reserved;
	struct { uint32_t offset, stride, width, height; } plane[3];
	uint64_t alias_addr, alias_size;
};

/* IOSurface `surface_id` as a BO: the kernel pins its pages, they get a GPU address; handle 0xffffffff only describes */
EXPORT int
sweet_drm_surface_bo(int fd, struct sweet_gpu_surface_bo_abi *io)
{
	(void)fd;
	if (!connect_once())
		return -ENODEV;
	IOSurfaceRef s = IOSurfaceLookup(io->surface_id);
	if (!s)
		return -ENOENT;
	uintptr_t base = (uintptr_t)IOSurfaceGetBaseAddress(s);
	size_t alloc = IOSurfaceGetAllocSize(s);
	uint64_t bytes = (alloc + 4095) & ~4095ull;
	io->offset = 0;
	io->bytes_per_row = (uint32_t)IOSurfaceGetBytesPerRow(s);
	io->width = (uint32_t)IOSurfaceGetWidth(s);
	io->height = (uint32_t)IOSurfaceGetHeight(s);
	io->pixel_format = IOSurfaceGetPixelFormat(s);
	io->planes = (uint32_t)IOSurfaceGetPlaneCount(s);
	io->flags = io->tiling = io->ubwc = 0;
	io->size = bytes;
	if (io->handle == 0xffffffffu) {
		CFRelease(s);
		return 0;
	}
	if ((base & 4095) || bytes == 0) {
		trace("surface %u: base %#lx not page aligned", io->surface_id, (unsigned long)base);
		CFRelease(s);
		return -EINVAL;
	}
	struct { uint64_t address, bytes; } imp = { base, bytes };
	uint64_t handle = 0;
	kern_return_t kr = na_call(NA_BO_IMPORT, &imp, sizeof(imp), &handle, sizeof(handle));
	if (kr != KERN_SUCCESS) {
		CFRelease(s);
		return -io_errno(kr);
	}
	int r = bo_place(handle, bytes, (void *)base, s);       /* keeps the reference on success */
	if (r) {
		CFRelease(s);
		return r;
	}
	pthread_mutex_lock(&g_lock);
	io->iova = g_bo[handle].iova;
	pthread_mutex_unlock(&g_lock);
	io->handle = (uint32_t)handle;
	trace("surface %u: %ux%u bpr %u fmt %#x -> h=%u iova=%#llx", io->surface_id, io->width, io->height,
	    io->bytes_per_row, io->pixel_format, io->handle, (unsigned long long)io->iova);
	return 0;
}

EXPORT int
sweet_drm_gem_close(int fd, uint32_t handle)
{
	(void)fd;
	return connect_once() ? gem_close(handle) : -ENODEV;
}

/* Allocated BOs are write-combined for the CPU: nothing to do. An imported IOSurface is cacheable: clean and
 * invalidate [offset, offset + size) (all of it for size 0), before the GPU reads it and after the GPU wrote it. */
EXPORT int
sweet_drm_bo_sync(int fd, uint32_t handle, uint64_t offset, uint64_t size)
{
	(void)fd;
	pthread_mutex_lock(&g_lock);
	struct bo *b = bo_get(handle);
	uintptr_t cpu = b ? (uintptr_t)b->cpu : 0;
	uint64_t bytes = b ? b->size : 0;
	pthread_mutex_unlock(&g_lock);
	if (!b)
		return -ENOENT;
	if (!cpu)
		return 0;
	if (offset >= bytes)
		return 0;
	if (size == 0 || offset + size > bytes)
		size = bytes - offset;
	const uintptr_t line = 64;      /* Cortex-A53/A73 D-cache line (CTR_EL0 may trap: UCT is not known set) */
	for (uintptr_t a = (cpu + offset) & ~(line - 1); a < cpu + offset + size; a += line)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" : : : "memory");
	return 0;
}

struct interpose_tuple { const void *replacement, *replacee; };
__attribute__((used)) static const struct interpose_tuple interpose_mmap
    __attribute__((section("__DATA,__interpose"))) = { (const void *)a9y_mmap, (const void *)mmap };
__attribute__((used)) static const struct interpose_tuple interpose_munmap
    __attribute__((section("__DATA,__interpose"))) = { (const void *)a9y_munmap, (const void *)munmap };
