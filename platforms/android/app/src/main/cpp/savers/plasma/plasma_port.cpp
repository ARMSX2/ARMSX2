/* Android entry points for Plasma. plasma.cpp is byte-identical to upstream. */

#include "gl1.h"

namespace saver_plasma {
void setDefaults();
void initSaver();
void reshape(int width, int height);
void idleProc();
void cleanUp();
extern int readyToDraw;
extern int dZoom, dFocus, dSpeed, dResolution;
}

namespace { bool g_started = false; }

extern "C" {

void plasma_port_free();  /* defined below; port_new tears down a stale run */

int plasma_port_new(int preset)
{
    /* NOT "return 1". g_started can only be set here if a previous run was never
     * freed, and nativeInit has already called gl1_lost() -- so gl1 is DOWN, and
     * reporting success would hand the caller a saver with no shim under it. Tear the
     * stale run down and start clean. */
    if (g_started) plasma_port_free();
    if (!gl1_init()) return 0;

    saver_plasma::setDefaults();

    /* Plasma ships no preset list upstream -- every knob was a registry value -- so these are
     * ours, built from the settings its config dialog exposed. Confirmed working on device;
     * 1 is upstream's defaults untouched. */
    switch (preset) {
    case 2:  // Tight
        saver_plasma::dZoom = 25; saver_plasma::dFocus = 60; break;
    case 3:  // Wide
        saver_plasma::dZoom = 3; saver_plasma::dFocus = 12; break;
    case 4:  // Fast
        saver_plasma::dSpeed = 50; break;
    case 5:  // Slow drift
        saver_plasma::dSpeed = 6; break;
    case 6:  // Coarse, and cheapest to draw
        saver_plasma::dResolution = 12; saver_plasma::dSpeed = 25; break;
    default: break;
    }

    saver_plasma::initSaver();
    g_started = saver_plasma::readyToDraw != 0;
    if (!g_started) {
        /* Returning 0 means the JNI never calls port_free, so this is the only chance to give
         * gl1 back. Leaving it up would strand g.ready with names from a context that is about
         * to die, and gl1_init() early-returns on g.ready -- poisoning the NEXT saver. */
        gl1_shutdown();
        return 0;
    }
    return 1;
}

void plasma_port_resize(int width, int height)
{
    if (width > 0 && height > 0) saver_plasma::reshape(width, height);
}

void plasma_port_draw()
{
    if (!g_started) return;
    gl1_frame_begin();
    saver_plasma::idleProc();
}

void plasma_port_free()
{
    if (g_started) {
        saver_plasma::cleanUp();
        saver_plasma::readyToDraw = 0;
        g_started = false;
    }
    /* Unconditional, and NOT guarded by g_started. gl1_init() ran in port_new, and everything
     * it holds -- the shader program, the vertex buffers -- belongs to the EGL context that is
     * about to be destroyed. Leaving g.ready set with names from a dead context poisons the NEXT
     * saver, because gl1_init() early-returns on g.ready. That the JNI currently only calls this
     * after a successful create is a property of today's call graph, not of this function.
     * gl1_shutdown() is idempotent. */
    gl1_shutdown();
}

}
