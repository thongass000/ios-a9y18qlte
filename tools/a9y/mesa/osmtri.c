/*
 * osmtri: Mesa's GLES driver on the Adreno 512, on the phone. The scene of glreplay/host/tri.c (a clear and one
 * RGB triangle into an RGBA8 FBO), compiled and submitted on the device: OSMesa (Bergamota's ES profile and
 * hardware target) -> Gallium freedreno fd5 + ir3 -> a9y_drm.c -> SweetAccelerator v2.
 *
 * 512x512 (1 MiB) is about four times the A512's 272 KiB of GMEM, so the default GMEM path renders it in several
 * tiles; A9Y_SYSMEM=1 renders straight to memory instead (FD_MESA_DEBUG=sysmem, the path glreplay proved).
 *
 * Installed as /usr/local/bin/a9ytest (tools/a9y/drop: a9ydrop runs it at boot) and logged through sweetiolog;
 * Mesa's own stderr is forwarded the same way.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <spawn.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <GL/osmesa.h>
#include <GLES2/gl2.h>

#define OSMESA_ES_PROFILE 0x36  /* bergamota-mesa-25.0.7.patch */
#define W 512
#define H 512

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
	char line[300] = "IOService:/a9ytest ";
	size_t pre = sizeof("IOService:/a9ytest ") - 1;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + pre, sizeof(line) - pre, fmt, ap);
	va_end(ap);
	printf("%s\n", line + pre);
	io_object_t o = IORegistryEntryFromPath(kIOMainPortDefault, line);
	if (o)
		IOObjectRelease(o);
}

/* Mesa and the stand-in write to stderr: each line becomes a say() line */
static int err_pipe[2];

static void *err_pump(void *arg)
{
	char buf[1024], line[260];
	size_t n = 0;
	(void)arg;
	for (;;) {
		ssize_t r = read(err_pipe[0], buf, sizeof(buf));
		if (r <= 0)
			return NULL;
		for (ssize_t i = 0; i < r; i++) {
			if (buf[i] == '\n' || n == sizeof(line) - 1) {
				line[n] = 0;
				say("stderr: %s", line);
				n = 0;
			}
			if (buf[i] != '\n')
				line[n++] = buf[i];
		}
	}
}

#define GL_FN(ret, name, args) static ret (*p_##name) args
GL_FN(const GLubyte *, glGetString, (GLenum));
GL_FN(GLenum, glGetError, (void));
GL_FN(GLuint, glCreateShader, (GLenum));
GL_FN(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *));
GL_FN(void, glCompileShader, (GLuint));
GL_FN(void, glGetShaderiv, (GLuint, GLenum, GLint *));
GL_FN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *));
GL_FN(GLuint, glCreateProgram, (void));
GL_FN(void, glAttachShader, (GLuint, GLuint));
GL_FN(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *));
GL_FN(void, glLinkProgram, (GLuint));
GL_FN(void, glUseProgram, (GLuint));
GL_FN(void, glGenRenderbuffers, (GLsizei, GLuint *));
GL_FN(void, glBindRenderbuffer, (GLenum, GLuint));
GL_FN(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei));
GL_FN(void, glGenFramebuffers, (GLsizei, GLuint *));
GL_FN(void, glBindFramebuffer, (GLenum, GLuint));
GL_FN(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint));
GL_FN(GLenum, glCheckFramebufferStatus, (GLenum));
GL_FN(void, glGenBuffers, (GLsizei, GLuint *));
GL_FN(void, glBindBuffer, (GLenum, GLuint));
GL_FN(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum));
GL_FN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));
GL_FN(void, glEnableVertexAttribArray, (GLuint));
GL_FN(void, glViewport, (GLint, GLint, GLsizei, GLsizei));
GL_FN(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat));
GL_FN(void, glClear, (GLbitfield));
GL_FN(void, glDrawArrays, (GLenum, GLint, GLsizei));
GL_FN(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *));
GL_FN(void, glFinish, (void));

