package com.armsx2.memcard

import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Draws a PS2 save icon the way the console's browser presents it: on its icon.sys background
 * gradient, shaded by its three lights and ambient light, at a given animation time and turn.
 *
 * A plain software rasteriser with a depth buffer, so it runs anywhere (a unit test included),
 * renders a cover once and is done: covers are cached, never drawn per frame of scrolling.
 */
object Ps2IconRenderer {
    data class Options(
        /** Turn about the vertical axis, radians, or null to choose one (see [choosePose]). */
        val yaw: Float? = null,
        /** Animation time in frames, or null to choose one along with the turn. */
        val time: Float? = null,
        /** How much of the tile the icon may fill, 0..1. */
        val fill: Float = 0.82f,
        /** Renders at this many times the size and averages down, to smooth the edges. */
        val supersample: Int = 2,
        /** Least light any surface gets. The console shades with the save's own ambient, which
         *  is often dim (0.25); on a still cover that leaves unlit faces nearly black. */
        val ambientFloor: Float = 0.45f,
        /** Draw the save's icon.sys gradient behind it; without, everything but the icon is
         *  transparent (a library cover is just the icon, on the shelf like a cut-out case). */
        val background: Boolean = true,
        /** Stand the icon on the bottom edge, as it stands on y = 0 in its own space, rather than
         *  centring it: on the shelf it sits on the shelf, over its reflection. */
        val anchorBottom: Boolean = false,
        /** How far the camera looks down on the icon, radians. The console's browser looks down on
         *  its icons (the tops of boxes show), about this much. */
        val pitch: Float = CAMERA_PITCH,
    )

    const val CAMERA_PITCH = 0.3f

    /**
     * Buffers a moving icon keeps from frame to frame, so drawing a frame allocates nothing: a
     * library full of turning covers would otherwise make garbage by the megabyte every second.
     * One per animation; not for use by two renders at once.
     */
    class Scratch {
        internal var color = IntArray(0)
        internal var depth = FloatArray(0)
        internal var pos = FloatArray(0)
        internal var weights = FloatArray(0)
        internal var sx = FloatArray(0)
        internal var sy = FloatArray(0)
        internal var sz = FloatArray(0)
        internal var lit = FloatArray(0)

        // The framing of the icon last drawn here: fixed for an icon at a size, so worked out once
        // rather than over every vertex of every shape each frame.
        internal var framedIcon: Ps2Icon? = null
        internal var framedWidth = 0
        internal var framedHeight = 0
        internal var framedOptions: Options? = null
        internal var cx = 0f
        internal var cz = 0f
        internal var scale = 0f
        internal var refY = 0f
        internal var baseY = 0f

        internal fun fit(pixels: Int, vertices: Int, shapes: Int) {
            if (color.size != pixels) { color = IntArray(pixels); depth = FloatArray(pixels) }
            if (sx.size != vertices) {
                pos = FloatArray(vertices * 3); sx = FloatArray(vertices); sy = FloatArray(vertices)
                sz = FloatArray(vertices); lit = FloatArray(vertices * 3)
            }
            if (weights.size != shapes) weights = FloatArray(shapes)
        }
    }

    /** A turn about the vertical axis (radians) and a moment of the animation (frames). */
    data class Pose(val yaw: Float, val time: Float)

    // A slight turn either side of each quarter: 20 degrees off square reads as 3D. The first two
    // are the front, which a still cover keeps unless it is clearly a poor view.
    private val YAW_CANDIDATES = floatArrayOf(-0.35f, 0.35f, 1.22f, 1.92f, 2.79f, 3.49f, 4.36f, 5.06f)
    private const val FRONT_CANDIDATES = 2

    /** Room left under an icon stood on the bottom edge. */
    private const val BOTTOM_MARGIN = 0.03f

    /** How the last automatic choice was made, for the dev contact sheet. */
    @Volatile internal var lastPick: String = ""

    /** ARGB pixels, [width] x [height], at the pose [options] names or, for what it leaves
     *  open, the one [choosePose] picks. */
    fun render(icon: Ps2Icon, sys: Ps2IconSys?, width: Int, height: Int, options: Options = Options()): IntArray {
        val pose = if (options.yaw != null && options.time != null) Pose(options.yaw, options.time)
            else choosePose(icon, sys, options)
        return draw(icon, sys, width, height, pose.yaw, pose.time, options, options.supersample.coerceIn(1, 4)).pixels
    }

