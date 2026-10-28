/*
 * fit: the panel present as one GLES2 draw: a 1125x2436 RGBA8 texture (iOS's swapped frame) scaled, bilinear, into
 * the 1026x2220 centre of a 1080x2220 RGBA8 render target (the panel, x = 27). Bytes pass through unswizzled, so the
 * BGRA frame lands BGRA on the panel. No upload and no clear: a single submit for rd2tmpl.py to template (the
 * kernel then points the texture at the frame and the target at the panel).
 * Run on the host under freedreno's noop drm-shim (FD_GPU_ID=512, FD_RD_DUMP=full, FD_MESA_DEBUG=sysmem,notile).
 */
#include <stdio.h>
#include <stdlib.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define SRC_W 1125
#define SRC_H 2436
#define DST_W 1080
#define DST_H 2220
#define FIT_X 27
#define FIT_W 1026

static const char *vs_src =
	"attribute vec4 pos;\n"
	"attribute vec2 uv;\n"
	"varying vec2 v_uv;\n"
	"void main() { gl_Position = pos; v_uv = uv; }\n";

static const char *fs_src =
	"precision mediump float;\n"
	"uniform sampler2D tex;\n"
	"varying vec2 v_uv;\n"
	"void main() { gl_FragColor = texture2D(tex, v_uv); }\n";

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return s;
}

int main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if (!eglInitialize(dpy, NULL, NULL)) {
		fprintf(stderr, "eglInitialize failed\n");
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
	if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "context failed\n");
		return 1;
	}
	printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SRC_W, SRC_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GLuint rb, fb;
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, DST_W, DST_H);
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "fbo incomplete\n");
		return 1;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
	glBindAttribLocation(prog, 0, "pos");
	glBindAttribLocation(prog, 1, "uv");
	glLinkProgram(prog);
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "tex"), 0);

	/* a strip over the whole viewport; the frame's rows go top-down in memory as the target's do */
	static const GLfloat quad[] = {
		-1, -1, 0, 0,
		 1, -1, 1, 0,
		-1,  1, 0, 1,
		 1,  1, 1, 1,
	};
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
	glEnableVertexAttribArray(1);

	glDisable(GL_DITHER);
	glViewport(FIT_X, 0, FIT_W, DST_H);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	printf("glError 0x%x\n", glGetError());
	eglTerminate(dpy);
	return 0;
}