static int load_gl(void)
{
	int missing = 0;
#define LOAD(name) do { p_##name = (typeof(p_##name))OSMesaGetProcAddress(#name); \
	if (!p_##name) { say("FAIL: no %s", #name); missing++; } } while (0)
	LOAD(glGetString); LOAD(glGetError); LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader);
	LOAD(glGetShaderiv); LOAD(glGetShaderInfoLog); LOAD(glCreateProgram); LOAD(glAttachShader);
	LOAD(glBindAttribLocation); LOAD(glLinkProgram); LOAD(glUseProgram); LOAD(glGenRenderbuffers);
	LOAD(glBindRenderbuffer); LOAD(glRenderbufferStorage); LOAD(glGenFramebuffers); LOAD(glBindFramebuffer);
	LOAD(glFramebufferRenderbuffer); LOAD(glCheckFramebufferStatus); LOAD(glGenBuffers); LOAD(glBindBuffer);
	LOAD(glBufferData); LOAD(glVertexAttribPointer); LOAD(glEnableVertexAttribArray); LOAD(glViewport);
	LOAD(glClearColor); LOAD(glClear); LOAD(glDrawArrays); LOAD(glReadPixels); LOAD(glFinish);
#undef LOAD
	return missing;
}

static const char *vs_src =
	"attribute vec4 pos;\n"
	"attribute vec4 col;\n"
	"varying vec4 v_col;\n"
	"void main() { gl_Position = pos; v_col = col; }\n";

static const char *fs_src =
	"precision mediump float;\n"
	"varying vec4 v_col;\n"
	"void main() { gl_FragColor = v_col; }\n";

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = p_glCreateShader(type);
	GLint ok = 0;
	p_glShaderSource(s, 1, &src, NULL);
	p_glCompileShader(s);
	p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[256];
		p_glGetShaderInfoLog(s, sizeof(log), NULL, log);
		say("shader: %s", log);
	}
	return s;
}

/* 32 columns x 16 rows, one character per cell: '.' clear colour, R/G/B strongest channel, '?' other */
static void show(const char *mode, const unsigned char *px)
{
	for (int y = H - H / 32; y >= 0; y -= H / 16) {
		char row[33];
		for (int c = 0; c < 32; c++) {
			const unsigned char *p = px + (y * W + c * (W / 32) + W / 64) * 4;
			char ch = '?';
			if (p[0] == 0x11 && p[1] == 0x22 && p[2] == 0x33)
				ch = '.';
			else if (p[3] == 0xff)
				ch = p[0] >= p[1] && p[0] >= p[2] ? 'R' : p[1] >= p[2] ? 'G' : 'B';
			row[c] = ch;
		}
		row[32] = 0;
		say("%s %3d |%s|", mode, y, row);
	}
}

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/*
 * The GPU into an IOSurface: a 512x512 BGRA surface, filled with a marker by the CPU, gets the renderbuffer by
 * OSMesaSweetBlit (Bergamota's GPU blit; a9y_drm.c imports the surface's pages as a BO), then the CPU reads it
 * and compares it with glReadPixels' picture, in either row order. 1 when every pixel matches.
 */
static int g_surface_ok;

static int surface_test(GLuint rb, const unsigned char *px)
{
	int (*blit)(GLuint, int, uint32_t, int, int) =
		(int (*)(GLuint, int, uint32_t, int, int))dlsym(RTLD_DEFAULT, "OSMesaSweetBlit");
	int w = W, h = H, bpe = 4, bpr = W * 4;
	uint32_t fmt = 'BGRA';
	const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow,
			       kIOSurfacePixelFormat, kIOSurfaceIsGlobal };
	const void *vals[] = { CFNumberCreate(NULL, kCFNumberIntType, &w), CFNumberCreate(NULL, kCFNumberIntType, &h),
			       CFNumberCreate(NULL, kCFNumberIntType, &bpe), CFNumberCreate(NULL, kCFNumberIntType, &bpr),
			       CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt), kCFBooleanTrue };
	CFDictionaryRef props = CFDictionaryCreate(NULL, keys, vals, 6, &kCFTypeDictionaryKeyCallBacks,
						   &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef s = IOSurfaceCreate(props);
	if (!blit || !s) {
		say("surface: FAIL: %s", !blit ? "no OSMesaSweetBlit" : "IOSurfaceCreate failed");
		return 0;
	}
	uint32_t id = IOSurfaceGetID(s);
	unsigned stride = (unsigned)IOSurfaceGetBytesPerRow(s);
	IOSurfaceLock(s, 0, NULL);
	unsigned char *base = IOSurfaceGetBaseAddress(s);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			memcpy(base + y * stride + x * 4, "\xef\xbe\xad\xde", 4);
	IOSurfaceUnlock(s, 0, NULL);

	double t0 = now_ms();
	int r = blit(rb, 0, id, 0, 1);
	p_glFinish();
	double t1 = now_ms();

	IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
	unsigned same = 0, flipped = 0, marker = 0;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			const unsigned char *q = base + y * stride + x * 4;     /* B G R A */
			const unsigned char *a = px + (y * W + x) * 4, *b = px + ((H - 1 - y) * W + x) * 4;
			same += q[0] == a[2] && q[1] == a[1] && q[2] == a[0];
			flipped += q[0] == b[2] && q[1] == b[1] && q[2] == b[0];
			marker += !memcmp(q, "\xef\xbe\xad\xde", 4);
		}
	}
	const unsigned char *c = base + H / 2 * stride + W / 2 * 4;
	IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
	say("surface %u (stride %u): blit %d in %.1f ms; %u px match in GL row order, %u flipped, %u still marker; "
	    "centre BGRA %02x%02x%02x%02x", id, stride, r, t1 - t0, same, flipped, marker, c[0], c[1], c[2], c[3]);
	CFRelease(s);
	CFRelease(props);
	return r && (same == W * H || flipped == W * H);
}

