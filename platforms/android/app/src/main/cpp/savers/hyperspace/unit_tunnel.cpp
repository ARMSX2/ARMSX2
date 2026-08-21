/* Compiles tunnel.cpp inside namespace saver_hyperspace. See flux/flux_unit.cpp for why.
 *
 * Only shared headers are pre-included here. Hyperspace's OWN headers must be left to be
 * included from inside the namespace, or their declarations would land at global scope while
 * the definitions land in the namespace, and nothing would match. */
#include <rsXScreenSaver/rsXScreenSaver.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <string>
#include <vector>
#include <list>
#include <deque>
#include <map>
#include <algorithm>
#include <iterator>
#include <iostream>
#include <fstream>
#include <sstream>
#include <rsText/rsText.h>
#include <rsMath/rsMath.h>
#include <Rgbhsl/Rgbhsl.h>
#include <arb_shaders.h>
/* Implicit is a shared library compiled at global scope, so its headers must be seen out here.
 * Included inside the namespace they would declare namespaced classes whose definitions do not
 * exist, which shows up only at link time. */
#include <Implicit/impCapsule.h>
#include <Implicit/impCrawlPoint.h>
#include <Implicit/impCubeData.h>
#include <Implicit/impCubeTables.h>
#include <Implicit/impCubeVolume.h>
#include <Implicit/impEllipsoid.h>
#include <Implicit/impHexahedron.h>
#include <Implicit/impKnot.h>
#include <Implicit/impRoundedHexahedron.h>
#include <Implicit/impShape.h>
#include <Implicit/impSphere.h>
#include <Implicit/impSurface.h>
#include <Implicit/impTorus.h>
#include <GL/gl.h>
#include <GL/glu.h>

namespace saver_hyperspace {
#include "tunnel.cpp"
}
