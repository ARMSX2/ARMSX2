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

/* preset is 1..6 from the UI. Upstream's DEFAULTS1..DEFAULTS6 is a zero-based ENUM here, not
 * the 1-based #defines Flux uses, so the UI value is shifted down. */
int solarwinds_port_new(int preset)
{
    if (g_started) return 1;
    if (!gl1_init()) return 0;

    if (preset < 1 || preset > 6) preset = 1;
    saver_solarwinds::setDefaults(preset - 1);

    saver_solarwinds::initSaver();
    g_started = saver_solarwinds::readyToDraw != 0;
    return g_started ? 1 : 0;
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
    if (!g_started) return;
    saver_solarwinds::cleanUp();
    gl1_shutdown();
    saver_solarwinds::readyToDraw = 0;
    g_started = false;
}

}
