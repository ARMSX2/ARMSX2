/*
 * Just enough OpenGL 1.x for Flurry, implemented on GLES2.
 *
 * Flurry is from 2002 and draws the only way that existed then: client-side vertex arrays
 * (glVertexPointer and friends), a fixed-function ortho projection, and GL_QUADS. GLES2 has
 * none of those. Rather than rewrite the renderer -- and lose the property that this is
 * recognisably Calum Robinson's code -- the calls are redirected here and answered with a
 * shader, an index buffer, and a matrix uniform.
 *
 * The surface is small because the smoke is the only thing drawn: DRAW_SPARKS is never
 * defined upstream, so the immediate-mode spark path (glBegin/glVertex2f, matrix stack) is
 * dead code and is not emulated. If sparks are ever switched on, this file needs a lot more
 * in it.
 */
#ifndef FLURRY_GL_COMPAT_H
#define FLURRY_GL_COMPAT_H

#include <GLES2/gl2.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GL 1.x names the sources use that GLES2 does not define. */
#ifndef GL_QUADS
#define GL_QUADS 0x0007
#endif
#ifndef GL_QUAD_STRIP
#define GL_QUAD_STRIP 0x0008
#endif
#ifndef GL_ALPHA_TEST
#define GL_ALPHA_TEST 0x0BC0
#endif
#ifndef GL_LIGHTING
#define GL_LIGHTING 0x0B50
#endif
#ifndef GL_VERTEX_ARRAY
#define GL_VERTEX_ARRAY 0x8074
#endif
#ifndef GL_COLOR_ARRAY
#define GL_COLOR_ARRAY 0x8076
#endif
#ifndef GL_TEXTURE_COORD_ARRAY
#define GL_TEXTURE_COORD_ARRAY 0x8078
#endif
#ifndef GL_LINEAR_MIPMAP_NEAREST
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#endif
#ifndef GL_TEXTURE_2D_ENABLE_COMPAT
#define GL_TEXTURE_2D_ENABLE_COMPAT 0x0DE1
#endif

/* Lifetime. fx_init needs a current context; fx_lost forgets GL names without touching them,
 * for when the context has already gone away underneath us. */
int  fx_init(void);
void fx_shutdown(void);
void fx_lost(void);

/* Replaces the projection matrix calls. */
void fx_ortho(float left, float right, float bottom, float top);

/* Array + draw emulation. */
void fx_vertex_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
void fx_color_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
void fx_texcoord_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
void fx_enable_client_state(GLenum cap);
void fx_disable_client_state(GLenum cap);
void fx_draw_arrays(GLenum mode, GLint first, GLsizei count);

/* glEnable/glDisable filtered: GLES2 rejects GL_ALPHA_TEST and GL_LIGHTING outright, and an
 * invalid enum here would leave a sticky GL error for everything after it. */
void fx_enable(GLenum cap);
void fx_disable(GLenum cap);

/* The per-frame fade. Flurry darkens the whole screen with a translucent black rect before
 * drawing, which is what gives the smoke its trails -- so this is not decoration, it is the
 * effect. glRectd and the current-colour state it reads are both gone in GLES2. */
void fx_color4f(float r, float g, float b, float a);
void fx_rect(float x0, float y0, float x1, float y1);

#ifdef __cplusplus
}
#endif

/* The redirection itself. Included by the Flurry sources ahead of any GL use. */
#define glVertexPointer      fx_vertex_pointer
#define glColorPointer       fx_color_pointer
#define glTexCoordPointer    fx_texcoord_pointer
#define glEnableClientState  fx_enable_client_state
#define glDisableClientState fx_disable_client_state
#define glDrawArrays         fx_draw_arrays
#define glEnable             fx_enable
#define glDisable            fx_disable

/* No analogue and nothing depends on the effect. */
#define glAlphaFunc(func, ref)   ((void) 0)
#define glShadeModel(mode)       ((void) 0)
#define glMatrixMode(mode)       ((void) 0)
#define glLoadIdentity()         ((void) 0)
#define glTexEnvf(t, p, v)       ((void) 0)
#define glOrtho(l, r, b, t, n, f) fx_ortho((float) (l), (float) (r), (float) (b), (float) (t))
#define glColor4f                fx_color4f
#define glColor3f(r, g, b)       fx_color4f((r), (g), (b), 1.0f)
#define glRectd(x0, y0, x1, y1)  fx_rect((float) (x0), (float) (y0), (float) (x1), (float) (y1))
#define glDrawBuffer(mode)       ((void) 0)
#define glFinish()               ((void) 0)

#endif /* FLURRY_GL_COMPAT_H */