/* the scene into px (RGBA, bottom row first); 1 when it was drawn on the GPU with no error and no pixel left over */
static int render(const char *mode, unsigned char *px)
{
	static unsigned char fb[W * H * 4];
	static const int attr[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 0, OSMESA_STENCIL_BITS, 0,
		OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, 2, OSMESA_CONTEXT_MINOR_VERSION, 0, 0 };
	OSMesaContext ctx = OSMesaCreateContextAttribs(attr, NULL);
	if (!ctx || !OSMesaMakeCurrent(ctx, fb, GL_UNSIGNED_BYTE, W, H)) {
		say("%s: FAIL: OSMesa context %p", mode, (void *)ctx);
		return 0;
	}
	if (load_gl())
		return 0;
	const char *renderer = (const char *)p_glGetString(GL_RENDERER);
	say("%s: GL_RENDERER %s, GL_VERSION %s", mode, renderer, (const char *)p_glGetString(GL_VERSION));

	GLuint rb, fbo;
	p_glGenRenderbuffers(1, &rb);
	p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
	p_glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */, W, H);
	p_glGenFramebuffers(1, &fbo);
	p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);

	GLuint prog = p_glCreateProgram();
	p_glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
	p_glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
	p_glBindAttribLocation(prog, 0, "pos");
	p_glBindAttribLocation(prog, 1, "col");
	p_glLinkProgram(prog);
	p_glUseProgram(prog);

	static const GLfloat pos[] = { -0.8f, -0.8f, 0.0f, 0.8f, 0.8f, -0.8f };
	static const GLfloat col[] = { 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1 };
	GLuint vbo[2];
	p_glGenBuffers(2, vbo);
	p_glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
	p_glBufferData(GL_ARRAY_BUFFER, sizeof(pos), pos, GL_STATIC_DRAW);
	p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	p_glEnableVertexAttribArray(0);
	p_glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
	p_glBufferData(GL_ARRAY_BUFFER, sizeof(col), col, GL_STATIC_DRAW);
	p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0);
	p_glEnableVertexAttribArray(1);

	double t0 = now_ms();
	p_glViewport(0, 0, W, H);
	p_glClearColor(0.0667f, 0.1333f, 0.2f, 1.0f);
	p_glClear(GL_COLOR_BUFFER_BIT);
	p_glDrawArrays(GL_TRIANGLES, 0, 3);

	memset(px, 0xde, W * H * 4);
	p_glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	p_glFinish();
	double t1 = now_ms();
	GLenum err = p_glGetError();

	unsigned clear = 0, untouched = 0, drawn = 0;
	for (int i = 0; i < W * H; i++) {
		const unsigned char *p = px + i * 4;
		if (p[0] == 0x11 && p[1] == 0x22 && p[2] == 0x33)
			clear++;
		else if (p[0] == 0xde && p[1] == 0xde && p[2] == 0xde && p[3] == 0xde)
			untouched++;
		else
			drawn++;
	}
	const unsigned char *c = px + (H / 2 * W + W / 2) * 4;
	say("%s: fbo status %#x, glError %#x; %u clear, %u untouched, %u drawn; centre %02x%02x%02x%02x; "
	    "clear+draw+readback %.1f ms", mode, st, err, clear, untouched, drawn, c[0], c[1], c[2], c[3], t1 - t0);
	show(mode, px);
	if (!strcmp(mode, "gmem"))
		g_surface_ok = surface_test(rb, px);
	int hw = renderer && strstr(renderer, "FD512") != NULL;
	OSMesaDestroyContext(ctx);
	if (!hw)
		say("%s: not on the GPU (softpipe?)", mode);
	return hw && err == 0 && untouched == 0 && clear > 0 && drawn > 0;
}

