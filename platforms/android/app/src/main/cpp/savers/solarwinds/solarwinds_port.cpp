/* Android entry points for SolarWinds. solarWinds.cpp is byte-identical to upstream. */

#include "gl1.h"

namespace saver_solarwinds {
void setDefaults(int which);
void initSaver();
void reshape(int width, int height);
void idleProc();
void cleanUp();
extern int readyToDraw;
}

namespace { bool g_started = false; }

extern "C" {

void solarwinds_port_free();  /* defined below; port_new tears down a stale run */

/* preset is 1..6 from the UI. Upstream's DEFAULTS1..DEFAULTS6 is a zero-based ENUM here, not
 * the 1-based #defines Flux uses, so the UI value is shifted down. */
int solarwinds_port_new(int preset)
{
    /* NOT "return 1". g_started can only be set here if a previous run was never
     * freed, and nativeInit has already called gl1_lost() -- so gl1 is DOWN, and
     * reporting success would hand the caller a saver with no shim under it. Tear the
     * stale run down and start clean. */
    if (g_started) solarwinds_port_free();
    if (!gl1_init()) return 0;

    if (preset < 1 || preset > 6) preset = 1;
    saver_solarwinds::setDefaults(preset - 1);

    saver_solarwinds::initSaver();
    g_started = saver_solarwinds::readyToDraw != 0;
    if (!g_started) {
        /* Returning 0 means the JNI never calls port_free, so this is the only chance to give
         * gl1 back. Leaving it up would strand g.ready with names from a context that is about
         * to die, and gl1_init() early-returns on g.ready -- poisoning the NEXT saver. */
        gl1_shutdown();
        return 0;
    }
    return 1;
}

void solarwinds_port_resize(int width, int height)
{
    if (width > 0 && height > 0) saver_solarwinds::reshape(width, height);
}

void solarwinds_port_draw()
{
    if (!g_started) return;
    gl1_frame_begin();
    saver_solarwinds::idleProc();

}

void solarwinds_port_free()
{
    if (g_started) {
        saver_solarwinds::cleanUp();
        saver_solarwinds::readyToDraw = 0;
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
