/*
 * Enough OpenGL 1.x for the Really Slick Screensavers, implemented on GLES2.
 *
 * Terry Welsh's screensavers are late-90s/2000s OpenGL: immediate mode (glBegin/glVertex3f),
 * the fixed-function matrix stack (glPushMatrix/glTranslatef/gluPerspective) and fixed-function
 * lighting. GLES2 has none of it. Rather than rewrite each renderer -- and lose the property
 * that these stay recognisably his code, which the GPL headers on them ask us to preserve --
 * the calls are redirected here and answered with one shader and a matrix stack.
 *
 * This is deliberately a SEPARATE shim from flurry/gl_compat.h. That one is Flurry-shaped:
 * client-side vertex arrays and a 2D ortho, with glMatrixMode a no-op. Making its matrices real
 * would risk a renderer that took a long time to get right. Once a couple of savers are running
 * on this file, Flurry can move over to it and the two can merge.
 *
 * NOT emulated yet, because no ported saver has needed it: fixed-function lighting. glLightfv
 * and friends are accepted and ignored, so anything relying on them draws unlit. Flux calls
 * them -- check what it actually looks like before deciding whether to implement per-vertex
 * lighting here or bake it into that saver.
 */
#ifndef SAVERS_GL1_H
#define SAVERS_GL1_H

#include <GLES2/gl2.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GL 1.x names GLES2 does not define. */
#ifndef GL_QUADS
#define GL_QUADS          0x0007
#endif
#ifndef GL_QUAD_STRIP
#define GL_QUAD_STRIP     0x0008
#endif
#ifndef GL_POLYGON
#define GL_POLYGON        0x0009
#endif
#ifndef GL_MODELVIEW
#define GL_MODELVIEW      0x1700
#endif
#ifndef GL_PROJECTION
#define GL_PROJECTION     0x1701
#endif
#ifndef GL_ALPHA_TEST
#define GL_ALPHA_TEST     0x0BC0
#endif
#ifndef GL_LIGHTING
#define GL_LIGHTING       0x0B50
#endif
#ifndef GL_LIGHT0
#define GL_LIGHT0         0x4000
#endif
#ifndef GL_NORMALIZE
#define GL_NORMALIZE      0x0BA1
#endif
#ifndef GL_COLOR_MATERIAL
#define GL_COLOR_MATERIAL 0x0B57
#endif
#ifndef GL_POINT_SMOOTH
#define GL_POINT_SMOOTH   0x0B10
#endif
#ifndef GL_LINE_SMOOTH
#define GL_LINE_SMOOTH    0x0B20
#endif
#ifndef GL_COMPILE
#define GL_COMPILE        0x1300
#endif
#ifndef GL_COMPILE_AND_EXECUTE
#define GL_COMPILE_AND_EXECUTE 0x1301
#endif
#ifndef GL_TEXTURE_GEN_S
#define GL_TEXTURE_GEN_S  0x0C60
#endif
#ifndef GL_TEXTURE_GEN_T
#define GL_TEXTURE_GEN_T  0x0C61
#endif
/* Cube maps are core in GLES2; only the ARB spelling is missing. */
#ifndef GL_TEXTURE_CUBE_MAP_ARB
#define GL_TEXTURE_CUBE_MAP_ARB             GL_TEXTURE_CUBE_MAP
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB  GL_TEXTURE_CUBE_MAP_POSITIVE_X
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB  GL_TEXTURE_CUBE_MAP_NEGATIVE_X
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB  GL_TEXTURE_CUBE_MAP_POSITIVE_Y
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB  GL_TEXTURE_CUBE_MAP_NEGATIVE_Y
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB  GL_TEXTURE_CUBE_MAP_POSITIVE_Z
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB  GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
#define GL_REFLECTION_MAP_ARB               0x8512
#define GL_NORMAL_MAP_ARB                   0x8511
#endif
#ifndef GL_FOG
#define GL_FOG            0x0B60
#endif
#ifndef GL_MODELVIEW_MATRIX
#define GL_MODELVIEW_MATRIX  0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
/* GLES2 has only CLAMP_TO_EDGE. The savers use GL_CLAMP on textures they never sample outside
 * [0,1], so the border-vs-edge difference is not reachable. */