    /** One frame of an animated cover: no choosing, straight to the pixels. With [scratch], and no
     *  supersampling, the pixels are the scratch's own buffer, good until its next frame. */
    fun renderFrame(
        icon: Ps2Icon, sys: Ps2IconSys?, width: Int, height: Int, pose: Pose, options: Options = Options(), scratch: Scratch? = null,
    ): IntArray = draw(icon, sys, width, height, pose.yaw, pose.time, options, options.supersample.coerceIn(1, 4), scratch).pixels

    /**
     * The pose for a still cover, which has to choose one moment of something built to be seen
     * moving: the browser spins every icon and plays its animation.
     *
     * The front, turned slightly, at the animation's first frame is the default, since that is
     * the pose the icon is designed around. Only when it is clearly a poor view, under 60% of the
     * area of the best one (GRAW's emblem is a coin that faces sideways and flips: edge-on it is
     * a sliver, 52% from the camera's height), are other moments tried at the front, and then
     * other angles. The closest a good rest view comes is Blinky head-on, at 61%. Candidates are drawn
     * at thumbnail size, and the framing does not change with the turn, so area is how big the
     * icon really looks from there.
     *
     * Deliberately not judged by which way surfaces face: the stored normals can't be trusted for
     * that (every normal on Bakugan's card points the same way, both faces included).
     */
    fun choosePose(icon: Ps2Icon, sys: Ps2IconSys?, options: Options = Options()): Pose {
        val yaws = options.yaw?.let { floatArrayOf(it) } ?: YAW_CANDIDATES
        val len = icon.frameLength.coerceAtLeast(1).toFloat()
        val first = icon.playOffset.toFloat().coerceIn(0f, len)
        val times = options.time?.let { floatArrayOf(it) }
            ?: if (icon.animated) floatArrayOf(first, len * 0.25f, len * 0.5f, len * 0.75f) else floatArrayOf(first)
        class Try(val pose: Pose, val front: Boolean, val d: Drawn) {
            val view get() = d.coverage * (0.7f + 0.3f * d.luma / 255f)
        }
        val tries = ArrayList<Try>()
        for ((i, y) in yaws.withIndex()) for (t in times) {
            tries += Try(Pose(y, t), options.yaw != null || i < FRONT_CANDIDATES, draw(icon, sys, 36, 52, y, t, options, 1, measure = true))
        }
        val biggest = tries.maxOf { it.d.coverage }
        fun good(t: Try?) = t != null && t.d.coverage >= biggest * 0.6f
        val restPose = tries.filter { it.front && it.pose.time == times[0] }.maxByOrNull { it.view }
        val frontAnyTime = tries.filter { it.front }.maxByOrNull { it.view }
        val pick = when {
            good(restPose) -> restPose
            good(frontAnyTime) -> frontAnyTime
            else -> tries.maxByOrNull { it.view }
        } ?: return Pose(0f, first)
        lastPick = "rest(cov=%.3f) front(cov=%.3f t=%.0f) biggest=%.3f -> yaw=%.2f t=%.0f".format(
            restPose?.d?.coverage ?: 0f, frontAnyTime?.d?.coverage ?: 0f, frontAnyTime?.pose?.time ?: 0f, biggest,
            pick.pose.yaw, pick.pose.time)
        return pick.pose
    }

    /** What a draw came out as: the pixels, and for choosing between draws (when measured), how
     *  much of the tile the icon covers and its mean brightness. */
    private class Drawn(val pixels: IntArray, val coverage: Float, val luma: Float)

