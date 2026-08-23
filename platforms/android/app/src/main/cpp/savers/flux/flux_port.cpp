/*
 * Android entry points for Flux.
 *
 * flux.cpp is upstream, byte-identical. flux_unit.cpp compiles it inside a namespace, with
 * RS_XSCREENSAVER defined so it takes its platform-neutral path -- initSaver(), reshape(),
 * idleProc(), cleanUp() as plain functions, and no Win32 shell. Everything that path expects
 * from an X11 host is answered by compat/rsXScreenSaver/rsXScreenSaver.h.
 */

#include "gl1.h"

namespace saver_flux {
void setDefaults(int which);
void initSaver();
void reshape(int width, int height);
void idleProc();
void cleanUp();
extern int readyToDraw;
extern int dGeometry;
}

namespace { bool g_started = false; }

extern "C" {

/* preset is 1..6, matching the saver's own DEFAULTS1..DEFAULTS6. */
int flux_port_new(int preset)
{
    if (g_started) return 1;
    if (!gl1_init()) return 0;

    if (preset < 1 || preset > 6) preset = 1;
    saver_flux::setDefaults(preset);

    /* Sphere geometry needs fixed-function lighting, which the shim does not emulate -- the
     * spheres would draw flat and unlit, which looks worse than the alternative rather than
     * merely different. Points and the textured "lights" need no lighting, so fall back to the
     * lights, which is the mode the effect is known for anyway. */
    if (saver_flux::dGeometry == 1) saver_flux::dGeometry = 2;

    saver_flux::initSaver();
    g_started = saver_flux::readyToDraw != 0;
    if (!g_started) {
        /* Returning 0 means the JNI never calls port_free, so this is the only chance to give
         * gl1 back. Leaving it up would strand g.ready with names from a context that is about
         * to die, and gl1_init() early-returns on g.ready -- poisoning the NEXT saver. */
        gl1_shutdown();
        return 0;
    }
    return 1;
}

void flux_port_resize(int width, int height)
{
    if (width > 0 && height > 0) saver_flux::reshape(width, height);
}

/* idleProc() does the frame timing itself and calls draw(). */
void flux_port_draw()
{
    if (!g_started) return;
    gl1_frame_begin();
    saver_flux::idleProc();
}

void flux_port_free()
{
    if (!g_started) return;
    saver_flux::cleanUp();
    gl1_shutdown();
    saver_flux::readyToDraw = 0;
    g_started = false;
}

}  /* extern "C" */
