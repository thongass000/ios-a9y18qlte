/*
 * osmtri: Mesa's GLES driver on the Adreno 512, on the phone. The same scene as glreplay/host/tri.c (a clear and
 * one RGB triangle into a 64x64 RGBA8 FBO), but compiled and submitted on the device: OSMesa (Bergamota's ES
 * profile and hardware target) -> Gallium freedreno fd5 + ir3 -> a9y_drm.c -> SweetAccelerator v2.
 *
 * Installed as /usr/local/bin/a9ygputest so the boot-time launchd job runs it and sweetiolog=a9ygputest routes
 * its lines to SWEETLOG; Mesa's own stderr is forwarded the same way.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <GL/osmesa.h>
#include <GLES2/gl2.h>

#define OSMESA_ES_PROFILE 0x36  /* bergamota-mesa-25.0.7.patch */
#define W 64
#define H 64

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
	char line[300] = "IOService:/a9ygputest ";
	size_t pre = sizeof("IOService:/a9ygputest ") - 1;
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

/* one character per 2x4 pixel cell, as glreplay's a9yreplay: '.' clear colour, R/G/B strongest channel, '?' other */
static void show(const unsigned char *px)
{
	for (int y = H - 4; y >= 0; y -= 4) {
		char row[W / 2 + 1];
		for (int x = 0; x < W; x += 2) {
			const unsigned char *p = px + (y * W + x) * 4;
			char ch = '?';
			if (p[0] == 0x11 && p[1] == 0x22 && p[2] == 0x33)
				ch = '.';
			else if (p[3] == 0xff)
				ch = p[0] >= p[1] && p[0] >= p[2] ? 'R' : p[1] >= p[2] ? 'G' : 'B';
			row[x / 2] = ch;
		}
		row[W / 2] = 0;
		say("rt %2d |%s|", y, row);
	}
}

int main(void)
{
	pthread_t pump;
	say("=== a9yosmtri: Mesa GLES on the Adreno 512, compiled on the phone ===");
	if (pipe(err_pipe) == 0 && dup2(err_pipe[1], 2) >= 0) {
		setvbuf(stderr, NULL, _IOLBF, 0);
		pthread_create(&pump, NULL, err_pump, NULL);
	}
	setenv("A9Y_DRM_TRACE", "1", 0);
	setenv("SWEET_GPU_DEBUG", "1", 0);
	setenv("FD_MESA_DEBUG", "sysmem", 0);   /* the path the replay proved; GMEM tiling next */

	static unsigned char fb[W * H * 4];
	static const int attr[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 0, OSMESA_STENCIL_BITS, 0,
		OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, 2, OSMESA_CONTEXT_MINOR_VERSION, 0, 0 };
	OSMesaContext ctx = OSMesaCreateContextAttribs(attr, NULL);
	if (!ctx || !OSMesaMakeCurrent(ctx, fb, GL_UNSIGNED_BYTE, W, H)) {
		say("FAIL: OSMesa context %p", (void *)ctx);
		sleep(1);
		return 1;
	}
	if (load_gl()) {
		sleep(1);
		return 1;
	}
	const char *renderer = (const char *)p_glGetString(GL_RENDERER);
	say("GL_RENDERER %s, GL_VERSION %s", renderer, (const char *)p_glGetString(GL_VERSION));

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

	p_glViewport(0, 0, W, H);
	p_glClearColor(0.0667f, 0.1333f, 0.2f, 1.0f);
	p_glClear(GL_COLOR_BUFFER_BIT);
	p_glDrawArrays(GL_TRIANGLES, 0, 3);

	static unsigned char px[W * H * 4];
	memset(px, 0xde, sizeof(px));
	p_glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	p_glFinish();
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
	say("fbo status %#x, glError %#x; %u clear, %u untouched, %u drawn; centre %02x%02x%02x%02x",
	    st, err, clear, untouched, drawn, c[0], c[1], c[2], c[3]);
	show(px);
	int hw = renderer && strstr(renderer, "FD512") != NULL;
	int ok = hw && err == 0 && untouched == 0 && clear > 0 && drawn > 0;
	say("=== a9yosmtri: %s ===", ok ? "PASS: MESA GLES DREW ON THE ADRENO 512, ON THE PHONE" :
	    hw ? "FAIL" : "FAIL (not on the GPU: softpipe?)");
	OSMesaDestroyContext(ctx);
	fflush(stderr);
	sleep(1);       /* let the stderr pump finish */
	return ok ? 0 : 1;
}