    /**
     * Icon space is x right, y down, z into the screen, with the camera on the -z side: that is
     * the side a save's own logo reads correctly from (ESPN's, not mirrored). The icon.sys light
     * directions point the way the light travels, so a surface is lit by the reverse of each.
     *
     * Back faces, wound clockwise on screen, are not drawn, as on the console: on 200 of 212 real
     * saves everything visible is wound the other way, and Bakugan's card, which is built inside
     * out, reads the right way round from either side only with its near face hidden.
     *
     * The camera looks down on the model by [Options.pitch]. The model turns about its own vertical
     * axis, through the middle of its footprint, and is framed by the most room it takes on screen
     * over a full turn and every animation shape. So a turn or an animation never changes its size
     * or pushes it out of the tile, and a spinning cover starts exactly where its still picture is.
     */
    private fun draw(
        icon: Ps2Icon, sys: Ps2IconSys?, width: Int, height: Int, yaw: Float, time: Float, options: Options, ss: Int,
        scratch: Scratch? = null, measure: Boolean = false,
    ): Drawn {
        val w = width * ss
        val h = height * ss
        val nv = icon.vertexCount
        val buf = scratch ?: Scratch()
        buf.fit(w * h, nv, icon.shapeCount)
        val color = buf.color
        val depth = buf.depth
        depth.fill(Float.NEGATIVE_INFINITY)
        if (options.background) background(color, w, h, sys) else color.fill(0)

        val pos = blendInto(icon, time, buf.pos, buf.weights)
        val cy = cos(yaw)
        val sy = sin(yaw)
        val cp = cos(options.pitch)
        val sp = sin(options.pitch)

        if (buf.framedIcon !== icon || buf.framedWidth != w || buf.framedHeight != h ||
            buf.framedOptions?.let { it.pitch == options.pitch && it.fill == options.fill && it.anchorBottom == options.anchorBottom } != true
        ) {
            frame(icon, w, h, options, cp, sp, buf)
        }
        val cx = buf.cx
        val cz = buf.cz
        val scale = buf.scale
        val refY = buf.refY
        val baseY = buf.baseY

        // Transform and light each vertex once.
        val sx = buf.sx; val syy = buf.sy; val sz = buf.sz
        val lit = buf.lit

        val floor = options.ambientFloor
        val amb = sys?.ambient ?: floatArrayOf(0.55f, 0.55f, 0.55f)
        for (v in 0 until nv) {
            val x = pos[v * 3] - cx; val y = pos[v * 3 + 1]; val z = pos[v * 3 + 2] - cz
            val rx = x * cy + z * sy
            val rz = -x * sy + z * cy
            sx[v] = w / 2f + rx * scale
            // Looking down: what is further back sits higher on screen.
            syy[v] = baseY + (y * cp - rz * sp - refY) * scale
            // Nearer the camera = less depth; the depth test keeps the larger value, so negate.
            sz[v] = -(y * sp + rz * cp)

            val n0 = icon.normals[v * 3]; val ny = icon.normals[v * 3 + 1]; val n2 = icon.normals[v * 3 + 2]
            val nx = n0 * cy + n2 * sy
            val nz = -n0 * sy + n2 * cy
            val len = sqrt(nx * nx + ny * ny + nz * nz).takeIf { it > 1e-6f } ?: 1f
            var r = max(amb[0], floor); var g = max(amb[1], floor); var b = max(amb[2], floor)
            if (sys != null) for (l in 0 until 3) {
                val d = sys.lightDirections[l]
                val dl = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).takeIf { it > 1e-6f } ?: continue
                val k = -(nx * d[0] + ny * d[1] + nz * d[2]) / (len * dl)
                if (k > 0f) {
                    val c = sys.lightColors[l]
                    r += c[0] * k; g += c[1] * k; b += c[2] * k
                }
            } else {
                // No icon.sys: one soft light from the camera.
                val k = (-nz / len).coerceAtLeast(0f) * 0.5f
                r += k; g += k; b += k
            }
            lit[v * 3] = r * icon.colors[v * 3]
            lit[v * 3 + 1] = g * icon.colors[v * 3 + 1]
            lit[v * 3 + 2] = b * icon.colors[v * 3 + 2]
        }

