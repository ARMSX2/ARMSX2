/* GL 1.x on GLES2 for the Really Slick Screensavers. See gl1.h. */

#include "gl1.h"

/* Every redirected name, undefined before the implementations below.
 *
 * Without this, gl1_translatef's own call to glTranslatef expands to gl1_translatef and the
 * function calls itself. At -O2 the tail call turns it into a silent infinite loop rather than
 * a stack overflow, so it presents as a hang with no backtrace. This exact bug cost most of a
 * session on the Flurry shim; do not remove this block. */
#undef glMatrixMode
#undef glLoadIdentity
#undef glPushMatrix
#undef glPopMatrix
#undef glLoadMatrixf
#undef glMultMatrixf
#undef glTranslatef
#undef glScalef
#undef glRotatef
#undef glOrtho
#undef glFrustum
#undef gluPerspective
#undef gluLookAt
#undef glBegin
#undef glEnd
#undef glVertex2f
#undef glVertex3f
#undef glVertex3fv
#undef glTexCoord2f
#undef glColor3f
#undef glColor4f
#undef glColor3fv
#undef glColor4fv
#undef glNormal3f
#undef glNormal3fv
#undef glTexCoord2fv
#undef gluProject
#undef glGetFloatv
#undef glGetDoublev
#undef glEnable
#undef glDisable
#undef glPointSize
#undef glGenLists
#undef glNewList
#undef glEndList
#undef glCallList
#undef glDeleteLists
#undef gluNewQuadric
#undef gluDeleteQuadric
#undef gluSphere
#undef glTexImage2D
#undef gluBuild2DMipmaps
#undef glVertexPointer
#undef glNormalPointer
#undef glColorPointer
#undef glTexCoordPointer
#undef glEnableClientState
#undef glDisableClientState
#undef glInterleavedArrays
#undef glDrawArrays
#undef glDrawElements
#undef glMultiDrawElements
#undef glTexSubImage2D
#undef glPixelStorei
#undef gluOrtho2D

#include <android/log.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Savers", __VA_ARGS__)

#define STACK_DEPTH 32
#define GL1_MAX_LISTS 16

typedef struct {
    float pos[3];
    float color[4];
    float tex[2];
    float normal[3];
} vertex;

/* A client-side (or VBO-relative) array, as set by glVertexPointer and friends. */
typedef struct {
    GLint      size;
    GLenum     type;
    GLsizei    stride;
    const void *ptr;
    int        enabled;
} client_array;

static struct {
    int ready;

    GLuint program;
    GLint  aPos, aColor, aTex, aNormal;
    GLint  uMVP, uMV, uTex, uUseTex, uPointSize, uTexGen;

    client_array ca_vertex, ca_normal, ca_color, ca_texcoord;

    /* Column-major, as GL wants them. [0] is the live matrix for each mode. */
    float  modelview[STACK_DEPTH][16];
    float  projection[STACK_DEPTH][16];
    int    mv_top, proj_top;
    GLenum mode;

    /* Immediate mode. */
    int      in_begin;
    GLenum   prim;
    vertex  *verts;
    int      count, capacity;
    GLushort *indices;
    int      index_capacity;

    /* Display lists. Names are small integers the savers hard-code (list 1), so a flat table
     * indexed by name is enough. */
    struct {
        struct { GLenum prim; vertex *verts; int count; } *batches;
        int n_batches;
        int used;
    } lists[GL1_MAX_LISTS];
    int recording;      /* list being compiled, 0 = none */

    float current_color[4];
    float current_tex[2];
    float current_normal[3];
    int   texgen;           /* GL_SPHERE_MAP emulation, for Helios's reflective surface */
    float point_size;
    int   tex_enabled;
    GLint unpack_row_length;
    int   clear_frames;
    int   stat_batches;
    long  stat_vertices;
} g;

/* ---- matrix helpers (column-major: m[col * 4 + row]) ---- */

