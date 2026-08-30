/*
 * JNI shim between the wallpaper's GL thread and Flurry.
 *
 * Every function here must be called with the EGL context current -- the Kotlin side owns the
 * context and guarantees that. Nothing is thread-safe beyond that assumption, which is fine
 * because a wallpaper engine has exactly one GL thread.
 */
#include <jni.h>
#include <android/log.h>
#include <stdlib.h>
#include <time.h>

#include "gl_compat.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "Flurry", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Flurry", __VA_ARGS__)

typedef struct _global_info_t global_info_t;

global_info_t *flurry_port_new(int preset);
global_info_t *flurry_port_new_custom(int streams, int colour, float thickness,
                                      float speed, float brightness);
void flurry_port_resize(global_info_t *global, int width, int height);
int  flurry_port_draw(global_info_t *global);
void flurry_port_free(global_info_t *global);

#define NS(name) Java_com_armsx2_ui_home_FlurryNative_##name

JNIEXPORT jlong JNICALL NS(nativeCreate)(JNIEnv *env, jclass clazz, jint preset)
{
    (void) env; (void) clazz;

    /* Flurry's entire look comes out of frand(); an unseeded run would be identical every
     * time the wallpaper restarts, which on a phone is many times a day. */
    srandom((unsigned) time(NULL) ^ (unsigned) clock());

    if (!fx_init()) {
        LOGE("GL compatibility layer failed to initialise");
        return 0;
    }

    global_info_t *g = flurry_port_new(preset);
    if (!g) {
        LOGE("flurry_port_new failed");
        return 0;
    }

    LOGI("Flurry started, preset %d", preset);
    return (jlong) (intptr_t) g;
}

JNIEXPORT jlong JNICALL NS(nativeCreateCustom)(JNIEnv *env, jclass clazz, jint streams,
                                               jint colour, jfloat thickness, jfloat speed,
                                               jfloat brightness)
{
    (void) env; (void) clazz;

    srandom((unsigned) time(NULL) ^ (unsigned) clock());

    if (!fx_init()) {
        LOGE("GL compatibility layer failed to initialise");
        return 0;
    }

    global_info_t *g = flurry_port_new_custom(streams, colour, thickness, speed, brightness);
    if (!g) {
        LOGE("flurry_port_new_custom failed");
        return 0;
    }

    LOGI("Flurry started, custom: streams=%d colour=%d thickness=%.2f speed=%.2f brightness=%.2f",
         streams, colour, (double) thickness, (double) speed, (double) brightness);
    return (jlong) (intptr_t) g;
}

JNIEXPORT void JNICALL NS(nativeResize)(JNIEnv *env, jclass clazz, jlong handle, jint w, jint h)
{
    (void) env; (void) clazz;
    if (handle) flurry_port_resize((global_info_t *) (intptr_t) handle, w, h);
}

JNIEXPORT jboolean JNICALL NS(nativeDraw)(JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    if (!handle) return JNI_FALSE;
    return flurry_port_draw((global_info_t *) (intptr_t) handle) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL NS(nativeDestroy)(JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    if (!handle) return;

    flurry_port_free((global_info_t *) (intptr_t) handle);
    fx_shutdown();
}

/*
 * The context went away without us being able to tidy up -- the usual case when a wallpaper is
 * torn down. Forget the GL names instead of deleting them: the objects are already gone with
 * the context, and deleting a name in a context that no longer exists is undefined.
 */
JNIEXPORT void JNICALL NS(nativeContextLost)(JNIEnv *env, jclass clazz, jlong handle)
{
    (void) env; (void) clazz;
    if (handle) flurry_port_free((global_info_t *) (intptr_t) handle);
    fx_lost();
}
