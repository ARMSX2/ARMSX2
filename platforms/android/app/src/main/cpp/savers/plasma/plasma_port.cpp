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

int plasma_port_new(int preset)
{
    if (g_started) return 1;
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
    return g_started ? 1 : 0;
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
    if (!g_started) return;
    saver_plasma::cleanUp();
    gl1_shutdown();
    saver_plasma::readyToDraw = 0;
    g_started = false;
}

}
