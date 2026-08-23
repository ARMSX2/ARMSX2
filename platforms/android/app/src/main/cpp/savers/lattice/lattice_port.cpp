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

void lattice_port_free();  /* defined below; port_new tears down a stale run */

int lattice_port_new(int preset)
{
    /* NOT "return 1". g_started can only be set here if a previous run was never
     * freed, and nativeInit has already called gl1_lost() -- so gl1 is DOWN, and
     * reporting success would hand the caller a saver with no shim under it. Tear the
     * stale run down and start clean. */
    if (g_started) lattice_port_free();
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
    /* NOT "if (!g_started) return": this saver waits for a surface size before it initialises,
     * so it can be created and torn down having never started. See the note below. */
    if (g_started) {
        saver_lattice::cleanUp();
        saver_lattice::readyToDraw = 0;
        g_started = false;
    }
    /* gl1_init() ran in port_new, and everything it holds -- the shader program, the vertex
     * buffers -- belongs to the EGL context that is about to be destroyed. Returning without
     * gl1_shutdown() leaves gl1's g.ready set with GL names from a DEAD context, and gl1_init()
     * early-returns on g.ready. The next saver, in a NEW context, would then run against those
     * dead names: undefined behaviour that some drivers answer with a segfault rather than a GL
     * error, which takes the whole app down. So gl1 is torn down whether or not this saver's own
     * init ever got as far as running. gl1_shutdown() is idempotent. */
    gl1_shutdown();
}

}  /* extern "C" */
