/* Compiles shockwave.cpp inside namespace saver_skyrocket. See flux/flux_unit.cpp for why. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <string>
#include <rsXScreenSaver/rsXScreenSaver.h>
#include <rsText/rsText.h>
#include <rsMath/rsMath.h>
#include <GL/gl.h>
#include <GL/glu.h>

/* Supplied by rsWin32Saver.h upstream; values match the RS_XSCREENSAVER savers. */
#define DEFAULTS1 1
#define DEFAULTS2 2
#define DEFAULTS3 3
#define DEFAULTS4 4
#define DEFAULTS5 5
#define DEFAULTS6 6
typedef int BOOL;
/* windows.h supplies these as macros, and the savers use them unqualified. */
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

namespace saver_skyrocket {
#include "shockwave.cpp"
}
