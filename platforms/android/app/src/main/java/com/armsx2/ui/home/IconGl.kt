package com.armsx2.ui.home

import android.content.Context
import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES30
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.TextureView
import androidx.compose.runtime.mutableStateOf
import com.armsx2.memcard.MemcardCovers
import com.armsx2.memcard.Ps2Icon
import com.armsx2.memcard.Ps2IconRenderer
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin

/**
 * Moving PS2 save icons drawn by the GPU, the way ARMSX3 draws its PS3 themes.
 *
 * The icons used to be drawn in software on the CPU, every frame of every moving tile, which with a
 * screenful moving kept an Odin 3's fan going. Here one GL thread with one OpenGL ES 3 context draws
 * each moving icon into its own small [IconView] (a TextureView) on the shared beat, with a depth
 * buffer and 4x multisampling, and the CPU's part is a few uniforms and, for an animated icon, its
 * blended vertex positions.
 *
 * The maths is the software renderer's, moved into shaders: the same turn, camera tilt, framing
 * ([Ps2IconRenderer.framing], so the two agree to the pixel and the moving icon lands exactly on
 * the still picture under it), back-face culling, lighting (per vertex, as the software's) and
 * texture sampling (bilinear, wrapping). If the GPU path can't start, [available] turns false and
 * moving icons go back to the software renderer.
 */
internal object IconGl {
    private const val TAG = "IconGl"
    private const val FRAME_MS = 42L // about 24 frames a second, the software renderer's beat
    // A full turn every six seconds, the unhurried spin of the console's own browser.
    private const val TURN_PER_SECOND = (2.0 * PI / 6.0).toFloat()

    /** False once the GPU path has failed to start; moving icons then use the software renderer. */
    val available = mutableStateOf(true)

    private val handler: Handler by lazy { Handler(HandlerThread("icon-gl").apply { start() }.looper) }
    private val main = Handler(Looper.getMainLooper())

    // Everything below belongs to the GL thread.
    private var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var context: EGLContext = EGL14.EGL_NO_CONTEXT
    private var config: EGLConfig? = null
    private var idle: EGLSurface = EGL14.EGL_NO_SURFACE
    private var failed = false
    private var program = 0
    private var uYaw = 0; private var uPitch = 0; private var uCenter = 0; private var uFrame = 0
    private var uSize = 0; private var uDepth = 0; private var uLightMode = 0; private var uAmbient = 0
    private var uLightDir = 0; private var uLightColor = 0; private var uTex = 0

    private val views = LinkedHashSet<IconView>()
    private val meshes = HashMap<Ps2Icon, Mesh>()
    // Per key: seconds of animation and of turning so far. A tile and its shelf reflection share
    // a key, so they move as one.
    private val clocks = HashMap<Any, FloatArray>()
    private var ticking = false
    private var lastTick = 0L
    @Volatile private var visible = true

    /** Whether the app is on screen; nothing is drawn while it isn't. */
    fun setVisible(on: Boolean) {
        visible = on
        if (on) wake()
    }

    fun wake() {
        handler.post { startTicking() }
    }

    fun attach(view: IconView, texture: SurfaceTexture) {
        handler.post {
            views += view
            if (!ensureGl()) return@post
            val surface = EGL14.eglCreateWindowSurface(display, config, texture, intArrayOf(EGL14.EGL_NONE), 0)
            if (surface == null || surface == EGL14.EGL_NO_SURFACE) {
                Log.w(TAG, "eglCreateWindowSurface failed: 0x${Integer.toHexString(EGL14.eglGetError())}")
                return@post
            }
            // Never wait on the compositor: a view it hasn't drawn yet (offscreen, say) would
            // otherwise hold up every icon on this thread. The newest frame wins.
            if (EGL14.eglMakeCurrent(display, surface, surface, context)) {
                EGL14.eglSwapInterval(display, 0)
                EGL14.eglMakeCurrent(display, idle, idle, context)
            }
            view.eglSurface = surface
            view.forgetDrawn()
            startTicking()
        }
    }

