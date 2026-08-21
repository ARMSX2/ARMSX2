#include "gl_compat.h"

/*
 * Undo the redirection for this file.
 *
 * gl_compat.h rewrites glEnable/glDisable/glDrawArrays and friends into the fx_* entry points
 * so the Flurry sources need no edits -- but this file IMPLEMENTS those entry points, and it
 * has to be able to call the real GL underneath them. Without these undefs, fx_enable's own
 * glEnable(cap) expands to fx_enable(cap) and recurses forever. At -O2 that is a tail call and
 * becomes an infinite LOOP rather than a stack overflow, so it does not crash: the thread just
 * spins at 100% and nothing after it ever runs. The first glDisable in GLSetupRC was enough to
 * hang the whole renderer with no error anywhere.
 */
#undef glEnable
#undef glDisable
#undef glDrawArrays
#undef glVertexPointer
#undef glColorPointer
#undef glTexCoordPointer
#undef glEnableClientState
#undef glDisableClientState
#undef glColor4f
#undef glColor3f
#undef glRectd
#undef glMatrixMode
#undef glLoadIdentity
#undef glOrtho
#undef glAlphaFunc
#undef glShadeModel
#undef glTexEnvf
#undef glDrawBuffer
#undef glFinish

#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Flurry", __VA_ARGS__)

/* Flurry draws at most NUMSMOKEPARTICLES quads in one call. Kept here rather than including
 * flurry.h so this file stays independent of it; the assert in fx_draw_arrays catches it if
 * the particle count ever grows past what the index buffer covers. */
#define FX_MAX_QUADS 4096

static const char *kVert =
    "attribute vec2 aPos;\n"
    "attribute vec4 aColor;\n"
    "attribute vec2 aTex;\n"
    "uniform mat4 uProj;\n"
    "varying vec4 vColor;\n"
    "varying vec2 vTex;\n"
    "void main() {\n"
    "  vColor = aColor;\n"
    "  vTex = aTex;\n"
    "  gl_Position = uProj * vec4(aPos, 0.0, 1.0);\n"
    "}\n";

/* mediump is deliberate: the smoke is additive and low contrast, highp costs real time on
 * mobile fragment hardware, and this runs behind a home screen. */
static const char *kFrag =
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "uniform int uUseTex;\n"
    "varying vec4 vColor;\n"
    "varying vec2 vTex;\n"
    "void main() {\n"
    "  vec4 c = vColor;\n"
    "  if (uUseTex == 1) c *= texture2D(uTex, vTex);\n"
    "  gl_FragColor = c;\n"
    "}\n";

typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    const void *ptr;
    int enabled;
} fx_array;

static struct {
    GLuint program;
    GLuint ibo;
    GLint aPos, aColor, aTex;
    GLint uProj, uTex, uUseTex;
    GLfloat proj[16];
    fx_array vertex, color, texcoord;
    GLfloat current_color[4];
    int ready;
} fx;

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    if (!sh) return 0;

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

int fx_init(void)
{
    if (fx.ready) return 1;

    memset(&fx, 0, sizeof(fx));

    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) return 0;

    fx.program = glCreateProgram();
    glAttachShader(fx.program, vs);
    glAttachShader(fx.program, fs);
    glLinkProgram(fx.program);

    GLint ok = 0;
    glGetProgramiv(fx.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(fx.program, sizeof(log), NULL, log);
        LOGE("program link failed: %s", log);
        return 0;
    }

    /* Attached shaders are refcounted by the program and can go now. */
    glDeleteShader(vs);
    glDeleteShader(fs);

    fx.aPos    = glGetAttribLocation(fx.program, "aPos");
    fx.aColor  = glGetAttribLocation(fx.program, "aColor");
    fx.aTex    = glGetAttribLocation(fx.program, "aTex");
    fx.uProj   = glGetUniformLocation(fx.program, "uProj");
    fx.uTex    = glGetUniformLocation(fx.program, "uTex");
    fx.uUseTex = glGetUniformLocation(fx.program, "uUseTex");

    /* One static index buffer for every quad draw. GL_QUADS is (0,1,2,3) per quad in draw
     * order, so each becomes two triangles sharing the 0-2 diagonal. Built once because the
     * pattern never depends on the data, only on how many quads are drawn. */
    GLushort *idx = (GLushort *) malloc(sizeof(GLushort) * FX_MAX_QUADS * 6);
    if (!idx) return 0;

    for (int q = 0; q < FX_MAX_QUADS; q++) {
        const GLushort base = (GLushort) (q * 4);
        GLushort *o = idx + q * 6;
        o[0] = base; o[1] = (GLushort)(base + 1); o[2] = (GLushort)(base + 2);
        o[3] = base; o[4] = (GLushort)(base + 2); o[5] = (GLushort)(base + 3);
    }

    glGenBuffers(1, &fx.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, fx.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * FX_MAX_QUADS * 6, idx, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    free(idx);

    fx.ready = 1;
    return 1;
}

void fx_shutdown(void)
{
    if (fx.program) glDeleteProgram(fx.program);
    if (fx.ibo) glDeleteBuffers(1, &fx.ibo);
    memset(&fx, 0, sizeof(fx));
}

void fx_lost(void)
{
    /* The context is already gone; deleting these names would be undefined. */
    memset(&fx, 0, sizeof(fx));
}

