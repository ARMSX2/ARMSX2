/*
 * The ARB_shader_objects surface Hyperspace expects.
 *
 * Hyperspace has two rendering paths and picks between them with dShaders. The shader path uses
 * ARB assembly-era shader objects, which GLES2 does not have at all. Rather than translate them,
 * the types are declared so the code compiles and the entry points are looked up at runtime --
 * on GLES2 they come back null, extensions.cpp reports the extension missing, and the saver
 * takes its own non-shader path, which is a supported upstream configuration rather than
 * something invented here.
 */
#ifndef SAVERS_COMPAT_ARB_SHADERS_H
#define SAVERS_COMPAT_ARB_SHADERS_H

#include <EGL/egl.h>
#include "gl1.h"

typedef unsigned int GLhandleARB;
typedef char GLcharARB;

typedef void (*PFNGLACTIVETEXTUREARBPROC)(GLenum);
typedef void (*PFNGLMULTITEXCOORD2FARBPROC)(GLenum, GLfloat, GLfloat);
typedef GLhandleARB (*PFNGLCREATEPROGRAMOBJECTARBPROC)(void);
typedef GLhandleARB (*PFNGLCREATESHADEROBJECTARBPROC)(GLenum);
typedef void (*PFNGLSHADERSOURCEARBPROC)(GLhandleARB, GLsizei, const GLcharARB **, const GLint *);
typedef void (*PFNGLCOMPILESHADERARBPROC)(GLhandleARB);
typedef void (*PFNGLATTACHOBJECTARBPROC)(GLhandleARB, GLhandleARB);
typedef void (*PFNGLLINKPROGRAMARBPROC)(GLhandleARB);
typedef void (*PFNGLUSEPROGRAMOBJECTARBPROC)(GLhandleARB);
typedef GLint (*PFNGLGETUNIFORMLOCATIONARBPROC)(GLhandleARB, const GLcharARB *);
typedef void (*PFNGLUNIFORM1IARBPROC)(GLint, GLint);
typedef void (*PFNGLUNIFORM1FARBPROC)(GLint, GLfloat);
typedef void (*PFNGLUNIFORM4FARBPROC)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFNGLGETOBJECTPARAMETERIVARBPROC)(GLhandleARB, GLenum, GLint *);
typedef void (*PFNGLGETINFOLOGARBPROC)(GLhandleARB, GLsizei, GLsizei *, GLcharARB *);
typedef void (*PFNGLDELETEOBJECTARBPROC)(GLhandleARB);

/* Multitexture unit names; core in GLES2 under the unsuffixed spelling. */
#ifndef GL_TEXTURE0_ARB
#define GL_TEXTURE0_ARB GL_TEXTURE0
#define GL_TEXTURE1_ARB GL_TEXTURE1
#define GL_TEXTURE2_ARB GL_TEXTURE2
#define GL_TEXTURE3_ARB GL_TEXTURE3
#endif

#ifndef GL_VERTEX_SHADER_ARB
#define GL_VERTEX_SHADER_ARB    0x8B31
#define GL_FRAGMENT_SHADER_ARB  0x8B30
#define GL_OBJECT_COMPILE_STATUS_ARB 0x8B81
#define GL_OBJECT_LINK_STATUS_ARB    0x8B82
#endif

/* Upstream only includes its own extensions.h under WIN32 -- its X11 build relies on the system
 * GL exporting these symbols directly, which Mesa does and Android does not. So they are
 * declared here, at global scope, where the savers' namespaced code still finds them.
 *
 * Nothing calls them: every use sits behind dShaders, which the port forces off. glActiveTexture
 * is the one exception, being core in GLES2, so that one points at the real function. */
extern PFNGLACTIVETEXTUREARBPROC glActiveTextureARB;
extern PFNGLCREATESHADEROBJECTARBPROC glCreateShaderObjectARB;
extern PFNGLSHADERSOURCEARBPROC glShaderSourceARB;
extern PFNGLCOMPILESHADERARBPROC glCompileShaderARB;
extern PFNGLCREATEPROGRAMOBJECTARBPROC glCreateProgramObjectARB;
extern PFNGLATTACHOBJECTARBPROC glAttachObjectARB;
extern PFNGLLINKPROGRAMARBPROC glLinkProgramARB;
extern PFNGLUSEPROGRAMOBJECTARBPROC glUseProgramObjectARB;
extern PFNGLGETUNIFORMLOCATIONARBPROC glGetUniformLocationARB;
extern PFNGLUNIFORM1IARBPROC glUniform1iARB;

/* Upstream resolves entry points through the platform's GetProcAddress; on Android that is
 * EGL's. Anything ARB-only returns null here, which is the point. */
#define glXGetProcAddressARB(name) ((void *) eglGetProcAddress((const char *) (name)))

#endif