    fun detach(view: IconView, texture: SurfaceTexture) {
        handler.post {
            views -= view
            if (view.eglSurface != EGL14.EGL_NO_SURFACE) {
                EGL14.eglMakeCurrent(display, idle, idle, context)
                EGL14.eglDestroySurface(display, view.eglSurface)
                view.eglSurface = EGL14.EGL_NO_SURFACE
            }
            texture.release()
            dropUnused()
        }
    }

    // ---- the beat -----------------------------------------------------------------------------

    private val tick = object : Runnable {
        override fun run() {
            ticking = false
            if (!visible || views.isEmpty() || failed) return
            val now = SystemClock.uptimeMillis()
            // After a pause the clocks carry on from where they were rather than jump.
            val dt = ((now - lastTick).coerceIn(0, 100)) / 1000f
            lastTick = now
            val spin = MemcardCovers.spin.value
            var moving = false
            val advanced = HashSet<Any>()
            for (v in views) {
                val key = v.key ?: continue
                if (v.animate && advanced.add(key)) {
                    val clock = clocks.getOrPut(key) { FloatArray(2) }
                    clock[0] += dt
                    clock[1] = if (spin) clock[1] + dt else 0f
                }
                if (v.animate) moving = true
            }
            for (v in views) draw(v)
            if (moving) schedule(now)
        }
    }

    private fun startTicking() {
        if (ticking || failed || !visible || views.isEmpty()) return
        lastTick = SystemClock.uptimeMillis()
        handler.post(tick)
        ticking = true
    }

    // Every moving icon draws on the same beat of one clock, so a screenful changes together and
    // the screen is redrawn once per beat.
    private fun schedule(now: Long) {
        ticking = true
        handler.postAtTime(tick, now - now % FRAME_MS + FRAME_MS)
    }

    // ---- drawing ------------------------------------------------------------------------------

