/*
 * The handful of globals every saver's RS_XSCREENSAVER path expects the host to own.
 *
 * Declared by compat/rsXScreenSaver/rsXScreenSaver.h; defined once here rather than per saver,
 * since they would collide if each one defined its own. Types mirror upstream exactly -- the
 * flags are int, not bool.
 */

/* The host view stops calling a saver when it is not visible rather than flagging suspension,
 * so these stay 0. */
int checkingPassword = 0;
int isSuspended = 0;
int doingPreview = 0;

/* Pixel-format bits the X11 host reported. Read only by savers deciding whether they can rely
 * on the buffer surviving a swap. We guarantee that at the EGL level instead
 * (EGL_BUFFER_PRESERVED), so leaving these 0 costs nothing. */
int pfd_swap_exchange = 0;
int pfd_swap_copy = 0;

/* Savers set this themselves in setDefaults(); the host paces frames, so nothing reads it. */
unsigned int dFrameRateLimit = 60;

/* The on-screen frame-rate overlay: debug furniture for a wallpaper, and rsText is a stub. */
int kStatistics = 0;

void *xdisplay = nullptr;
unsigned long xwindow = 0;

/* ---- ARB entry points Hyperspace expects the platform GL to export ----
 *
 * See compat/arb_shaders.h. Only glActiveTextureARB is real: it is core in GLES2. The
 * shader-object ones stay null because the shader path they belong to is never taken, and a
 * null here is a crash rather than silent wrong output if that assumption ever breaks. */
#include "arb_shaders.h"

PFNGLACTIVETEXTUREARBPROC glActiveTextureARB = glActiveTexture;
PFNGLCREATESHADEROBJECTARBPROC glCreateShaderObjectARB = nullptr;
PFNGLSHADERSOURCEARBPROC glShaderSourceARB = nullptr;
PFNGLCOMPILESHADERARBPROC glCompileShaderARB = nullptr;
PFNGLCREATEPROGRAMOBJECTARBPROC glCreateProgramObjectARB = nullptr;
PFNGLATTACHOBJECTARBPROC glAttachObjectARB = nullptr;
PFNGLLINKPROGRAMARBPROC glLinkProgramARB = nullptr;
PFNGLUSEPROGRAMOBJECTARBPROC glUseProgramObjectARB = nullptr;
PFNGLGETUNIFORMLOCATIONARBPROC glGetUniformLocationARB = nullptr;
PFNGLUNIFORM1IARBPROC glUniform1iARB = nullptr;