#define REF_FILE "/usr/local/.a9ytest-sysmem.rgba"
extern char **environ;

int main(int argc, char **argv)
{
	static unsigned char px[W * H * 4], ref[W * H * 4];

	if (argc > 1 && !strcmp(argv[1], "--sysmem")) {
		/* the reference, in its own process (FD_MESA_DEBUG is read once per process); stderr is the parent's pipe.
		 * A spawned process has launchd's default memory limit again: raise it as a9ydrop does (cmd 7, not fatal) */
		int (*msc)(uint32_t, int32_t, uint32_t, void *, size_t) =
			(int (*)(uint32_t, int32_t, uint32_t, void *, size_t))dlsym(RTLD_DEFAULT, "memorystatus_control");
		struct { int32_t active; uint32_t active_attr; int32_t inactive; uint32_t inactive_attr; } lim = { 1024, 0, 1024, 0 };
		if (!msc || msc(7, getpid(), 0, &lim, sizeof(lim)))
			say("sysmem: could not raise the memory limit");
		int ok = render("sysmem", ref);
		FILE *f = fopen(REF_FILE, "wb");
		ok = ok && f && fwrite(ref, 1, sizeof(ref), f) == sizeof(ref);
		if (f)
			fclose(f);
		fflush(stderr);
		return ok ? 0 : 1;
	}

	pthread_t pump;
	say("=== a9yosmtri: Mesa GLES on the Adreno 512, %dx%d, GMEM vs sysmem ===", W, H);
	if (pipe(err_pipe) == 0 && dup2(err_pipe[1], 2) >= 0) {
		setvbuf(stderr, NULL, _IOLBF, 0);
		pthread_create(&pump, NULL, err_pump, NULL);
	}
	setenv("SWEET_GPU_DEBUG", "1", 0);

	/* a fresh process of this binary (posix_spawn: no fork without exec, CoreFoundation forbids it) */
	int ref_ok = 0, status = 0;
	pid_t pid;
	char *child_argv[] = { argv[0], "--sysmem", NULL };
	unlink(REF_FILE);
	setenv("FD_MESA_DEBUG", "sysmem", 1);
	int sr = posix_spawn(&pid, argv[0], NULL, NULL, child_argv, environ);
	unsetenv("FD_MESA_DEBUG");
	if (sr == 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		FILE *f = fopen(REF_FILE, "rb");
		ref_ok = f && fread(ref, 1, sizeof(ref), f) == sizeof(ref);
		if (f)
			fclose(f);
	}
	if (!ref_ok)
		say("no sysmem reference (spawn %d, status %#x)", sr, status);

	int ok = render("gmem", px);
	unsigned diff = 0, first = 0;
	for (unsigned i = 0; ref_ok && i < W * H; i++) {
		if (memcmp(px + i * 4, ref + i * 4, 4)) {
			if (!diff++)
				first = i;
		}
	}
	if (ref_ok)
		say("gmem vs sysmem: %u of %u pixels differ%s", diff, W * H, diff ? "" : " (identical)");
	if (diff)
		say("first difference at x %u y %u: gmem %02x%02x%02x%02x sysmem %02x%02x%02x%02x", first % W, first / W,
		    px[first * 4], px[first * 4 + 1], px[first * 4 + 2], px[first * 4 + 3],
		    ref[first * 4], ref[first * 4 + 1], ref[first * 4 + 2], ref[first * 4 + 3]);
	int pass = ok && ref_ok && diff == 0 && g_surface_ok;
	say("=== a9yosmtri: %s ===", pass ? "PASS: GMEM MATCHES SYSMEM, AND THE GPU WROTE IT INTO AN IOSURFACE" :
	    !ref_ok ? "FAIL (sysmem reference)" : !ok ? "FAIL (gmem render)" : diff ? "FAIL (gmem differs from sysmem)" :
	    "FAIL (IOSurface blit)");
	fflush(stderr);
	sleep(1);       /* let the stderr pump finish */
	return pass ? 0 : 1;
}