    private fun draw(v: IconView) {
        val loaded = v.loaded ?: return
        val surface = v.eglSurface
        if (surface == EGL14.EGL_NO_SURFACE) return
        val w = v.surfaceWidth
        val h = v.surfaceHeight
        if (w <= 0 || h <= 0) return
        val icon = loaded.icon
        val clock = v.key?.let { clocks[it] }
        val animation = clock?.get(0) ?: 0f
        val turn = clock?.get(1) ?: 0f
        val yaw = loaded.pose.yaw + turn * TURN_PER_SECOND
        val length = icon.frameLength.coerceAtLeast(1).toFloat()
        val time = if (icon.animated) (loaded.pose.time + animation * 60f * icon.animSpeed) % length else loaded.pose.time
        // Nothing moved since the last frame drawn here: leave it on screen.
        if (v.drawnLoaded === loaded && v.drawnYaw == yaw && v.drawnTime == time && v.drawnW == w && v.drawnH == h) return

        if (!EGL14.eglMakeCurrent(display, surface, surface, context)) {
            Log.w(TAG, "eglMakeCurrent failed: 0x${Integer.toHexString(EGL14.eglGetError())}")
            return
        }
        val mesh = meshes.getOrPut(icon) { Mesh.create(icon) }
        mesh.upload(time)
        val options = v.options
        if (v.framingFor !== loaded || v.framingW != w || v.framingH != h || v.framingOptions != options) {
            v.framing = Ps2IconRenderer.framing(icon, w, h, options)
            v.framingFor = loaded
            v.framingW = w
            v.framingH = h
            v.framingOptions = options
        }
        val f = v.framing ?: return

        GLES30.glViewport(0, 0, w, h)
        GLES30.glClearColor(0f, 0f, 0f, 0f)
        GLES30.glClear(GLES30.GL_COLOR_BUFFER_BIT or GLES30.GL_DEPTH_BUFFER_BIT)
        GLES30.glUniform2f(uYaw, cos(yaw), sin(yaw))
        GLES30.glUniform2f(uPitch, cos(options.pitch), sin(options.pitch))
        GLES30.glUniform2f(uCenter, f.cx, f.cz)
        GLES30.glUniform3f(uFrame, f.scale, f.refY, f.baseY)
        GLES30.glUniform2f(uSize, w.toFloat(), h.toFloat())
        GLES30.glUniform1f(uDepth, f.depthRange)
        val sys = loaded.sys
        val floor = options.ambientFloor
        if (sys != null) {
            val amb = sys.ambient
            GLES30.glUniform1i(uLightMode, 1)
            GLES30.glUniform3f(uAmbient, max(amb[0], floor), max(amb[1], floor), max(amb[2], floor))
            val dirs = FloatArray(9)
            val colors = FloatArray(9)
            for (l in 0 until 3) for (c in 0 until 3) {
                dirs[l * 3 + c] = sys.lightDirections[l][c]
                colors[l * 3 + c] = sys.lightColors[l][c]
            }
            GLES30.glUniform3fv(uLightDir, 3, dirs, 0)
            GLES30.glUniform3fv(uLightColor, 3, colors, 0)
        } else {
            // No icon.sys: the software renderer's default ambient and one soft light from the camera.
            GLES30.glUniform1i(uLightMode, 0)
            GLES30.glUniform3f(uAmbient, max(0.55f, floor), max(0.55f, floor), max(0.55f, floor))
        }
        GLES30.glBindVertexArray(mesh.vao)
        GLES30.glActiveTexture(GLES30.GL_TEXTURE0)
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, mesh.texture)
        GLES30.glDrawArrays(GLES30.GL_TRIANGLES, 0, icon.vertexCount)
        GLES30.glBindVertexArray(0)
        if (!EGL14.eglSwapBuffers(display, surface)) {
            Log.w(TAG, "eglSwapBuffers failed: 0x${Integer.toHexString(EGL14.eglGetError())}")
            return
        }
        v.drawnLoaded = loaded
        v.drawnYaw = yaw
        v.drawnTime = time
        v.drawnW = w
        v.drawnH = h
    }

    /** GL objects of icons no view shows any more, and clocks no view uses. */
    private fun dropUnused() {
        val icons = views.mapNotNullTo(HashSet()) { it.loaded?.icon }
        val gone = meshes.keys.filter { it !in icons }
        if (gone.isNotEmpty() && context != EGL14.EGL_NO_CONTEXT) {
            EGL14.eglMakeCurrent(display, idle, idle, context)
            for (icon in gone) meshes.remove(icon)?.delete()
        }
        val keys = views.mapNotNullTo(HashSet()) { it.key }
        clocks.keys.retainAll(keys)
    }

    /** Dropping an icon a view no longer shows, when it is given another. */
    fun iconChanged() {
        handler.post { dropUnused() }
    }

    // ---- setup --------------------------------------------------------------------------------

    private fun ensureGl(): Boolean {
        if (failed) return false
        if (context != EGL14.EGL_NO_CONTEXT) return true
        try {
            display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            val version = IntArray(2)
            check(EGL14.eglInitialize(display, version, 0, version, 1)) { "eglInitialize" }
            config = chooseConfig(4) ?: chooseConfig(0) ?: error("no RGBA8888 ES3 config")
            context = EGL14.eglCreateContext(
                display, config, EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE), 0,
            )
            check(context != null && context != EGL14.EGL_NO_CONTEXT) { "eglCreateContext" }
            idle = EGL14.eglCreatePbufferSurface(display, config, intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0)
            check(EGL14.eglMakeCurrent(display, idle, idle, context)) { "eglMakeCurrent" }
            program = buildProgram()
            GLES30.glUseProgram(program)
            uYaw = GLES30.glGetUniformLocation(program, "uYaw")
            uPitch = GLES30.glGetUniformLocation(program, "uPitch")
            uCenter = GLES30.glGetUniformLocation(program, "uCenter")
            uFrame = GLES30.glGetUniformLocation(program, "uFrame")
            uSize = GLES30.glGetUniformLocation(program, "uSize")
            uDepth = GLES30.glGetUniformLocation(program, "uDepth")
            uLightMode = GLES30.glGetUniformLocation(program, "uLightMode")
            uAmbient = GLES30.glGetUniformLocation(program, "uAmbient")
            uLightDir = GLES30.glGetUniformLocation(program, "uLightDir")
            uLightColor = GLES30.glGetUniformLocation(program, "uLightColor")
            uTex = GLES30.glGetUniformLocation(program, "uTex")
            GLES30.glUniform1i(uTex, 0)
            // Nearer is less depth, as in the software renderer; a triangle clockwise on screen
            // is a back face, and GL's own front is counter-clockwise.
            GLES30.glEnable(GLES30.GL_DEPTH_TEST)
            GLES30.glDepthFunc(GLES30.GL_LESS)
            GLES30.glEnable(GLES30.GL_CULL_FACE)
            GLES30.glCullFace(GLES30.GL_BACK)
            GLES30.glFrontFace(GLES30.GL_CCW)
            return true
        } catch (e: Throwable) {
            Log.w(TAG, "GPU icons unavailable, using the software renderer", e)
            failed = true
            main.post { available.value = false }
            return false
        }
    }

    private fun chooseConfig(samples: Int): EGLConfig? {
        val attributes = intArrayOf(
            EGL14.EGL_RENDERABLE_TYPE, EGLExt.EGL_OPENGL_ES3_BIT_KHR,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT or EGL14.EGL_PBUFFER_BIT,
            EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_DEPTH_SIZE, 16,
            EGL14.EGL_SAMPLE_BUFFERS, if (samples > 0) 1 else 0,
            EGL14.EGL_SAMPLES, samples,
            EGL14.EGL_NONE,
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val count = IntArray(1)
        if (!EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) || count[0] == 0) return null
        return configs[0]
    }

    private fun buildProgram(): Int {
        fun shader(type: Int, source: String): Int {
            val s = GLES30.glCreateShader(type)
            GLES30.glShaderSource(s, source)
            GLES30.glCompileShader(s)
            val ok = IntArray(1)
            GLES30.glGetShaderiv(s, GLES30.GL_COMPILE_STATUS, ok, 0)
            check(ok[0] != 0) { "shader: " + GLES30.glGetShaderInfoLog(s) }
            return s
        }
        val p = GLES30.glCreateProgram()
        GLES30.glAttachShader(p, shader(GLES30.GL_VERTEX_SHADER, VERTEX))
        GLES30.glAttachShader(p, shader(GLES30.GL_FRAGMENT_SHADER, FRAGMENT))
        GLES30.glLinkProgram(p)
        val ok = IntArray(1)
        GLES30.glGetProgramiv(p, GLES30.GL_LINK_STATUS, ok, 0)
        check(ok[0] != 0) { "link: " + GLES30.glGetProgramInfoLog(p) }
        return p
    }

    // The software renderer's per-vertex maths (Ps2IconRenderer.draw), line for line.
    private const val VERTEX = """#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec3 aColor;
uniform vec2 uYaw;
uniform vec2 uPitch;
uniform vec2 uCenter;
uniform vec3 uFrame;
uniform vec2 uSize;
uniform float uDepth;
uniform int uLightMode;
uniform vec3 uAmbient;
uniform vec3 uLightDir[3];
uniform vec3 uLightColor[3];
out vec3 vColor;
out highp vec2 vUv;
void main() {
    float x = aPos.x - uCenter.x;
    float z = aPos.z - uCenter.y;
    float rx = x * uYaw.x + z * uYaw.y;
    float rz = -x * uYaw.y + z * uYaw.x;
    float sx = uSize.x * 0.5 + rx * uFrame.x;
    float sy = uFrame.z + (aPos.y * uPitch.x - rz * uPitch.y - uFrame.y) * uFrame.x;
    float depth = aPos.y * uPitch.y + rz * uPitch.x;
    gl_Position = vec4(sx / uSize.x * 2.0 - 1.0, 1.0 - sy / uSize.y * 2.0, clamp(depth / uDepth, -1.0, 1.0), 1.0);
    vec3 n = vec3(aNormal.x * uYaw.x + aNormal.z * uYaw.y, aNormal.y, -aNormal.x * uYaw.y + aNormal.z * uYaw.x);
    float len = length(n);
    if (len <= 1e-6) len = 1.0;
    vec3 lit = uAmbient;
    if (uLightMode == 1) {
        for (int i = 0; i < 3; i++) {
            float dl = length(uLightDir[i]);
            if (dl > 1e-6) {
                float k = -dot(n, uLightDir[i]) / (len * dl);
                if (k > 0.0) lit += uLightColor[i] * k;
            }
        }
    } else {
        lit += vec3(max(-n.z / len, 0.0) * 0.5);
    }
    vColor = lit * aColor;
    vUv = aUv;
}
"""

    private const val FRAGMENT = """#version 300 es
precision mediump float;
in vec3 vColor;
in highp vec2 vUv;
uniform sampler2D uTex;
out vec4 outColor;
void main() {
    outColor = vec4(texture(uTex, vUv).rgb * vColor, 1.0);
}
"""

    /** An icon's GL objects: a static buffer (normals, texture coordinates, colours), one for
     *  positions (filled per frame for an animated icon), a vertex array and the texture. */
    private class Mesh private constructor(private val icon: Ps2Icon) {
        var vao = 0
        var texture = 0
        private var staticVbo = 0
        private var positionVbo = 0
        private var uploaded = Float.NaN
        private val positions = FloatArray(icon.vertexCount * 3)
        private val weights = FloatArray(icon.shapeCount)
        private val positionBuffer: FloatBuffer =
            ByteBuffer.allocateDirect(icon.vertexCount * 12).order(ByteOrder.nativeOrder()).asFloatBuffer()

        /** The blended positions at [time] into the GPU, when they aren't there already. */
        fun upload(time: Float) {
            if (time == uploaded) return
            Ps2IconRenderer.blendInto(icon, time, positions, weights)
            positionBuffer.position(0)
            positionBuffer.put(positions)
            positionBuffer.position(0)
            GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, positionVbo)
            GLES30.glBufferSubData(GLES30.GL_ARRAY_BUFFER, 0, positions.size * 4, positionBuffer)
            GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, 0)
            uploaded = time
        }

        fun delete() {
            GLES30.glDeleteBuffers(2, intArrayOf(staticVbo, positionVbo), 0)
            GLES30.glDeleteVertexArrays(1, intArrayOf(vao), 0)
            GLES30.glDeleteTextures(1, intArrayOf(texture), 0)
        }

        companion object {
            fun create(icon: Ps2Icon): Mesh {
                val m = Mesh(icon)
                val nv = icon.vertexCount
                val attributes = ByteBuffer.allocateDirect(nv * 8 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
                for (v in 0 until nv) {
                    attributes.put(icon.normals[v * 3]).put(icon.normals[v * 3 + 1]).put(icon.normals[v * 3 + 2])
                    attributes.put(icon.uvs[v * 2]).put(icon.uvs[v * 2 + 1])
                    attributes.put(icon.colors[v * 3]).put(icon.colors[v * 3 + 1]).put(icon.colors[v * 3 + 2])
                }
                attributes.position(0)
                val buffers = IntArray(2)
                GLES30.glGenBuffers(2, buffers, 0)
                m.staticVbo = buffers[0]
                m.positionVbo = buffers[1]
                GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, m.staticVbo)
                GLES30.glBufferData(GLES30.GL_ARRAY_BUFFER, nv * 32, attributes, GLES30.GL_STATIC_DRAW)
                GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, m.positionVbo)
                GLES30.glBufferData(GLES30.GL_ARRAY_BUFFER, nv * 12, null, GLES30.GL_DYNAMIC_DRAW)

                val vao = IntArray(1)
                GLES30.glGenVertexArrays(1, vao, 0)
                m.vao = vao[0]
                GLES30.glBindVertexArray(m.vao)
                GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, m.positionVbo)
                GLES30.glEnableVertexAttribArray(0)
                GLES30.glVertexAttribPointer(0, 3, GLES30.GL_FLOAT, false, 12, 0)
                GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, m.staticVbo)
                GLES30.glEnableVertexAttribArray(1)
                GLES30.glVertexAttribPointer(1, 3, GLES30.GL_FLOAT, false, 32, 0)
                GLES30.glEnableVertexAttribArray(2)
                GLES30.glVertexAttribPointer(2, 2, GLES30.GL_FLOAT, false, 32, 12)
                GLES30.glEnableVertexAttribArray(3)
                GLES30.glVertexAttribPointer(3, 3, GLES30.GL_FLOAT, false, 32, 20)
                GLES30.glBindVertexArray(0)
                GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, 0)

                // The 128x128 texture, wrapping and bilinear as the software sampler; an untextured
                // icon samples plain white, so its vertex colours are all there is.
                val tex = IntArray(1)
                GLES30.glGenTextures(1, tex, 0)
                m.texture = tex[0]
                GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, m.texture)
                GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_REPEAT)
                GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_REPEAT)
                GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR)
                GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR)
                val src = icon.texture
                val size = if (src != null) Ps2Icon.TEXTURE_SIZE else 1
                val pixels = ByteBuffer.allocateDirect(size * size * 4).order(ByteOrder.nativeOrder())
                for (i in 0 until size * size) {
                    val c = src?.get(i) ?: 0xFFFFFF
                    pixels.put((c shr 16).toByte()).put((c shr 8).toByte()).put(c.toByte()).put(0xFF.toByte())
                }
                pixels.position(0)
                GLES30.glTexImage2D(GLES30.GL_TEXTURE_2D, 0, GLES30.GL_RGBA, size, size, 0, GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE, pixels)
                GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, 0)
                return m
            }
        }
    }
}