void fx_ortho(float left, float right, float bottom, float top)
{
    /* Column-major, near/far fixed at -1/1: Flurry is entirely 2D. */
    const float rl = right - left, tb = top - bottom;
    memset(fx.proj, 0, sizeof(fx.proj));
    fx.proj[0]  =  2.0f / rl;
    fx.proj[5]  =  2.0f / tb;
    fx.proj[10] = -1.0f;
    fx.proj[12] = -(right + left) / rl;
    fx.proj[13] = -(top + bottom) / tb;
    fx.proj[15] =  1.0f;
}

static void set_array(fx_array *a, GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->ptr = ptr;
}

void fx_vertex_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr)   { set_array(&fx.vertex, size, type, stride, ptr); }
void fx_color_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr)    { set_array(&fx.color, size, type, stride, ptr); }
void fx_texcoord_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr) { set_array(&fx.texcoord, size, type, stride, ptr); }

void fx_enable_client_state(GLenum cap)
{
    if (cap == GL_VERTEX_ARRAY) fx.vertex.enabled = 1;
    else if (cap == GL_COLOR_ARRAY) fx.color.enabled = 1;
    else if (cap == GL_TEXTURE_COORD_ARRAY) fx.texcoord.enabled = 1;
}

void fx_disable_client_state(GLenum cap)
{
    if (cap == GL_VERTEX_ARRAY) fx.vertex.enabled = 0;
    else if (cap == GL_COLOR_ARRAY) fx.color.enabled = 0;
    else if (cap == GL_TEXTURE_COORD_ARRAY) fx.texcoord.enabled = 0;
}

void fx_enable(GLenum cap)
{
    /* GLES2 has no alpha test or lighting; passing either would set GL_INVALID_ENUM and the
     * error would then be blamed on whatever ran next. */
    /* GL_TEXTURE_2D as a capability is gone in GLES2 -- whether a sampler is used is decided by
     * the shader. Passing it sets GL_INVALID_ENUM, and the sticky error then gets blamed on
     * whatever ran next. */
    if (cap == GL_ALPHA_TEST || cap == GL_LIGHTING || cap == GL_TEXTURE_2D_ENABLE_COMPAT) return;
    glEnable(cap);
}

void fx_disable(GLenum cap)
{
    if (cap == GL_ALPHA_TEST || cap == GL_LIGHTING || cap == GL_TEXTURE_2D_ENABLE_COMPAT) return;
    glDisable(cap);
}

static void bind(GLint loc, const fx_array *a)
{
    if (loc < 0) return;

    if (!a->enabled || !a->ptr) {
        glDisableVertexAttribArray((GLuint) loc);
        return;
    }

    glEnableVertexAttribArray((GLuint) loc);
    glVertexAttribPointer((GLuint) loc, a->size, a->type, GL_FALSE, a->stride, a->ptr);
}

void fx_color4f(float r, float g, float b, float a)
{
    fx.current_color[0] = r;
    fx.current_color[1] = g;
    fx.current_color[2] = b;
    fx.current_color[3] = a;
}

void fx_rect(float x0, float y0, float x1, float y1)
{
    if (!fx.ready) return;

    const GLfloat verts[8] = { x0, y0,  x1, y0,  x1, y1,  x0, y1 };

    glUseProgram(fx.program);
    glUniformMatrix4fv(fx.uProj, 1, GL_FALSE, fx.proj);
    glUniform1i(fx.uUseTex, 0);

    if (fx.aColor >= 0) {
        glDisableVertexAttribArray((GLuint) fx.aColor);
        glVertexAttrib4fv((GLuint) fx.aColor, fx.current_color);
    }
    if (fx.aTex >= 0) glDisableVertexAttribArray((GLuint) fx.aTex);

    if (fx.aPos >= 0) {
        glEnableVertexAttribArray((GLuint) fx.aPos);
        glVertexAttribPointer((GLuint) fx.aPos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, fx.ibo);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    /* The arrays the smoke set up are still recorded; the next fx_draw_arrays rebinds them. */
}

void fx_draw_arrays(GLenum mode, GLint first, GLsizei count)
{

    if (!fx.ready || count <= 0) return;

    glUseProgram(fx.program);
    glUniformMatrix4fv(fx.uProj, 1, GL_FALSE, fx.proj);
    glUniform1i(fx.uTex, 0);
    glUniform1i(fx.uUseTex, fx.texcoord.enabled ? 1 : 0);

    bind(fx.aPos, &fx.vertex);
    bind(fx.aColor, &fx.color);
    bind(fx.aTex, &fx.texcoord);

    /* An attribute the shader declares but no array feeds still needs a value, or the draw
     * reads whatever the last one left behind. */
    if (fx.aColor >= 0 && (!fx.color.enabled || !fx.color.ptr))
        glVertexAttrib4f((GLuint) fx.aColor, 1.0f, 1.0f, 1.0f, 1.0f);

    if (mode != GL_QUADS) {
        glDrawArrays(mode, first, count);
        return;
    }

    /* GL_QUADS, the only mode Flurry actually uses. `first` is always 0 upstream; honouring a
     * non-zero one would mean an index buffer per offset, so it is rejected loudly instead of
     * being silently drawn wrong. */
    if (first != 0) {
        LOGE("fx_draw_arrays: GL_QUADS with first=%d is not supported", first);
        return;
    }

    GLsizei quads = count / 4;
    if (quads > FX_MAX_QUADS) {
        LOGE("fx_draw_arrays: %d quads exceeds the %d the index buffer covers; clamping",
             quads, FX_MAX_QUADS);
        quads = FX_MAX_QUADS;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, fx.ibo);
    glDrawElements(GL_TRIANGLES, quads * 6, GL_UNSIGNED_SHORT, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}
