/*
 * Compiles flux.cpp inside a namespace.
 *
 * Every Really Slick saver declares the same global names -- draw(), idleProc(), cleanUp(),
 * setDefaults(), readyToDraw, aspectRatio, dSize, dBlur -- because each was built as its own
 * executable. Two of them in one .so collide at link time.
 *
 * The headers are pulled in FIRST, at global scope, so that the copies inside the namespace hit
 * their include guards and expand to nothing. flux.cpp itself is then included unchanged, which
 * is the point: it stays byte-identical to upstream and can be re-pulled without re-patching.
 */

#include <rsXScreenSaver/rsXScreenSaver.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <rsText/rsText.h>
#include <rsMath/rsMath.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <Rgbhsl/Rgbhsl.h>

namespace saver_flux {
#include "flux.cpp"
}