/**
 * One moving icon on screen, drawn into by [IconGl]'s thread. Compose sets what to show; the GL
 * thread reads it on each beat.
 */
internal class IconView(context: Context) : TextureView(context), TextureView.SurfaceTextureListener {
    @Volatile var key: Any? = null
    @Volatile var loaded: MemcardCovers.Loaded? = null
    @Volatile var options: Ps2IconRenderer.Options = Ps2IconRenderer.Options()
    @Volatile var animate: Boolean = true
    @Volatile var surfaceWidth = 0
    @Volatile var surfaceHeight = 0
    /** Called once, on the UI thread, when the first frame is on screen. */
    var onFirstFrame: (() -> Unit)? = null
    private var shown = false

    // The GL thread's own.
    internal var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    internal var drawnLoaded: MemcardCovers.Loaded? = null
    internal var drawnYaw = Float.NaN
    internal var drawnTime = Float.NaN
    internal var drawnW = 0
    internal var drawnH = 0
    internal var framing: Ps2IconRenderer.Framing? = null
    internal var framingFor: MemcardCovers.Loaded? = null
    internal var framingW = 0
    internal var framingH = 0
    internal var framingOptions: Ps2IconRenderer.Options? = null

    init {
        isOpaque = false
        surfaceTextureListener = this
    }

