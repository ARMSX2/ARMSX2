/* Compiles lattice.cpp and camera.cpp inside a namespace. See flux/flux_unit.cpp for why. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string>
#include <rsXScreenSaver/rsXScreenSaver.h>
#include <rsText/rsText.h>
#include <rsMath/rsMath.h>
#include <GL/gl.h>
#include <GL/glu.h>

/* Supplied by rsWin32Saver.h upstream, which this port does not use. The values match the
 * RS_XSCREENSAVER path in the savers that have one (flux.cpp:87), so preset numbering is
 * consistent across all of them. */
#define DEFAULTS1 1
#define DEFAULTS2 2
#define DEFAULTS3 3
#define DEFAULTS4 4
#define DEFAULTS5 5
#define DEFAULTS6 6

/* Lattice declares two of its settings BOOL and assigns TRUE/FALSE to them. */
typedef int BOOL;
#define TRUE 1
#define FALSE 0

namespace saver_lattice {
#include "camera.cpp"
#include "lattice.cpp"
}
