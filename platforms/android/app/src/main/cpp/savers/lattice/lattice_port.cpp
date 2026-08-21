/*
 * Android entry points for Lattice.
 *
 * Lattice has no reshape(): it builds its projection once inside initSaver and hands the matrix
 * to its camera. So initialisation is deferred until the first resize, when the surface size is
 * actually known, rather than guessed at construction time.
 */

#include "gl1.h"

namespace saver_lattice {
void setDefaults(int which);
void initSaver(int surfaceWidth, int surfaceHeight);
void idleProc();
void cleanUp();
extern int readyToDraw;
}

namespace {
bool g_started = false;
int  g_preset = 1;
}

extern "C" {

int lattice_port_new(int preset)
{
    if (g_started) return 1;
    if (!gl1_init()) return 0;

    g_preset = (preset >= 1 && preset <= 6) ? preset : 1;
    saver_lattice::setDefaults(g_preset);
    return 1;  /* The real work waits for a surface size. */
}

void lattice_port_resize(int width, int height)
{
    if (width <= 0 || height <= 0 || g_started) return;

    saver_lattice::initSaver(width, height);

    /* Lattice leaves this to its Win32 shell, as Helios and Hyperspace do. */
    saver_lattice::readyToDraw = 1;
    g_started = true;
}

void lattice_port_draw()
{
    if (!g_started) return;
    gl1_frame_begin();
    saver_lattice::idleProc();
}

void lattice_port_free()
{
    if (!g_started) return;
    saver_lattice::cleanUp();
    gl1_shutdown();
    saver_lattice::readyToDraw = 0;
    g_started = false;
}

}  /* extern "C" */
