/*
 * Android entry points for Hyperspace. hyperspace.cpp and its nine companions are
 * byte-identical to upstream.
 *
 * Hyperspace has two rendering paths and chooses with dShaders. The shader one uses ARB
 * shader objects, which GLES2 does not have; the port forces the other, which is a supported
 * upstream configuration rather than something improvised -- upstream exposes it as -shaders 0
 * and falls back to it on any card lacking the extensions.
 */

#include "gl1.h"

namespace saver_hyperspace {
void setDefaults();
void initSaver();
void reshape(int width, int height);
void idleProc();
void cleanUp();
extern int readyToDraw;
extern int dShaders;
}

namespace { bool g_started = false; }

extern "C" {

void hyperspace_port_free();  /* defined below; port_new tears down a stale run */

int hyperspace_port_new(int preset)
{
    (void) preset;  /* No presets upstream; every knob was a registry value. */
    /* NOT "return 1". g_started can only be set here if a previous run was never
     * freed, and nativeInit has already called gl1_lost() -- so gl1 is DOWN, and
     * reporting success would hand the caller a saver with no shim under it. Tear the
     * stale run down and start clean. */
    if (g_started) hyperspace_port_free();
    if (!gl1_init()) return 0;

    saver_hyperspace::setDefaults();

    /* Must be off BEFORE initSaver: with it on, initSaver builds the wavy normal cube maps and
     * the draw path then calls ARB entry points that are null here. */
    saver_hyperspace::dShaders = 0;

    saver_hyperspace::initSaver();

    /* Like Helios, Hyperspace leaves this to its Win32 shell. Unlike Helios, that is all it
     * leaves out. */
    saver_hyperspace::readyToDraw = 1;
    g_started = true;
    return 1;
}

void hyperspace_port_resize(int width, int height)
{
    if (width > 0 && height > 0) saver_hyperspace::reshape(width, height);
}

void hyperspace_port_draw()
{
    if (!g_started) return;
    gl1_frame_begin();
    saver_hyperspace::idleProc();
}

void hyperspace_port_free()
{
    if (g_started) {
        saver_hyperspace::cleanUp();
        saver_hyperspace::readyToDraw = 0;
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

}  /* extern "C" */