#ifndef GL_CLAMP
#define GL_CLAMP          GL_CLAMP_TO_EDGE
#endif
#ifndef GL_DECAL
#define GL_DECAL          0x2101
#endif
#ifndef GL_VERTEX_ARRAY
#define GL_VERTEX_ARRAY   0x8074
#endif
#ifndef GL_NORMAL_ARRAY
#define GL_NORMAL_ARRAY   0x8075
#endif
#ifndef GL_COLOR_ARRAY
#define GL_COLOR_ARRAY    0x8076
#endif
#ifndef GL_TEXTURE_COORD_ARRAY
#define GL_TEXTURE_COORD_ARRAY 0x8078
#endif
#ifndef GL_N3F_V3F
#define GL_N3F_V3F        0x2A25
#endif
#ifndef GL_S
#define GL_S              0x2000
#endif
#ifndef GL_T
#define GL_T              0x2001
#endif
#ifndef GL_TEXTURE_GEN_MODE
#define GL_TEXTURE_GEN_MODE 0x2500
#endif
#ifndef GL_SPHERE_MAP
#define GL_SPHERE_MAP     0x2402
#endif
#ifndef GL_TEXTURE_2D_ENABLE_COMPAT
#define GL_TEXTURE_2D_ENABLE_COMPAT 0x0DE1
#endif

/* Lifetime. gl1_init needs a current context; gl1_lost forgets GL names without touching
 * them, for when the context has already gone away underneath us. */
int  gl1_init(void);
void gl1_shutdown(void);
void gl1_lost(void);

/* Matrix stack. */
void gl1_matrix_mode(GLenum mode);
void gl1_load_identity(void);
void gl1_push_matrix(void);
void gl1_pop_matrix(void);
void gl1_load_matrixf(const float *m);
void gl1_mult_matrixf(const float *m);
void gl1_translatef(float x, float y, float z);
void gl1_scalef(float x, float y, float z);
void gl1_rotatef(float angle, float x, float y, float z);
void gl1_ortho(double l, double r, double b, double t, double n, double f);
void gl1_frustum(double l, double r, double b, double t, double n, double f);
void gl1_perspective(double fovy, double aspect, double zn, double zf);
void gl1_look_at(double ex, double ey, double ez, double cx, double cy, double cz,
                 double ux, double uy, double uz);

/* Immediate mode. */
void gl1_begin(GLenum mode);
void gl1_end(void);
void gl1_vertex2f(float x, float y);
void gl1_vertex3f(float x, float y, float z);
void gl1_vertex3fv(const float *v);
void gl1_texcoord2f(float s, float t);
void gl1_color3f(float r, float g, float b);
void gl1_color4f(float r, float g, float b, float a);
void gl1_color3fv(const float *c);
void gl1_color4fv(const float *c);
void gl1_normal3f(float x, float y, float z);
void gl1_normal3fv(const float *n);
void gl1_texcoord2fv(const float *t);

/* The savers read the matrices back to hand them to gluProject. GLES2 has no matrix state at
 * all, so these answer from the shim's own stack. GLdouble is likewise absent. */
typedef double gl1_double;
void gl1_get_floatv(GLenum pname, float *params);
void gl1_get_doublev(GLenum pname, gl1_double *params);

/* Object space to window space, for savers that place 2D overlays over 3D positions. */
int gl1_project(double ox, double oy, double oz, double *winx, double *winy, double *winz);

/* Display lists.
 *
 * The savers build a shape once (a sphere, or a textured quad) and replay it per particle with
 * a different matrix each time -- so this is not an optimisation to skip, it IS how they draw.
 * Recording captures the immediate-mode batches; replaying re-draws them under whatever the
 * matrices currently are. */
GLuint gl1_gen_lists(GLsizei range);
void   gl1_new_list(GLuint list, GLenum mode);
void   gl1_end_list(void);
void   gl1_call_list(GLuint list);
void   gl1_delete_lists(GLuint list, GLsizei range);

/* GLU quadrics. Only spheres are used, and only as list content, so the quadric object is a
 * token and the sphere is emitted through the immediate-mode path above. */
