/* Compiles solarWinds.cpp inside a namespace. See flux/flux_unit.cpp for why. */

#include <rsXScreenSaver/rsXScreenSaver.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string>
#include <rsText/rsText.h>
#include <rsMath/rsMath.h>
#include <GL/gl.h>
#include <GL/glu.h>

namespace saver_solarwinds {
#include "solarWinds.cpp"
}
