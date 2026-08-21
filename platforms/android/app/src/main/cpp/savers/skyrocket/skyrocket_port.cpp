/*
 * Android entry points for Skyrocket.
 *
 * Audio is off. Upstream drives OpenAL and bakes roughly 7MB of firework samples into headers;
 * dSound = 0 leaves soundengine null, every call site is already guarded, and neither the sound
 * engine nor the samples are built. See soundEngine.h.
 */

#include "gl1.h"

namespace saver_skyrocket {
void setDefaults();
void initSaver(int surfaceWidth, int surfaceHeight);
void reshape();
void idleProc();
void cleanup();
extern int readyToDraw;
extern int dSound;
extern int xsize, ysize, centerx, centery;
extern float aspectRatio;
}

namespace {
bool g_started = false;
}

extern "C" {

int skyrocket_port_new(int preset)
{
    (void) preset;  /* No presets upstream; every knob was a registry value. */
    if (g_started) return 1;
    if (!gl1_init()) return 0;

    saver_skyrocket::setDefaults();
    saver_skyrocket::dSound = 0;
    return 1;  /* initSaver waits for a surface size, as Lattice's does. */
}

void skyrocket_port_resize(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    if (!g_started) {
        saver_skyrocket::initSaver(width, height);
        /* Left to the Win32 shell upstream. */
        saver_skyrocket::readyToDraw = 1;
        g_started = true;
        return;
    }

    /* Skyrocket does have a real reshape(); it reads these globals rather than taking
     * arguments. */
    saver_skyrocket::xsize = width;
    saver_skyrocket::ysize = height;
    saver_skyrocket::centerx = width / 2;
    saver_skyrocket::centery = height / 2;
    saver_skyrocket::aspectRatio = float(width) / float(height);
    glViewport(0, 0, width, height);
    saver_skyrocket::reshape();
}

void skyrocket_port_draw()
{
    if (!g_started) return;
    gl1_frame_begin();
    saver_skyrocket::idleProc();
}

void skyrocket_port_free()
{
    if (!g_started) return;
    saver_skyrocket::cleanup();
    gl1_shutdown();
    saver_skyrocket::readyToDraw = 0;
    g_started = false;
}

}  /* extern "C" */