typedef struct gl1_quadric gl1_quadric;
gl1_quadric *gl1_new_quadric(void);
void gl1_delete_quadric(gl1_quadric *q);
void gl1_sphere(gl1_quadric *q, double radius, int slices, int stacks);

/* Vertex arrays.
 *
 * The Implicit surface library draws through these, with real VBOs -- so unlike the immediate
 * mode above, the data is already in a form GLES2 accepts and only the fixed-function
 * plumbing (which attribute each pointer feeds, and the enable flags) has to be emulated. */
void gl1_vertex_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
void gl1_normal_pointer(GLenum type, GLsizei stride, const void *ptr);
void gl1_color_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
void gl1_texcoord_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
void gl1_enable_client_state(GLenum cap);
void gl1_disable_client_state(GLenum cap);
void gl1_interleaved_arrays(GLenum format, GLsizei stride, const void *ptr);
void gl1_draw_arrays(GLenum mode, GLint first, GLsizei count);
void gl1_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices);
void gl1_multi_draw_elements(GLenum mode, const GLsizei *count, GLenum type,
                             const void *const *indices, GLsizei primcount);

/* State GLES2 rejects outright; an invalid enum would leave a sticky error for everything
 * after it, so these are filtered rather than passed through. */
void gl1_enable(GLenum cap);
void gl1_disable(GLenum cap);
void gl1_point_size(float size);

/* Draw counters since the last call, for working out whether a black saver is drawing nothing
 * or drawing something invisible. Resets on read. */
void gl1_stats_take(int *batches, long *vertices, unsigned *last_error);

/* Call at the top of every frame, before the saver draws.
 *
 * Handles the two things that make an accumulating saver show TV static on a composited
 * surface, both learned the hard way from Flurry:
 *   - the buffer starts UNDEFINED, so the first few frames are cleared outright; a saver that
 *     fades instead of clearing never establishes a known background on its own
 *   - the fade writes destination ALPHA as well as colour, dragging the surface toward
 *     transparent so the window compositor blends whatever is behind it. Alpha is set to 1 once
 *     and then masked off, which leaves RGB and the trails untouched. */
void gl1_frame_begin(void);

/* GL 1.x let internalformat be a component COUNT (1..4). GLES2 requires the actual enum and
 * rejects the number with GL_INVALID_ENUM -- which leaves a black texture and a sticky error
 * blamed on whatever runs next. Flux passes 1. */
/* GLU built the mip chain on the CPU; GLES2 has glGenerateMipmap. Same component-count
 * translation as above applies to the internalformat argument. */
void gl1_build_2d_mipmaps(GLenum target, GLint components, GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const void *data);

/* GLES2 accepts neither GL_FLOAT pixel data nor GL_UNPACK_ROW_LENGTH. Plasma uses both: it
 * keeps a float RGB map and uploads a sub-rectangle of it. These repack into tight bytes. */
void gl1_pixel_store_i(GLenum pname, GLint param);
void gl1_tex_sub_image_2d(GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei width,
                          GLsizei height, GLenum format, GLenum type, const void *pixels);

void gl1_tex_image_2d(GLenum target, GLint level, GLint internalformat, GLsizei width,
                      GLsizei height, GLint border, GLenum format, GLenum type,
                      const void *pixels);

#ifdef __cplusplus
}
#endif

/* The redirection. Included by each saver's sources ahead of any GL use.
 *
 * gl1.c MUST #undef every name below before defining the functions, or each implementation
 * calls itself -- and at -O2 that becomes a silent infinite loop rather than a crash. */
