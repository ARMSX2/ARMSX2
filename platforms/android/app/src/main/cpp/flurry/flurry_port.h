/*
 * The Android environment for Calum Robinson's Flurry.
 *
 * Upstream this comes from xscreensaver: xlockmoreI.h pulls in the GL headers and the ModeInfo
 * plumbing, yarandom.h supplies the RNG. Neither exists here, and the parts Flurry actually
 * uses are small enough to state directly.
 */
#ifndef FLURRY_PORT_H
#define FLURRY_PORT_H

#include <stdlib.h>
#include <math.h>
#include <sys/time.h>   /* gettimeofday, via xlockmoreI.h upstream */

#include "gl_compat.h"

/* Normally set by xscreensaver's configure. Bionic's gettimeofday takes two arguments like
 * every other POSIX system, so the one-argument branch in currentTime() would not compile. */
#define GETTIMEOFDAY_TWO_ARGS 1

/* xscreensaver's yarandom.h: a float in [0, f). random() is bionic's, seeded in flurry_jni.c
 * -- the smoke is driven entirely by it, so an unseeded run would look identical every time. */
#ifndef frand
#define frand(f) ((float) ((double) (f) * ((double) random() / ((double) RAND_MAX + 1.0))))
#endif

#endif /* FLURRY_PORT_H */
