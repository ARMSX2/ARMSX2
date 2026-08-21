package com.armsx2.ui.home

import android.content.Context
import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.util.Log
import android.view.TextureView

/**
 * Calum Robinson's Flurry (2002) as a library background.
 *
 * The renderer is his, built as libflurry.so; this is the same TextureView + EGL shell
 * [XmbGlView] uses, so the two backgrounds behave identically to HomeScreen -- including
 * reporting through [onGlStatus] so a device that cannot bring GL up falls back to the 2D
 * backdrop instead of showing a hole.
 *
 * Flurry is BSD-3-clause; see app/src/main/cpp/flurry for the notice and for what the GLES2
 * compatibility layer does and does not emulate.
 */
class FlurryGlView(context: Context, private val preset: Int) :
    TextureView(context), TextureView.SurfaceTextureListener {

    private var thread: RenderThread? = null

    /** True once a frame has presented, false if EGL or Flurry's own init failed. Main thread. */
    var onGlStatus: ((Boolean) -> Unit)? = null

    init {
        surfaceTextureListener = this

        // Opaque, unlike XmbGlView.
        //
        // That view can afford isOpaque = false because, as its own comment says, it "draws an
        // opaque gradient every frame, fully covering that layer". Flurry does the opposite: it
        // never draws anything opaque, it fades the screen toward black at a few percent per
        // frame, which is what produces the trails. So the surface's alpha channel stays low,
        // and a non-opaque TextureView with low alpha composites whatever is behind it -- which
        // showed as full-screen static over the top of the smoke.
        isOpaque = true
    }

    override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
        thread = RenderThread(st, w, h, preset) { ok -> post { onGlStatus?.invoke(ok) } }
            .also { it.start() }
    }

    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
        thread?.resize(w, h)
    }

    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
        thread?.finish()
        thread = null
        return true
    }

    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}

    /**
     * Stop rendering without waiting for the surface callback.
     *
     * Toggling the setting recomposes the AndroidView, and the replacement view's thread can be
     * started before the old one's surface is destroyed -- two particle simulations at once,
     * which was visible as UI lag. AndroidView's onRelease calls this so teardown is tied to the
     * composable leaving rather than to a callback that arrives whenever it arrives.
     */
    fun stop() {
        thread?.finish()
        thread = null
    }

    private class RenderThread(
        private val surfaceTexture: SurfaceTexture,
        private var width: Int,
        private var height: Int,
        private val preset: Int,
        private val onStatus: (Boolean) -> Unit,
    ) : Thread("flurry-gl") {

        @Volatile private var running = true
        @Volatile private var sizeDirty = true

        private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
        private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
        private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE

        private var handle = 0L

        fun resize(w: Int, h: Int) { width = w; height = h; sizeDirty = true }
        fun finish() { running = false; runCatching { join(500) } }

        override fun run() {
            if (!initEgl()) { onStatus(false); teardown(); return }

            handle = runCatching { FlurryNative.nativeCreate(preset) }.getOrDefault(0L)
            if (handle == 0L) {
                Log.w(TAG, "Flurry failed to start")
                onStatus(false); teardown(); return
            }

            var announced = false
            while (running) {
                val frameStart = System.nanoTime()

                if (sizeDirty) {
                    FlurryNative.nativeResize(handle, width, height)
                    sizeDirty = false
                }

                runCatching { FlurryNative.nativeDraw(handle) }

                if (!EGL14.eglSwapBuffers(eglDisplay, eglSurface)) running = false
                else if (!announced) { announced = true; onStatus(true) }

                // Flurry already refuses to advance faster than 60fps -- above that its additive
                // blending saturates into a white smear -- but without a cap here the loop would
                // still spin at panel rate and do the swap anyway. Same reason XmbGlView caps
                // itself: on a handheld that difference is audible in the fans.
                val leftMs = FRAME_TARGET_MS - (System.nanoTime() - frameStart) / 1_000_000L
                if (leftMs > 1) runCatching { sleep(leftMs) }
            }

            if (handle != 0L) runCatching { FlurryNative.nativeDestroy(handle) }
            handle = 0L
            teardown()
        }

        private fun initEgl(): Boolean {
            // Not exposed by EGL14.
            val EGL_SWAP_BEHAVIOR_PRESERVED_BIT = 0x0400

            eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            if (eglDisplay == EGL14.EGL_NO_DISPLAY) return false

            val ver = IntArray(2)
            if (!EGL14.eglInitialize(eglDisplay, ver, 0, ver, 1)) return false

            // Flurry draws its trails by fading the PREVIOUS frame rather than clearing, so it
            // needs the back buffer to still hold what it drew last time. EGL defaults
            // EGL_SWAP_BEHAVIOR to EGL_BUFFER_DESTROYED, which leaves the buffer undefined after
            // every swap -- so each frame started from uninitialised GPU memory and the fade had
            // nothing coherent to work on. That undefined memory is what showed up on screen as
            // TV static. Measured: one fade darkened the buffer 3.17%, but sixty consecutive
            // fades only managed 6.5% total, because the result never survived the swap.
            //
            // Preservation has to be asked for in the CONFIG as well as on the surface, and not
            // every driver offers it, so fall back to a plain window config if it is refused.
            fun attribs(preserved: Boolean) = intArrayOf(
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE,
                if (preserved) EGL14.EGL_WINDOW_BIT or EGL_SWAP_BEHAVIOR_PRESERVED_BIT
                else EGL14.EGL_WINDOW_BIT,
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                // A real alpha channel, which the renderer clears to 1 and then masks off, so
                // the compositor always sees a fully opaque surface.
                EGL14.EGL_ALPHA_SIZE, 8,
                // No depth buffer: Flurry is 2D additive smoke, and asking for one on a tiler
                // costs bandwidth for something never read.
                EGL14.EGL_DEPTH_SIZE, 0,
                EGL14.EGL_NONE,
            )

            val cfg = arrayOfNulls<EGLConfig>(1)
            val num = IntArray(1)
            var preserved = EGL14.eglChooseConfig(eglDisplay, attribs(true), 0, cfg, 0, 1, num, 0) &&
                num[0] > 0
            if (!preserved &&
                (!EGL14.eglChooseConfig(eglDisplay, attribs(false), 0, cfg, 0, 1, num, 0) || num[0] == 0)
            ) {
                return false
            }

            val ctxAttr = intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE)
            eglContext = EGL14.eglCreateContext(eglDisplay, cfg[0], EGL14.EGL_NO_CONTEXT, ctxAttr, 0)
            if (eglContext == EGL14.EGL_NO_CONTEXT) return false

            eglSurface = EGL14.eglCreateWindowSurface(
                eglDisplay, cfg[0], surfaceTexture, intArrayOf(EGL14.EGL_NONE), 0,
            )
            if (eglSurface == EGL14.EGL_NO_SURFACE) return false

            if (!EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) return false

            // Asking for the config is not enough; the surface has to be told as well. Query it
            // back rather than trusting the request -- a driver may quietly refuse.
            if (preserved) {
                EGL14.eglSurfaceAttrib(
                    eglDisplay, eglSurface, EGL14.EGL_SWAP_BEHAVIOR, EGL14.EGL_BUFFER_PRESERVED,
                )
                val got = IntArray(1)
                preserved = EGL14.eglQuerySurface(
                    eglDisplay, eglSurface, EGL14.EGL_SWAP_BEHAVIOR, got, 0,
                ) && got[0] == EGL14.EGL_BUFFER_PRESERVED
            }
            android.util.Log.e(
                "Flurry",
                "swap behavior = " + (if (preserved) "PRESERVED" else "DESTROYED (trails will not accumulate)"),
            )
            return true
        }

        private fun teardown() {
            if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(
                    eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT,
                )
                if (eglSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(eglDisplay, eglSurface)
                if (eglContext != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(eglDisplay, eglContext)
                EGL14.eglTerminate(eglDisplay)
            }
            eglDisplay = EGL14.EGL_NO_DISPLAY
            eglContext = EGL14.EGL_NO_CONTEXT
            eglSurface = EGL14.EGL_NO_SURFACE
        }
    }

    companion object {
        private const val TAG = "FlurryGlView"
        private const val FRAME_TARGET_MS = 16L
    }
}

/** libflurry.so. Every call needs the EGL context current; RenderThread is the only caller. */
internal object FlurryNative {
    init { System.loadLibrary("flurry") }

    external fun nativeCreate(preset: Int): Long
    external fun nativeCreateCustom(
        streams: Int, colour: Int, thickness: Float, speed: Float, brightness: Float,
    ): Long
    external fun nativeResize(handle: Long, width: Int, height: Int)
    external fun nativeDraw(handle: Long): Boolean
    external fun nativeDestroy(handle: Long)
    external fun nativeContextLost(handle: Long)
}