#define glMatrixMode      gl1_matrix_mode
#define glLoadIdentity    gl1_load_identity
#define glPushMatrix      gl1_push_matrix
#define glPopMatrix       gl1_pop_matrix
#define glLoadMatrixf     gl1_load_matrixf
#define glMultMatrixf     gl1_mult_matrixf
#define glTranslatef      gl1_translatef
#define glScalef          gl1_scalef
#define glRotatef         gl1_rotatef
#define glOrtho           gl1_ortho
#define glFrustum         gl1_frustum
#define gluPerspective    gl1_perspective
#define gluLookAt         gl1_look_at
#define glBegin           gl1_begin
#define glEnd             gl1_end
#define glVertex2f        gl1_vertex2f
#define glVertex3f        gl1_vertex3f
#define glVertex3fv       gl1_vertex3fv
#define glTexCoord2f      gl1_texcoord2f
#define glColor3f         gl1_color3f
#define glColor4f         gl1_color4f
#define glColor3fv        gl1_color3fv
#define glColor4fv        gl1_color4fv
#define glNormal3f        gl1_normal3f
#define glNormal3fv       gl1_normal3fv
#define glTexCoord2fv     gl1_texcoord2fv
#define glEnable          gl1_enable
#define glDisable         gl1_disable
#define glPointSize       gl1_point_size
#define glGenLists        gl1_gen_lists
#define glNewList         gl1_new_list
#define glEndList         gl1_end_list
#define glCallList        gl1_call_list
#define glDeleteLists     gl1_delete_lists
#define GLUquadricObj     gl1_quadric
#define gluNewQuadric     gl1_new_quadric
#define gluDeleteQuadric  gl1_delete_quadric
#define gluSphere         gl1_sphere
#define glTexImage2D      gl1_tex_image_2d
#define gluBuild2DMipmaps gl1_build_2d_mipmaps
#define glTexSubImage2D   gl1_tex_sub_image_2d
#define glPixelStorei     gl1_pixel_store_i
#define gluOrtho2D(l, r, b, t) gl1_ortho((l), (r), (b), (t), -1.0, 1.0)
/* The savers pass the matrices and viewport they just queried; this shim keeps its own copies,
 * so those arguments are ignored and the live state is used instead. */
#define gluProject(ox, oy, oz, mv, pj, vp, wx, wy, wz) \
    gl1_project((ox), (oy), (oz), (wx), (wy), (wz))

/* No analogue, and nothing ported so far depends on the effect. */
#define glShadeModel(m)        ((void) 0)
#define glAlphaFunc(f, r)      ((void) 0)
#define glTexEnvf(t, p, v)     ((void) 0)
#define glTexEnvi(t, p, v)     ((void) 0)
#define glLightfv(l, p, v)     ((void) 0)
#define glLightf(l, p, v)      ((void) 0)
#define glMaterialfv(f, p, v)  ((void) 0)
#define glMaterialf(f, p, v)   ((void) 0)
#define glColorMaterial(f, m)  ((void) 0)
#define glFogfv(p, v)          ((void) 0)
#define glFogf(p, v)           ((void) 0)
#define glFogi(p, v)           ((void) 0)
#define glHint(t, m)           ((void) 0)
#define glDrawBuffer(m)        ((void) 0)
#define glReadBuffer(m)        ((void) 0)
#define glGetFloatv            gl1_get_floatv
#define glGetDoublev           gl1_get_doublev
#define GLdouble               gl1_double
#define glFinish()             ((void) 0)
#define glPolygonMode(f, m)    ((void) 0)
/* The generation MODE is not stored: GL_SPHERE_MAP is the only one the savers ask for, and
 * gl1_vertex3f implements exactly that. A saver wanting GL_OBJECT_LINEAR or GL_EYE_LINEAR
 * would silently get sphere mapping instead, so check this if one ever looks wrong. */
#define glVertexPointer      gl1_vertex_pointer
#define glNormalPointer      gl1_normal_pointer
#define glColorPointer       gl1_color_pointer
#define glTexCoordPointer    gl1_texcoord_pointer
#define glEnableClientState  gl1_enable_client_state
#define glDisableClientState gl1_disable_client_state
#define glInterleavedArrays  gl1_interleaved_arrays
#define glDrawArrays         gl1_draw_arrays
#define glDrawElements       gl1_draw_elements
#define glMultiDrawElements  gl1_multi_draw_elements
#define glTexGeni(coord, pname, param)  ((void) 0)
#define glTexGenfv(coord, pname, param) ((void) 0)
#define glLightModeli(p, v)    ((void) 0)
#define glLightModelfv(p, v)   ((void) 0)

#endif /* SAVERS_GL1_H */