static void m_identity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* out = a * b. Safe when out aliases a or b. */
static void m_mul(float *out, const float *a, const float *b)
{
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i < 4; i++) {
            r[c * 4 + i] = a[i] * b[c * 4] + a[4 + i] * b[c * 4 + 1] +
                           a[8 + i] * b[c * 4 + 2] + a[12 + i] * b[c * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

static float *current(void)
{
    return g.mode == GL_PROJECTION ? g.projection[g.proj_top] : g.modelview[g.mv_top];
}

static void apply(const float *m)
{
    m_mul(current(), current(), m);
}

/* ---- lifetime ---- */

static void free_list(int i);

static const char *VS =
    "attribute vec3 aPos;\n"
    "attribute vec4 aColor;\n"
    "attribute vec2 aTex;\n"
    "attribute vec3 aNormal;\n"
    "uniform mat4 uMVP;\n"
    "uniform mat4 uMV;\n"
    "uniform int uTexGen;\n"
    "uniform float uPointSize;\n"
    "varying vec4 vColor;\n"
    "varying vec2 vTex;\n"
    "void main() {\n"
    "  vColor = aColor;\n"
    /* GL_SPHERE_MAP, done here rather than on the CPU so that immediate mode and vertex
     * arrays behave identically -- the implicit surface only ever supplies normals through
     * an array. mat3 is spelled out: constructing one from a mat4 is not portable in
     * GLSL ES 1.00. */
    "  if (uTexGen == 1) {\n"
    "    mat3 nm = mat3(uMV[0].xyz, uMV[1].xyz, uMV[2].xyz);\n"
    "    vec3 ep = normalize((uMV * vec4(aPos, 1.0)).xyz);\n"
    "    vec3 en = normalize(nm * aNormal);\n"
    "    vec3 r = ep - 2.0 * en * dot(en, ep);\n"
    "    float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));\n"
    "    vTex = vec2(r.x / m + 0.5, r.y / m + 0.5);\n"
    "  } else {\n"
    "    vTex = aTex;\n"
    "  }\n"
    "  gl_PointSize = uPointSize;\n"
    "  gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "}\n";

static const char *FS =
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

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOGE("gl1 shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

int gl1_init(void)
{
    if (g.ready) return 1;
    memset(&g, 0, sizeof(g));

    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) return 0;

    g.program = glCreateProgram();
    glAttachShader(g.program, vs);
    glAttachShader(g.program, fs);
    glLinkProgram(g.program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(g.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(g.program, sizeof(log), NULL, log);
        LOGE("gl1 link failed: %s", log);
        return 0;
    }

    g.aPos       = glGetAttribLocation(g.program, "aPos");
    g.aColor     = glGetAttribLocation(g.program, "aColor");
    g.aTex       = glGetAttribLocation(g.program, "aTex");
    g.aNormal    = glGetAttribLocation(g.program, "aNormal");
    g.uMVP       = glGetUniformLocation(g.program, "uMVP");
    g.uMV        = glGetUniformLocation(g.program, "uMV");
    g.uTexGen    = glGetUniformLocation(g.program, "uTexGen");
    g.uTex       = glGetUniformLocation(g.program, "uTex");
    g.uUseTex    = glGetUniformLocation(g.program, "uUseTex");
    g.uPointSize = glGetUniformLocation(g.program, "uPointSize");

    /* A -1 here is silent -- glUniform on a dead location is a no-op and the shader keeps
     * whatever the last draw left. Worth saying out loud rather than debugging later. */
    if (g.aPos < 0 || g.uMVP < 0)
        LOGE("gl1: essential locations missing (aPos=%d uMVP=%d)", g.aPos, g.uMVP);

    /* Enough frames to have visited whatever buffers the driver rotates through. */
    g.clear_frames = 8;

    m_identity(g.modelview[0]);
    m_identity(g.projection[0]);
    g.mode = GL_MODELVIEW;
    g.current_color[0] = g.current_color[1] = g.current_color[2] = g.current_color[3] = 1.0f;
    g.point_size = 1.0f;
    g.ready = 1;
    return 1;
}

void gl1_shutdown(void)
{
    for (int i = 1; i < GL1_MAX_LISTS; i++) free_list(i);
    if (g.program) glDeleteProgram(g.program);
    free(g.verts);
    free(g.indices);
    memset(&g, 0, sizeof(g));
}

void gl1_lost(void)
{
    for (int i = 1; i < GL1_MAX_LISTS; i++) free_list(i);
    free(g.verts);
    free(g.indices);
    memset(&g, 0, sizeof(g));
}

/* ---- matrix stack ---- */

void gl1_matrix_mode(GLenum mode) { g.mode = mode; }
void gl1_load_identity(void)      { m_identity(current()); }
void gl1_load_matrixf(const float *m) { memcpy(current(), m, 16 * sizeof(float)); }
void gl1_mult_matrixf(const float *m) { apply(m); }

void gl1_push_matrix(void)
{
    if (g.mode == GL_PROJECTION) {
        if (g.proj_top + 1 >= STACK_DEPTH) { LOGE("gl1: projection stack overflow"); return; }
        memcpy(g.projection[g.proj_top + 1], g.projection[g.proj_top], 16 * sizeof(float));
        g.proj_top++;
    } else {
        if (g.mv_top + 1 >= STACK_DEPTH) { LOGE("gl1: modelview stack overflow"); return; }
        memcpy(g.modelview[g.mv_top + 1], g.modelview[g.mv_top], 16 * sizeof(float));
        g.mv_top++;
    }
}

void gl1_pop_matrix(void)
{
    if (g.mode == GL_PROJECTION) {
        if (g.proj_top > 0) g.proj_top--; else LOGE("gl1: projection stack underflow");
    } else {
        if (g.mv_top > 0) g.mv_top--; else LOGE("gl1: modelview stack underflow");
    }
}

void gl1_translatef(float x, float y, float z)
{
    float m[16];
    m_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
    apply(m);
}

void gl1_scalef(float x, float y, float z)
{
    float m[16];
    m_identity(m);
    m[0] = x; m[5] = y; m[10] = z;
    apply(m);
}

void gl1_rotatef(float angle, float x, float y, float z)
{
    const float len = sqrtf(x * x + y * y + z * z);
    if (len == 0.0f) return;
    x /= len; y /= len; z /= len;

    const float rad = angle * (float) M_PI / 180.0f;
    const float c = cosf(rad), s = sinf(rad), ic = 1.0f - c;
    float m[16];
    m_identity(m);
    m[0] = x * x * ic + c;     m[1] = y * x * ic + z * s; m[2]  = x * z * ic - y * s;
    m[4] = x * y * ic - z * s; m[5] = y * y * ic + c;     m[6]  = y * z * ic + x * s;
    m[8] = x * z * ic + y * s; m[9] = y * z * ic - x * s; m[10] = z * z * ic + c;
    apply(m);
}

void gl1_ortho(double l, double r, double b, double t, double n, double f)
{
    float m[16];
    m_identity(m);
    m[0]  =  2.0f / (float) (r - l);
    m[5]  =  2.0f / (float) (t - b);
    m[10] = -2.0f / (float) (f - n);
    m[12] = -(float) ((r + l) / (r - l));
    m[13] = -(float) ((t + b) / (t - b));
    m[14] = -(float) ((f + n) / (f - n));
    apply(m);
}

void gl1_frustum(double l, double r, double b, double t, double n, double f)
{
    float m[16];
    memset(m, 0, sizeof(m));
    m[0]  = (float) (2.0 * n / (r - l));
    m[5]  = (float) (2.0 * n / (t - b));
    m[8]  = (float) ((r + l) / (r - l));
    m[9]  = (float) ((t + b) / (t - b));
    m[10] = (float) (-(f + n) / (f - n));
    m[11] = -1.0f;
    m[14] = (float) (-2.0 * f * n / (f - n));
    apply(m);
}

void gl1_perspective(double fovy, double aspect, double zn, double zf)
{
    const double h = zn * tan(fovy * M_PI / 360.0);
    gl1_frustum(-h * aspect, h * aspect, -h, h, zn, zf);
}

void gl1_look_at(double ex, double ey, double ez, double cx, double cy, double cz,
                 double ux, double uy, double uz)
{
    double fx = cx - ex, fy = cy - ey, fz = cz - ez;
    double fl = sqrt(fx * fx + fy * fy + fz * fz);
    if (fl == 0.0) return;
    fx /= fl; fy /= fl; fz /= fl;

    double ul = sqrt(ux * ux + uy * uy + uz * uz);
    if (ul == 0.0) return;
    ux /= ul; uy /= ul; uz /= ul;

    double sx = fy * uz - fz * uy, sy = fz * ux - fx * uz, sz = fx * uy - fy * ux;
    const double sl = sqrt(sx * sx + sy * sy + sz * sz);
    if (sl == 0.0) return;
    sx /= sl; sy /= sl; sz /= sl;

    const double tx = sy * fz - sz * fy, ty = sz * fx - sx * fz, tz = sx * fy - sy * fx;

    float m[16];
    m_identity(m);
    m[0] = (float) sx;  m[4] = (float) sy;  m[8]  = (float) sz;
    m[1] = (float) tx;  m[5] = (float) ty;  m[9]  = (float) tz;
    m[2] = (float) -fx; m[6] = (float) -fy; m[10] = (float) -fz;
    apply(m);
    gl1_translatef((float) -ex, (float) -ey, (float) -ez);
}

/* ---- immediate mode ---- */

static int reserve(int n)
{
    if (g.count + n <= g.capacity) return 1;
    int want = g.capacity ? g.capacity * 2 : 256;
    while (want < g.count + n) want *= 2;
    vertex *p = (vertex *) realloc(g.verts, (size_t) want * sizeof(vertex));
    if (!p) { LOGE("gl1: out of memory for %d vertices", want); return 0; }
    g.verts = p;
    g.capacity = want;
    return 1;
}

void gl1_begin(GLenum mode)
{
    g.in_begin = 1;
    g.prim = mode;
    g.count = 0;
}

void gl1_vertex3f(float x, float y, float z)
{
    if (!g.in_begin || !reserve(1)) return;
    vertex *v = &g.verts[g.count++];
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
    memcpy(v->color, g.current_color, sizeof(v->color));
    memcpy(v->tex, g.current_tex, sizeof(v->tex));
    memcpy(v->normal, g.current_normal, sizeof(v->normal));
}

void gl1_vertex2f(float x, float y)        { gl1_vertex3f(x, y, 0.0f); }
void gl1_vertex3fv(const float *v)         { gl1_vertex3f(v[0], v[1], v[2]); }
void gl1_texcoord2f(float s, float t)      { g.current_tex[0] = s; g.current_tex[1] = t; }
void gl1_normal3f(float x, float y, float z)
{
    g.current_normal[0] = x; g.current_normal[1] = y; g.current_normal[2] = z;
}

void gl1_color4f(float r, float gg, float b, float a)
{
    g.current_color[0] = r; g.current_color[1] = gg;
    g.current_color[2] = b; g.current_color[3] = a;
}

void gl1_normal3fv(const float *n)   { gl1_normal3f(n[0], n[1], n[2]); }
void gl1_texcoord2fv(const float *t) { gl1_texcoord2f(t[0], t[1]); }

void gl1_get_floatv(GLenum pname, float *params)
{
    if (pname == GL_MODELVIEW_MATRIX)  { memcpy(params, g.modelview[g.mv_top], 16 * sizeof(float)); return; }
    if (pname == GL_PROJECTION_MATRIX) { memcpy(params, g.projection[g.proj_top], 16 * sizeof(float)); return; }
    glGetFloatv(pname, params);
}

void gl1_get_doublev(GLenum pname, gl1_double *params)
{
    const float *m = NULL;
    if (pname == GL_MODELVIEW_MATRIX)  m = g.modelview[g.mv_top];
    if (pname == GL_PROJECTION_MATRIX) m = g.projection[g.proj_top];
    if (!m) { LOGE("gl1: glGetDoublev 0x%x unsupported", pname); return; }
    for (int i = 0; i < 16; i++) params[i] = m[i];
}

int gl1_project(double ox, double oy, double oz, double *winx, double *winy, double *winz)
{
    float mvp[16];
    m_mul(mvp, g.projection[g.proj_top], g.modelview[g.mv_top]);

    const float in[4] = { (float) ox, (float) oy, (float) oz, 1.0f };
    float out[4];
    for (int i = 0; i < 4; i++)
        out[i] = mvp[i] * in[0] + mvp[4 + i] * in[1] + mvp[8 + i] * in[2] + mvp[12 + i] * in[3];

    if (out[3] == 0.0f) return 0;
    out[0] /= out[3]; out[1] /= out[3]; out[2] /= out[3];

    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    if (winx) *winx = vp[0] + vp[2] * (out[0] + 1.0f) * 0.5f;
    if (winy) *winy = vp[1] + vp[3] * (out[1] + 1.0f) * 0.5f;
    if (winz) *winz = (out[2] + 1.0f) * 0.5f;
    return 1;
}

void gl1_color3f(float r, float gg, float b)  { gl1_color4f(r, gg, b, 1.0f); }
void gl1_color3fv(const float *c)             { gl1_color4f(c[0], c[1], c[2], 1.0f); }
void gl1_color4fv(const float *c)             { gl1_color4f(c[0], c[1], c[2], c[3]); }

/* GL_QUADS has no GLES2 equivalent, so each quad becomes two triangles via an index buffer
 * built to fit whatever the largest batch so far needed. */
static const GLushort *quad_indices(int quads)
{
    const int needed = quads * 6;
    if (needed > g.index_capacity) {
        GLushort *p = (GLushort *) realloc(g.indices, (size_t) needed * sizeof(GLushort));
        if (!p) { LOGE("gl1: out of memory for %d indices", needed); return NULL; }
        g.indices = p;
        g.index_capacity = needed;
    }
    for (int q = 0; q < quads; q++) {
        const GLushort b = (GLushort) (q * 4);
        GLushort *i = &g.indices[q * 6];
        i[0] = b; i[1] = (GLushort) (b + 1); i[2] = (GLushort) (b + 2);
        i[3] = b; i[4] = (GLushort) (b + 2); i[5] = (GLushort) (b + 3);
    }
    return g.indices;
}

/* Program, matrices and the flags every draw needs, whichever path it came from. */
static void setup_program(void)
{
    float mvp[16];
    m_mul(mvp, g.projection[g.proj_top], g.modelview[g.mv_top]);

    glUseProgram(g.program);
    glUniformMatrix4fv(g.uMVP, 1, GL_FALSE, mvp);
    if (g.uMV >= 0) glUniformMatrix4fv(g.uMV, 1, GL_FALSE, g.modelview[g.mv_top]);
    glUniform1i(g.uTex, 0);
    glUniform1i(g.uUseTex, g.tex_enabled ? 1 : 0);
    if (g.uTexGen >= 0) glUniform1i(g.uTexGen, g.texgen ? 1 : 0);
    glUniform1f(g.uPointSize, g.point_size);
}

static void draw_batch(GLenum prim, const vertex *verts, int count)
{
    if (count == 0) return;

    g.stat_batches++;
    g.stat_vertices += count;
    setup_program();

    const GLsizei stride = sizeof(vertex);
    if (g.aPos >= 0) {
        glEnableVertexAttribArray((GLuint) g.aPos);
        glVertexAttribPointer((GLuint) g.aPos, 3, GL_FLOAT, GL_FALSE, stride, verts->pos);
    }
    if (g.aColor >= 0) {
        glEnableVertexAttribArray((GLuint) g.aColor);
        glVertexAttribPointer((GLuint) g.aColor, 4, GL_FLOAT, GL_FALSE, stride, verts->color);
    }
    if (g.aTex >= 0) {
        glEnableVertexAttribArray((GLuint) g.aTex);
        glVertexAttribPointer((GLuint) g.aTex, 2, GL_FLOAT, GL_FALSE, stride, verts->tex);
    }
    if (g.aNormal >= 0) {
        glEnableVertexAttribArray((GLuint) g.aNormal);
        glVertexAttribPointer((GLuint) g.aNormal, 3, GL_FLOAT, GL_FALSE, stride, verts->normal);
    }

    /* Immediate mode supplies its own vertices, so no VBO may be bound underneath. */
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    switch (prim) {
    case GL_QUADS: {
        const int quads = count / 4;
        const GLushort *idx = quad_indices(quads);
        if (idx) glDrawElements(GL_TRIANGLES, quads * 6, GL_UNSIGNED_SHORT, idx);
        break;
    }
    /* Vertex order already matches: a quad strip IS a triangle strip, and a convex polygon
     * IS a fan. */
    case GL_QUAD_STRIP: glDrawArrays(GL_TRIANGLE_STRIP, 0, count); break;
    case GL_POLYGON:    glDrawArrays(GL_TRIANGLE_FAN, 0, count);   break;
    default:            glDrawArrays(prim, 0, count);              break;
    }
}

void gl1_end(void)
{
    g.in_begin = 0;
    if (!g.ready || g.count == 0) return;

    if (g.recording) {
        /* Compiling: keep the batch instead of drawing it. */
        const int i = g.recording;
        void *p = realloc(g.lists[i].batches,
                          (size_t) (g.lists[i].n_batches + 1) * sizeof(*g.lists[i].batches));
        if (!p) { LOGE("gl1: out of memory recording list %d", i); return; }
        g.lists[i].batches = p;

        vertex *copy = (vertex *) malloc((size_t) g.count * sizeof(vertex));
        if (!copy) { LOGE("gl1: out of memory copying %d verts", g.count); return; }
        memcpy(copy, g.verts, (size_t) g.count * sizeof(vertex));

        g.lists[i].batches[g.lists[i].n_batches].prim  = g.prim;
        g.lists[i].batches[g.lists[i].n_batches].verts = copy;
        g.lists[i].batches[g.lists[i].n_batches].count = g.count;
        g.lists[i].n_batches++;
        return;
    }

    draw_batch(g.prim, g.verts, g.count);
}

/* ---- display lists ---- */

static void free_list(int i)
{
    for (int b = 0; b < g.lists[i].n_batches; b++) free(g.lists[i].batches[b].verts);
    free(g.lists[i].batches);
    g.lists[i].batches = NULL;
    g.lists[i].n_batches = 0;
}

GLuint gl1_gen_lists(GLsizei range)
{
    for (GLuint base = 1; base + (GLuint) range <= GL1_MAX_LISTS; base++) {
        int free_run = 1;
        for (GLsizei k = 0; k < range; k++)
            if (g.lists[base + k].used) { free_run = 0; break; }
        if (free_run) {
            for (GLsizei k = 0; k < range; k++) g.lists[base + k].used = 1;
            return base;
        }
    }
    LOGE("gl1: no room for %d display lists", range);
    return 0;
}

void gl1_new_list(GLuint list, GLenum mode)
{
    (void) mode;  /* GL_COMPILE only; nothing here uses GL_COMPILE_AND_EXECUTE. */
    if (list == 0 || list >= GL1_MAX_LISTS) { LOGE("gl1: bad list %u", list); return; }
    free_list((int) list);
    g.lists[list].used = 1;
    g.recording = (int) list;
}

void gl1_end_list(void) { g.recording = 0; }

void gl1_call_list(GLuint list)
{
    if (list == 0 || list >= GL1_MAX_LISTS) return;
    for (int b = 0; b < g.lists[list].n_batches; b++)
        draw_batch(g.lists[list].batches[b].prim,
                   g.lists[list].batches[b].verts,
                   g.lists[list].batches[b].count);
}

void gl1_delete_lists(GLuint list, GLsizei range)
{
    for (GLsizei k = 0; k < range; k++) {
        const GLuint i = list + (GLuint) k;
        if (i > 0 && i < GL1_MAX_LISTS) { free_list((int) i); g.lists[i].used = 0; }
    }
}

/* ---- GLU quadrics ---- */

struct gl1_quadric { int unused; };
static struct gl1_quadric the_quadric;

gl1_quadric *gl1_new_quadric(void)          { return &the_quadric; }
void gl1_delete_quadric(gl1_quadric *q)     { (void) q; }

void gl1_sphere(gl1_quadric *q, double radius, int slices, int stacks)
{
    (void) q;
    if (slices < 3) slices = 3;
    if (stacks < 2) stacks = 2;

    /* A plain UV sphere emitted as triangle strips through the immediate-mode path, so that
     * inside glNewList it records exactly like any other geometry. */
    for (int st = 0; st < stacks; st++) {
        const double p0 = M_PI * ((double) st / stacks - 0.5);
        const double p1 = M_PI * ((double) (st + 1) / stacks - 0.5);
        const double y0 = sin(p0), y1 = sin(p1);
        const double r0 = cos(p0), r1 = cos(p1);

        gl1_begin(GL_TRIANGLE_STRIP);
        for (int sl = 0; sl <= slices; sl++) {
            const double th = 2.0 * M_PI * (double) sl / slices;
            const double cs = cos(th), sn = sin(th);
            gl1_texcoord2f((float) sl / slices, (float) st / stacks);
            gl1_vertex3f((float) (radius * r0 * cs), (float) (radius * y0),
                         (float) (radius * r0 * sn));
            gl1_texcoord2f((float) sl / slices, (float) (st + 1) / stacks);
            gl1_vertex3f((float) (radius * r1 * cs), (float) (radius * y1),
                         (float) (radius * r1 * sn));
        }
        gl1_end();
    }
}

/* ---- state ---- */

void gl1_enable(GLenum cap)
{
    if (cap == GL_TEXTURE_2D_ENABLE_COMPAT) { g.tex_enabled = 1; return; }
    /* S and T are always enabled together by these savers; one flag covers both. */
    if (cap == GL_TEXTURE_GEN_S || cap == GL_TEXTURE_GEN_T) { g.texgen = 1; return; }
    /* Gone in GLES2; passing them sets GL_INVALID_ENUM and the sticky error then gets blamed
     * on whatever runs next. */
    if (cap == GL_ALPHA_TEST || cap == GL_LIGHTING || cap == GL_LIGHT0 ||
        cap == GL_NORMALIZE || cap == GL_COLOR_MATERIAL || cap == GL_FOG ||
        cap == GL_POINT_SMOOTH || cap == GL_LINE_SMOOTH) return;
    /* Cube maps are not an enable in GLES2; the sampler type decides. */
    if (cap == GL_TEXTURE_CUBE_MAP) { g.tex_enabled = 1; return; }
    glEnable(cap);
}

void gl1_disable(GLenum cap)
{
    if (cap == GL_TEXTURE_2D_ENABLE_COMPAT) { g.tex_enabled = 0; return; }
    if (cap == GL_TEXTURE_GEN_S || cap == GL_TEXTURE_GEN_T) { g.texgen = 0; return; }
    if (cap == GL_ALPHA_TEST || cap == GL_LIGHTING || cap == GL_LIGHT0 ||
        cap == GL_NORMALIZE || cap == GL_COLOR_MATERIAL || cap == GL_FOG ||
        cap == GL_POINT_SMOOTH || cap == GL_LINE_SMOOTH) return;
    if (cap == GL_TEXTURE_CUBE_MAP) { g.tex_enabled = 0; return; }
    glDisable(cap);
}

/* ---- vertex arrays ---- */

static void set_array(client_array *a, GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    a->size = size; a->type = type; a->stride = stride; a->ptr = ptr;
}

void gl1_vertex_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{ set_array(&g.ca_vertex, size, type, stride, ptr); }
void gl1_normal_pointer(GLenum type, GLsizei stride, const void *ptr)
{ set_array(&g.ca_normal, 3, type, stride, ptr); }
void gl1_color_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{ set_array(&g.ca_color, size, type, stride, ptr); }
void gl1_texcoord_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{ set_array(&g.ca_texcoord, size, type, stride, ptr); }

static client_array *array_for(GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY:        return &g.ca_vertex;
    case GL_NORMAL_ARRAY:        return &g.ca_normal;
    case GL_COLOR_ARRAY:         return &g.ca_color;
    case GL_TEXTURE_COORD_ARRAY: return &g.ca_texcoord;
    default:                     return NULL;
    }
}

void gl1_enable_client_state(GLenum cap)
{ client_array *a = array_for(cap); if (a) a->enabled = 1; }
void gl1_disable_client_state(GLenum cap)
{ client_array *a = array_for(cap); if (a) a->enabled = 0; }

void gl1_interleaved_arrays(GLenum format, GLsizei stride, const void *ptr)
{
    /* GL_N3F_V3F is the only layout the savers use: normal then position, both 3 floats. */
    if (format != GL_N3F_V3F) { LOGE("gl1: unsupported interleaved format 0x%x", format); return; }

    const GLsizei s = stride ? stride : (GLsizei) (6 * sizeof(float));
    set_array(&g.ca_normal, 3, GL_FLOAT, s, ptr);
    set_array(&g.ca_vertex, 3, GL_FLOAT, s, (const char *) ptr + 3 * sizeof(float));
    g.ca_normal.enabled = 1;
    g.ca_vertex.enabled = 1;
    g.ca_color.enabled = 0;
    g.ca_texcoord.enabled = 0;
}

static void bind_client(GLint loc, const client_array *a, const float *fallback)
{
    if (loc < 0) return;
    if (!a->enabled) {
        glDisableVertexAttribArray((GLuint) loc);
        if (fallback) glVertexAttrib4fv((GLuint) loc, fallback);
        return;
    }
    glEnableVertexAttribArray((GLuint) loc);
    /* ptr is a VBO offset when a buffer is bound, which glVertexAttribPointer already
     * understands -- so it passes straight through either way. */
    glVertexAttribPointer((GLuint) loc, a->size, a->type, GL_FALSE, a->stride, a->ptr);
}

static void bind_client_arrays(void)
{
    g.stat_batches++;
    setup_program();
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const float up[4]    = { 0.0f, 0.0f, 1.0f, 0.0f };
    bind_client(g.aPos,    &g.ca_vertex,   NULL);
    bind_client(g.aColor,  &g.ca_color,    g.current_color);
    bind_client(g.aTex,    &g.ca_texcoord, NULL);
    bind_client(g.aNormal, &g.ca_normal,   up);
    (void) white;
}

void gl1_draw_arrays(GLenum mode, GLint first, GLsizei count)
{
    if (!g.ready || count <= 0) return;
    bind_client_arrays();

    if (mode == GL_QUADS) {
        if (first != 0) { LOGE("gl1: GL_QUADS with first=%d unsupported", first); return; }
        const int quads = count / 4;
        const GLushort *idx = quad_indices(quads);
        if (idx) glDrawElements(GL_TRIANGLES, quads * 6, GL_UNSIGNED_SHORT, idx);
        return;
    }
    if (mode == GL_QUAD_STRIP) mode = GL_TRIANGLE_STRIP;
    else if (mode == GL_POLYGON) mode = GL_TRIANGLE_FAN;
    glDrawArrays(mode, first, count);
}

void gl1_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    if (!g.ready || count <= 0) return;
    bind_client_arrays();
    if (mode == GL_QUAD_STRIP) mode = GL_TRIANGLE_STRIP;
    else if (mode == GL_POLYGON) mode = GL_TRIANGLE_FAN;
    glDrawElements(mode, count, type, indices);
}

