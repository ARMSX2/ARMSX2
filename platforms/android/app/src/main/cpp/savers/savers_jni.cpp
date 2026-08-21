/*
 * JNI for the Really Slick Screensavers.
 *
 * One entry point per lifecycle stage, dispatched through a table so that adding a saver is
 * adding a row plus its port file -- nothing here has to change shape.
 */

#include <jni.h>
#include <android/log.h>
#include <cstddef>
#include <mutex>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Savers", __VA_ARGS__)

#define SAVER_DECL(name)                    \
    int  name##_port_new(int preset);       \
    void name##_port_resize(int, int);      \
    void name##_port_draw();                \
    void name##_port_free();

extern "C" {
SAVER_DECL(flux)
SAVER_DECL(plasma)
SAVER_DECL(solarwinds)
SAVER_DECL(hyperspace)
SAVER_DECL(lattice)
SAVER_DECL(skyrocket)
}

#define SAVER_ROW(str, name) \
    { str, name##_port_new, name##_port_resize, name##_port_draw, name##_port_free }

namespace {

struct saver {
    const char *name;
    int  (*create)(int preset);
    void (*resize)(int, int);
    void (*draw)();
    void (*destroy)();
};

/* Index is the effect id passed from Kotlin. */
const saver k_savers[] = {
    SAVER_ROW("flux", flux),
    SAVER_ROW("plasma", plasma),
    SAVER_ROW("solarwinds", solarwinds),
    SAVER_ROW("hyperspace", hyperspace),
    SAVER_ROW("lattice", lattice),
    SAVER_ROW("skyrocket", skyrocket),
};
const int k_count = (int) (sizeof(k_savers) / sizeof(k_savers[0]));

const saver *g_active = nullptr;

/* Switching saver or preset tears down one view and builds another, and each view owns its own
 * GL thread. Without serialising, the OUTGOING thread's teardown can land after the INCOMING
 * thread's init and wipe the new saver's state -- gl1 keeps its state in one global, so the
 * second saver then draws nothing. That is why the first selection worked and every change
 * after it did not.
 *
 * The generation counter is what makes teardown safe: a view may only free what it started. A
 * late free from the old thread finds its generation stale and does nothing. */
std::mutex g_lock;
int g_generation = 0;

}  // namespace

extern "C" {

/* Returns the generation to pass back to nativeFree, or 0 if the saver did not start. */
JNIEXPORT jint JNICALL
Java_com_armsx2_ui_home_SaverNative_nativeInit(JNIEnv *, jobject, jint effect, jint preset)
{
    if (effect < 0 || effect >= k_count) {
        LOGE("unknown saver id %d", effect);
        return 0;
    }

    std::lock_guard<std::mutex> guard(g_lock);

    /* A previous saver may still be up if the outgoing view has not torn down yet. */
    if (g_active) g_active->destroy();

    g_active = &k_savers[effect];
    if (!g_active->create(preset)) {
        LOGE("%s failed to start (preset %d)", g_active->name, preset);
        g_active = nullptr;
        return 0;
    }
    return ++g_generation;
}

JNIEXPORT void JNICALL
Java_com_armsx2_ui_home_SaverNative_nativeResize(JNIEnv *, jobject, jint gen, jint w, jint h)
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_active && gen == g_generation) g_active->resize(w, h);
}

JNIEXPORT void JNICALL
Java_com_armsx2_ui_home_SaverNative_nativeDraw(JNIEnv *, jobject, jint gen)
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_active && gen == g_generation) g_active->draw();
}

JNIEXPORT void JNICALL
Java_com_armsx2_ui_home_SaverNative_nativeFree(JNIEnv *, jobject, jint gen)
{
    std::lock_guard<std::mutex> guard(g_lock);
    /* Only tear down what this view actually started. */
    if (!g_active || gen != g_generation) return;
    g_active->destroy();
    g_active = nullptr;
}

}  // extern "C"
