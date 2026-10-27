/*
 * tri: draw one RGB triangle into a 64x64 RGBA8 FBO with GLES2 on surfaceless EGL.
 * Run on the host under freedreno's noop drm-shim (FD_GPU_ID=512, FD_RD_DUMP=full)
 * to capture the Adreno 512 command stream that the phone replays.
 */
#include <stdio.h>
#include <stdlib.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define W 64
#define H 64

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

	GLuint rb, fb;
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
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
	glBindAttribLocation(prog, 1, "col");
	glLinkProgram(prog);
	glUseProgram(prog);

	static const GLfloat pos[] = { -0.8f, -0.8f, 0.0f, 0.8f, 0.8f, -0.8f };
	static const GLfloat col[] = { 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1 };
	GLuint vbo[2];
	glGenBuffers(2, vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(pos), pos, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(col), col, GL_STATIC_DRAW);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(1);

	glViewport(0, 0, W, H);
	glClearColor(0.0667f, 0.1333f, 0.2f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	static unsigned char px[W * H * 4];
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("glError 0x%x, centre %02x%02x%02x%02x\n", glGetError(),
	       px[(H / 2 * W + W / 2) * 4], px[(H / 2 * W + W / 2) * 4 + 1],
	       px[(H / 2 * W + W / 2) * 4 + 2], px[(H / 2 * W + W / 2) * 4 + 3]);
	eglTerminate(dpy);
	return 0;
}