void gl1_multi_draw_elements(GLenum mode, const GLsizei *count, GLenum type,
                             const void *const *indices, GLsizei primcount)
{
    /* Not in GLES2; the savers use it for one triangle strip per element, so a loop is exactly
     * equivalent. */
    for (GLsizei i = 0; i < primcount; i++)
        gl1_draw_elements(mode, count[i], type, indices[i]);
}

void gl1_frame_begin(void)
{
    if (g.clear_frames > 0) {
        g.clear_frames--;
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    /* Re-asserted every frame: the savers call glClear and glColorMask themselves, and a saver
     * that resets the mask would start writing alpha again. */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
}

void gl1_stats_take(int *batches, long *vertices, unsigned *last_error)
{
    if (batches)  *batches  = g.stat_batches;
    if (vertices) *vertices = g.stat_vertices;
    if (last_error) *last_error = glGetError();
    g.stat_batches = 0;
    g.stat_vertices = 0;
}

void gl1_point_size(float size) { g.point_size = size; }

void gl1_build_2d_mipmaps(GLenum target, GLint components, GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const void *data)
{
    gl1_tex_image_2d(target, 0, components, width, height, 0, format, type, data);
    glGenerateMipmap(target);
}

static int components_of(GLenum format)
{
    switch (format) {
    case GL_RGB:             return 3;
    case GL_RGBA:            return 4;
    case GL_LUMINANCE_ALPHA: return 2;
    default:                 return 1;
    }
}

/* Repacks GL_FLOAT pixels into tight GL_UNSIGNED_BYTE, honouring GL_UNPACK_ROW_LENGTH.
 * Returns NULL when no conversion is needed. Caller frees. */
static unsigned char *repack(GLsizei width, GLsizei height, GLenum format, GLenum type,
                             const void *pixels)
{
    if (type != GL_FLOAT || !pixels || width <= 0 || height <= 0) return NULL;

    const int comps = components_of(format);
    const GLsizei src_row = g.unpack_row_length > 0 ? g.unpack_row_length : width;
    unsigned char *out = (unsigned char *) malloc((size_t) width * height * comps);
    if (!out) { LOGE("gl1: out of memory repacking %dx%d", width, height); return NULL; }

    const float *src = (const float *) pixels;
    for (GLsizei y = 0; y < height; y++) {
        for (GLsizei x = 0; x < width * comps; x++) {
            float v = src[(size_t) y * src_row * comps + x];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            out[(size_t) y * width * comps + x] = (unsigned char) (v * 255.0f + 0.5f);
        }
    }
    return out;
}

void gl1_pixel_store_i(GLenum pname, GLint param)
{
    /* Recorded rather than forwarded: GLES2 rejects the enum, and repack() applies it. */
    if (pname == GL_UNPACK_ROW_LENGTH) { g.unpack_row_length = param; return; }
    glPixelStorei(pname, param);
}

void gl1_tex_sub_image_2d(GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei width,
                          GLsizei height, GLenum format, GLenum type, const void *pixels)
{
    unsigned char *conv = repack(width, height, format, type, pixels);
    glTexSubImage2D(target, level, xoff, yoff, width, height, format,
                    conv ? GL_UNSIGNED_BYTE : type, conv ? conv : pixels);
    free(conv);
}

void gl1_tex_image_2d(GLenum target, GLint level, GLint internalformat, GLsizei width,
                      GLsizei height, GLint border, GLenum format, GLenum type,
                      const void *pixels)
{
    switch (internalformat) {
    case 1: internalformat = GL_LUMINANCE;       break;
    case 2: internalformat = GL_LUMINANCE_ALPHA; break;
    case 3: internalformat = GL_RGB;             break;
    case 4: internalformat = GL_RGBA;            break;
    default: break;  /* already a real enum */
    }

    unsigned char *conv = repack(width, height, format, type, pixels);
    glTexImage2D(target, level, internalformat, width, height, border, format,
                 conv ? GL_UNSIGNED_BYTE : type, conv ? conv : pixels);
    free(conv);
}