    fun bind(key: Any, loaded: MemcardCovers.Loaded, options: Ps2IconRenderer.Options, animate: Boolean, onFirstFrame: () -> Unit) {
        val iconChanged = this.loaded != null && this.loaded !== loaded
        this.key = key
        this.loaded = loaded
        this.options = options
        this.animate = animate
        this.onFirstFrame = onFirstFrame
        if (iconChanged) IconGl.iconChanged()
        IconGl.wake()
    }

    internal fun forgetDrawn() {
        drawnLoaded = null
        drawnYaw = Float.NaN
    }

    override fun onSurfaceTextureAvailable(surface: SurfaceTexture, width: Int, height: Int) {
        surfaceWidth = width
        surfaceHeight = height
        IconGl.attach(this, surface)
    }

    override fun onSurfaceTextureSizeChanged(surface: SurfaceTexture, width: Int, height: Int) {
        surfaceWidth = width
        surfaceHeight = height
        IconGl.wake()
    }

    override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean {
        // Released by the GL thread once its EGL surface is gone, so it is never drawn into after.
        IconGl.detach(this, surface)
        return false
    }

    override fun onSurfaceTextureUpdated(surface: SurfaceTexture) {
        if (!shown) {
            shown = true
            onFirstFrame?.invoke()
        }
    }
}
