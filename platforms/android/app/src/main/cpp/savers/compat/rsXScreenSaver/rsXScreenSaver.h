/*
 * Stands in for the X11 screensaver shell (upstream rsXScreenSaver, LGPL-2.1).
 *
 * The savers carry three platform paths: WIN32, RS_XSCREENSAVER, and nothing. The X11 one is
 * closest to what a background needs -- it exposes initSaver(), reshape(), draw(), idleProc()
 * and cleanUp() as plain functions and leaves the whole Win32 dialog shell out -- so we compile
 * with RS_XSCREENSAVER defined and answer what that path expects here.
 *
 * The point of doing it this way is that the saver sources stay BYTE-IDENTICAL to upstream.
 * Nothing is patched, so they can be re-pulled, and they stay recognisably Terry Welsh's code,
 * which their GPL headers ask us to keep intact.
 *
 * Declarations mirror upstream's types exactly -- note these flags are int, not bool.
 */
#ifndef SAVERS_COMPAT_RSXSCREENSAVER_H
#define SAVERS_COMPAT_RSXSCREENSAVER_H

#include <string>
#include <vector>

#include "rsUtility/rsTimer.h"

/* The savers poll these before drawing. The host view stops calling us when it is not visible
 * rather than flagging suspension, so they stay 0. */
extern int checkingPassword;
extern int isSuspended;
extern int doingPreview;

/* Pixel-format bits the X11 host reported; meaningless here and read only by savers deciding
 * whether they can rely on buffer preservation. */
extern int pfd_swap_exchange;
extern int pfd_swap_copy;

extern unsigned int dFrameRateLimit;

/* The on-screen frame-rate overlay. Left off -- it is debug furniture for a wallpaper. */
extern int kStatistics;

/* GLX buffer swapping is the host view's job: it owns the EGL surface. */
extern void *xdisplay;
extern unsigned long xwindow;
#define glXSwapBuffers(dpy, win) ((void) 0)

/* The savers build their frame-rate string with a bare to_string(); upstream picks it up from
 * this header's include chain. */
using std::to_string;

/* Command-line parsing has no meaning here -- settings arrive through each saver's port API,
 * which calls setDefaults() and then assigns the globals directly. Accepting and ignoring the
 * call keeps handleCommandLine() compiling unmodified. */
inline int getArgumentsValue(int, char **, std::string, std::string &) { return 0; }
template <typename T>
inline int getArgumentsValue(int, char **, std::string, T &) { return 0; }
template <typename T>
inline int getArgumentsValue(int, char **, std::string, T &, T, T) { return 0; }

#endif