        val tex = icon.texture
        var t = 0
        while (t + 2 < nv) {
            triangle(t, t + 1, t + 2, sx, syy, sz, lit, icon.uvs, tex, color, depth, w, h)
            t += 3
        }
        var sum = 0f; var n = 0
        if (measure) for (i in color.indices) if (depth[i] != Float.NEGATIVE_INFINITY) {
            val p = color[i]
            sum += 0.299f * ((p shr 16) and 0xFF) + 0.587f * ((p shr 8) and 0xFF) + 0.114f * (p and 0xFF)
            n++
        }
        return Drawn(
            pixels = if (ss == 1) color else downsample(color, w, h, ss),
            coverage = n / color.size.toFloat(),
            luma = if (n == 0) 0f else sum / n,
        )
    }

    /** Where [icon] sits in a [w] x [h] tile, into [buf]: see [draw]. */
    private fun frame(icon: Ps2Icon, w: Int, h: Int, options: Options, cp: Float, sp: Float, buf: Scratch) {
        val f = framing(icon, w, h, options)
        buf.cx = f.cx
        buf.cz = f.cz
        buf.scale = f.scale
        buf.refY = f.refY
        buf.baseY = f.baseY
        buf.framedIcon = icon
        buf.framedWidth = w
        buf.framedHeight = h
        buf.framedOptions = options
    }

    /** Where an icon sits in a tile, as [framing] works it out. [depthRange] bounds how far any
     *  point gets from the axis in depth, for a GPU's depth buffer. */
    internal class Framing(val cx: Float, val cz: Float, val scale: Float, val refY: Float, val baseY: Float, val depthRange: Float)

    /** Where [icon] sits in a [w] x [h] tile: see [draw]. The software renderer and the GPU one
     *  (IconGl) both use this, so the two agree to the pixel. */
    internal fun framing(icon: Ps2Icon, w: Int, h: Int, options: Options): Framing {
        val cp = cos(options.pitch)
        val sp = sin(options.pitch)
        val all = icon.positions
        val count = icon.shapeCount * icon.vertexCount
        var lx = Float.MAX_VALUE; var hx = -Float.MAX_VALUE
        var lz = Float.MAX_VALUE; var hz = -Float.MAX_VALUE
        for (i in 0 until count) {
            lx = min(lx, all[i * 3]); hx = max(hx, all[i * 3])
            lz = min(lz, all[i * 3 + 2]); hz = max(hz, all[i * 3 + 2])
        }
        val cx = (lx + hx) / 2f
        val cz = (lz + hz) / 2f
        // A point r from the axis swings from r in front to r behind over a turn, so seen from
        // above it rises and falls on screen by r * sin(pitch) about its own height.
        var r2 = 1e-6f
        var top = Float.MAX_VALUE; var bottom = -Float.MAX_VALUE
        var reachY = 0f
        for (i in 0 until count) {
            val dx = all[i * 3] - cx; val dz = all[i * 3 + 2] - cz
            val rr = dx * dx + dz * dz
            r2 = max(r2, rr)
            val y = all[i * 3 + 1] * cp
            val swing = sqrt(rr) * sp
            top = min(top, y - swing); bottom = max(bottom, y + swing)
            reachY = max(reachY, kotlin.math.abs(all[i * 3 + 1]))
        }
        // Width and height fitted separately: a standing figure in a tall tile uses the height.
        val scale = min(
            w * options.fill / (2f * sqrt(r2)).coerceAtLeast(1e-3f),
            h * options.fill / (bottom - top).coerceAtLeast(1e-3f),
        )
        // Where the icon lands on screen: its lowest point on the bottom edge, or its middle in
        // the middle.
        return Framing(
            cx = cx,
            cz = cz,
            scale = scale,
            refY = if (options.anchorBottom) bottom else (top + bottom) / 2f,
            baseY = if (options.anchorBottom) h * (1f - BOTTOM_MARGIN) else h / 2f,
            depthRange = sqrt(r2) + reachY + 1e-3f,
        )
    }

    /** Morph-target positions at [time]: each frame names a shape and a weight curve over time;
     *  the shapes are blended by those weights. One shape, or nothing to go on, is just shape 0. */
    fun blend(icon: Ps2Icon, time: Float): FloatArray =
        blendInto(icon, time, FloatArray(icon.vertexCount * 3), FloatArray(icon.shapeCount))

    internal fun blendInto(icon: Ps2Icon, time: Float, out: FloatArray, weights: FloatArray): FloatArray {
        val nv = icon.vertexCount
        out.fill(0f)
        weights.fill(0f)
        if (icon.shapeCount > 1 && icon.frames.isNotEmpty()) {
            for (f in icon.frames) if (f.shape in 0 until icon.shapeCount) weights[f.shape] += weightAt(f, time)
        }
        var total = weights.sum()
        if (total <= 1e-6f) { weights.fill(0f); weights[0] = 1f; total = 1f }
        for (s in 0 until icon.shapeCount) {
            val wgt = weights[s] / total
            if (wgt == 0f) continue
            val base = s * icon.vertexCount * 3
            for (i in 0 until nv * 3) out[i] += icon.positions[base + i] * wgt
        }
        return out
    }

    private fun weightAt(f: Ps2Icon.Frame, time: Float): Float {
        val n = f.times.size
        if (n == 0) return 0f
        if (time <= f.times[0]) return f.weights[0]
        for (k in 1 until n) {
            if (time <= f.times[k]) {
                val t0 = f.times[k - 1]; val t1 = f.times[k]
                val a = if (t1 > t0) (time - t0) / (t1 - t0) else 1f
                return f.weights[k - 1] + (f.weights[k] - f.weights[k - 1]) * a
            }
        }
        return f.weights[n - 1]
    }

    /** Just a save's background gradient, [w] x [h], for a caller that draws the icon over it. */
    fun backgroundPixels(sys: Ps2IconSys?, w: Int, h: Int): IntArray = IntArray(w * h).also { background(it, w, h, sys) }

    // The browser's own backdrop, dark blue, that a save's background is laid over.
    private val BROWSER_BACKGROUND = intArrayOf(0x1A2A4A, 0x1A2A4A, 0x0A0F1E, 0x0A0F1E)

    private fun background(px: IntArray, w: Int, h: Int, sys: Ps2IconSys?) {
        // The save's gradient over the browser's, at the save's own opacity: ESPN's black at 96/128
        // is a dark blue on the console, not the flat black the colours alone would give.
        val alpha = (sys?.backgroundAlpha ?: 0) / 128f
        val c = IntArray(4) { i ->
            val over = sys?.background?.get(i) ?: 0
            val under = BROWSER_BACKGROUND[i]
            var mixed = 0
            for (shift in CHANNEL_SHIFTS) {
                val a = (under shr shift) and 0xFF
                val b = (over shr shift) and 0xFF
                mixed = mixed or ((a + (b - a) * alpha).toInt().coerceIn(0, 255) shl shift)
            }
            mixed
        }
        for (y in 0 until h) {
            val v = y / (h - 1f).coerceAtLeast(1f)
            for (x in 0 until w) {
                val u = x / (w - 1f).coerceAtLeast(1f)
                var out = 0xFF000000.toInt()
                for (shift in CHANNEL_SHIFTS) {
                    val tl = (c[0] shr shift) and 0xFF; val tr = (c[1] shr shift) and 0xFF
                    val bl = (c[2] shr shift) and 0xFF; val br = (c[3] shr shift) and 0xFF
                    val top = tl + (tr - tl) * u
                    val bottom = bl + (br - bl) * u
                    out = out or ((top + (bottom - top) * v).toInt().coerceIn(0, 255) shl shift)
                }
                px[y * w + x] = out
            }
        }
    }

    // Red, green and blue in a packed 0xRRGGBB, made once: an intArrayOf() in a loop is a new array
    // every time round.
    private val CHANNEL_SHIFTS = intArrayOf(16, 8, 0)

    private fun triangle(
        a: Int, b: Int, c: Int,
        sx: FloatArray, sy: FloatArray, sz: FloatArray, lit: FloatArray, uvs: FloatArray, tex: IntArray?,
        color: IntArray, depth: FloatArray, w: Int, h: Int,
    ) {
        val x0 = sx[a]; val y0 = sy[a]; val x1 = sx[b]; val y1 = sy[b]; val x2 = sx[c]; val y2 = sy[c]
        val den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if (den == 0f || !den.isFinite()) return
        // Clockwise on screen: a back face.
        if (den > 0f) return
        val minX = max(0, min(x0, min(x1, x2)).toInt())
        val maxX = min(w - 1, max(x0, max(x1, x2)).toInt() + 1)
        val minY = max(0, min(y0, min(y1, y2)).toInt())
        val maxY = min(h - 1, max(y0, max(y1, y2)).toInt() + 1)
        if (minX > maxX || minY > maxY) return
        val inv = 1f / den
        // The weights change by a constant from one pixel to the next along a row, so a pixel adds
        // rather than multiplies; and the corners' values are read once, not per pixel.
        val d0x = (y1 - y2) * inv; val d0y = (x2 - x1) * inv
        val d1x = (y2 - y0) * inv; val d1y = (x0 - x2) * inv
        val za = sz[a]; val zb = sz[b]; val zc = sz[c]
        val ra = lit[a * 3]; val ga = lit[a * 3 + 1]; val ba = lit[a * 3 + 2]
        val rb = lit[b * 3]; val gb = lit[b * 3 + 1]; val bb = lit[b * 3 + 2]
        val rc = lit[c * 3]; val gc = lit[c * 3 + 1]; val bc = lit[c * 3 + 2]
        val ua = uvs[a * 2]; val va = uvs[a * 2 + 1]
        val ub = uvs[b * 2]; val vb = uvs[b * 2 + 1]
        val uc = uvs[c * 2]; val vc = uvs[c * 2 + 1]
        val fx0 = minX + 0.5f - x2
        for (py in minY..maxY) {
            val fy = py + 0.5f - y2
            var w0 = d0x * fx0 + d0y * fy
            var w1 = d1x * fx0 + d1y * fy
            var k = py * w + minX
            for (px in minX..maxX) {
                val w2 = 1f - w0 - w1
                if (w0 >= 0f && w1 >= 0f && w2 >= 0f) {
                    val z = w0 * za + w1 * zb + w2 * zc
                    if (z > depth[k]) {
                        depth[k] = z
                        var r = w0 * ra + w1 * rb + w2 * rc
                        var g = w0 * ga + w1 * gb + w2 * gc
                        var bl = w0 * ba + w1 * bb + w2 * bc
                        if (tex != null) {
                            val t = sample(tex, w0 * ua + w1 * ub + w2 * uc, w0 * va + w1 * vb + w2 * vc)
                            r *= ((t shr 16) and 0xFF) * INV_255
                            g *= ((t shr 8) and 0xFF) * INV_255
                            bl *= (t and 0xFF) * INV_255
                        }
                        color[k] = 0xFF000000.toInt() or
                            (byte(r * 255f) shl 16) or (byte(g * 255f) shl 8) or byte(bl * 255f)
                    }
                }
                w0 += d0x
                w1 += d1x
                k++
            }
        }
    }

    private const val INV_255 = 1f / 255f

    /** Bilinear sample of the 128x128 texture, wrapping at the edges (a mask, the size being a
     *  power of two). Allocates nothing: it runs for every textured pixel of every frame. */
    private fun sample(tex: IntArray, u: Float, v: Float): Int {
        val fx = u * 128f - 0.5f
        val fy = v * 128f - 0.5f
        val flx = kotlin.math.floor(fx)
        val fly = kotlin.math.floor(fy)
        val ax = fx - flx
        val ay = fy - fly
        val x0 = flx.toInt() and 127; val x1 = (x0 + 1) and 127
        val y0 = (fly.toInt() and 127) shl 7; val y1 = (((fly.toInt() + 1)) and 127) shl 7
        val c00 = tex[y0 or x0]; val c10 = tex[y0 or x1]; val c01 = tex[y1 or x0]; val c11 = tex[y1 or x1]
        return (lerp2(c00 shr 16 and 0xFF, c10 shr 16 and 0xFF, c01 shr 16 and 0xFF, c11 shr 16 and 0xFF, ax, ay) shl 16) or
            (lerp2(c00 shr 8 and 0xFF, c10 shr 8 and 0xFF, c01 shr 8 and 0xFF, c11 shr 8 and 0xFF, ax, ay) shl 8) or
            lerp2(c00 and 0xFF, c10 and 0xFF, c01 and 0xFF, c11 and 0xFF, ax, ay)
    }

    // Both inline, here where they run for every channel of every pixel: as calls (coerceIn is one,
    // shared with the rest of the app once shrunk) they were a fifth of a moving cover's time.
    @Suppress("NOTHING_TO_INLINE")
    private inline fun lerp2(a: Int, b: Int, c: Int, d: Int, ax: Float, ay: Float): Int {
        val top = a + (b - a) * ax
        val bottom = c + (d - c) * ax
        return byte(top + (bottom - top) * ay)
    }

    /** 0..255 from a float, clamped. */
    @Suppress("NOTHING_TO_INLINE")
    private inline fun byte(v: Float): Int {
        val i = v.toInt()
        return if (i < 0) 0 else if (i > 255) 255 else i
    }

    /** Averages each [ss] x [ss] block. Colour is averaged over the covered samples only and
     *  coverage becomes alpha, so a transparent icon's edges are smooth with no dark fringe. */
    private fun downsample(src: IntArray, w: Int, h: Int, ss: Int): IntArray {
        val ow = w / ss; val oh = h / ss
        val out = IntArray(ow * oh)
        val n = ss * ss
        for (y in 0 until oh) for (x in 0 until ow) {
            var r = 0; var g = 0; var b = 0; var covered = 0
            for (dy in 0 until ss) for (dx in 0 until ss) {
                val p = src[(y * ss + dy) * w + x * ss + dx]
                if (p ushr 24 == 0) continue
                r += (p shr 16) and 0xFF; g += (p shr 8) and 0xFF; b += p and 0xFF
                covered++
            }
            out[y * ow + x] = if (covered == 0) 0
                else ((covered * 255 / n) shl 24) or ((r / covered) shl 16) or ((g / covered) shl 8) or (b / covered)
        }
        return out
    }
}
