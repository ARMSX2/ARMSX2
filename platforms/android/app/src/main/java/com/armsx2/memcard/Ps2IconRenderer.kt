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
    )

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

    /** One frame of an animated cover: no choosing, straight to the pixels. */
    fun renderFrame(icon: Ps2Icon, sys: Ps2IconSys?, width: Int, height: Int, pose: Pose, options: Options = Options()): IntArray =
        draw(icon, sys, width, height, pose.yaw, pose.time, options, options.supersample.coerceIn(1, 4)).pixels

    /**
     * The pose for a still cover, which has to choose one moment of something built to be seen
     * moving: the browser spins every icon and plays its animation.
     *
     * The front, turned slightly, at the animation's first frame is the default, since that is
     * the pose the icon is designed around. Only when it is clearly a poor view, under half the
     * area of the best one (GRAW's emblem is a coin that faces sideways and flips: edge-on it is
     * a line), are other moments tried at the front, and then other angles. Candidates are drawn
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
            tries += Try(Pose(y, t), options.yaw != null || i < FRONT_CANDIDATES, draw(icon, sys, 36, 52, y, t, options, 1))
        }
        val biggest = tries.maxOf { it.d.coverage }
        fun good(t: Try?) = t != null && t.d.coverage >= biggest * 0.5f
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

    /** What a draw came out as: the pixels, and for choosing between draws, how much of the
     *  tile the icon covers and its mean brightness. */
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
     * The model turns about its own vertical axis, through the middle of its footprint, and is
     * framed by the widest it gets over a full turn and every animation shape. So a turn or an
     * animation never changes its size or pushes it out of the tile, and a spinning cover starts
     * exactly where its still picture is.
     */
    private fun draw(
        icon: Ps2Icon, sys: Ps2IconSys?, width: Int, height: Int, yaw: Float, time: Float, options: Options, ss: Int,
    ): Drawn {
        val w = width * ss
        val h = height * ss
        val color = IntArray(w * h)
        val depth = FloatArray(w * h) { Float.NEGATIVE_INFINITY }
        if (options.background) background(color, w, h, sys)

        val nv = icon.vertexCount
        val pos = blend(icon, time)
        val cy = cos(yaw)
        val sy = sin(yaw)

        val all = icon.positions
        val count = icon.shapeCount * icon.vertexCount
        var lx = Float.MAX_VALUE; var hx = -Float.MAX_VALUE
        var lz = Float.MAX_VALUE; var hz = -Float.MAX_VALUE
        var minY = Float.MAX_VALUE; var maxY = -Float.MAX_VALUE
        for (i in 0 until count) {
            lx = min(lx, all[i * 3]); hx = max(hx, all[i * 3])
            minY = min(minY, all[i * 3 + 1]); maxY = max(maxY, all[i * 3 + 1])
            lz = min(lz, all[i * 3 + 2]); hz = max(hz, all[i * 3 + 2])
        }
        val cx = (lx + hx) / 2f
        val cz = (lz + hz) / 2f
        var r2 = 1e-6f
        for (i in 0 until count) {
            val dx = all[i * 3] - cx; val dz = all[i * 3 + 2] - cz
            r2 = max(r2, dx * dx + dz * dz)
        }
        // Width and height fitted separately: a standing figure in a tall tile uses the height.
        val scale = min(
            w * options.fill / (2f * sqrt(r2)).coerceAtLeast(1e-3f),
            h * options.fill / (maxY - minY).coerceAtLeast(1e-3f),
        )
        // Where icon-space y lands on screen: its base (the largest y) on the bottom edge, or
        // its middle in the middle.
        val refY = if (options.anchorBottom) maxY else (minY + maxY) / 2f
        val baseY = if (options.anchorBottom) h * (1f - BOTTOM_MARGIN) else h / 2f

        // Transform and light each vertex once.
        val sx = FloatArray(nv); val syy = FloatArray(nv); val sz = FloatArray(nv)
        val lit = FloatArray(nv * 3)

        val floor = options.ambientFloor
        val amb = sys?.ambient ?: floatArrayOf(0.55f, 0.55f, 0.55f)
        for (v in 0 until nv) {
            val x = pos[v * 3] - cx; val y = pos[v * 3 + 1]; val z = pos[v * 3 + 2] - cz
            val rx = x * cy + z * sy
            val rz = -x * sy + z * cy
            sx[v] = w / 2f + rx * scale
            syy[v] = baseY + (y - refY) * scale
            // Nearer the camera = smaller z; the depth test keeps the larger value, so negate.
            sz[v] = -rz

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
        for (i in color.indices) if (depth[i] != Float.NEGATIVE_INFINITY) {
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

    /** Morph-target positions at [time]: each frame names a shape and a weight curve over time;
     *  the shapes are blended by those weights. One shape, or nothing to go on, is just shape 0. */
    fun blend(icon: Ps2Icon, time: Float): FloatArray {
        val nv = icon.vertexCount
        val out = FloatArray(nv * 3)
        val weights = FloatArray(icon.shapeCount)
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
            for (shift in intArrayOf(16, 8, 0)) {
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
                for (shift in intArrayOf(16, 8, 0)) {
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
        for (py in minY..maxY) {
            val fy = py + 0.5f
            for (px in minX..maxX) {
                val fx = px + 0.5f
                val w0 = ((y1 - y2) * (fx - x2) + (x2 - x1) * (fy - y2)) * inv
                if (w0 < 0f) continue
                val w1 = ((y2 - y0) * (fx - x2) + (x0 - x2) * (fy - y2)) * inv
                if (w1 < 0f) continue
                val w2 = 1f - w0 - w1
                if (w2 < 0f) continue
                val z = w0 * sz[a] + w1 * sz[b] + w2 * sz[c]
                val k = py * w + px
                if (z <= depth[k]) continue
                depth[k] = z
                var r = 1f; var g = 1f; var bl = 1f
                if (tex != null) {
                    val u = w0 * uvs[a * 2] + w1 * uvs[b * 2] + w2 * uvs[c * 2]
                    val v = w0 * uvs[a * 2 + 1] + w1 * uvs[b * 2 + 1] + w2 * uvs[c * 2 + 1]
                    val t = sample(tex, u, v)
                    r = ((t shr 16) and 0xFF) / 255f; g = ((t shr 8) and 0xFF) / 255f; bl = (t and 0xFF) / 255f
                }
                r *= w0 * lit[a * 3] + w1 * lit[b * 3] + w2 * lit[c * 3]
                g *= w0 * lit[a * 3 + 1] + w1 * lit[b * 3 + 1] + w2 * lit[c * 3 + 1]
                bl *= w0 * lit[a * 3 + 2] + w1 * lit[b * 3 + 2] + w2 * lit[c * 3 + 2]
                color[k] = 0xFF000000.toInt() or
                    ((r * 255f).toInt().coerceIn(0, 255) shl 16) or
                    ((g * 255f).toInt().coerceIn(0, 255) shl 8) or
                    (bl * 255f).toInt().coerceIn(0, 255)
            }
        }
    }

    /** Bilinear sample of the 128x128 texture, wrapping at the edges. */
    private fun sample(tex: IntArray, u: Float, v: Float): Int {
        val n = Ps2Icon.TEXTURE_SIZE
        val fx = u * n - 0.5f
        val fy = v * n - 0.5f
        val x0 = kotlin.math.floor(fx).toInt(); val y0 = kotlin.math.floor(fy).toInt()
        val ax = fx - x0; val ay = fy - y0
        fun at(x: Int, y: Int) = tex[((y % n + n) % n) * n + ((x % n + n) % n)]
        val c00 = at(x0, y0); val c10 = at(x0 + 1, y0); val c01 = at(x0, y0 + 1); val c11 = at(x0 + 1, y0 + 1)
        var out = 0
        for (shift in intArrayOf(16, 8, 0)) {
            val a = (c00 shr shift) and 0xFF; val b = (c10 shr shift) and 0xFF
            val c = (c01 shr shift) and 0xFF; val d = (c11 shr shift) and 0xFF
            val top = a + (b - a) * ax
            val bottom = c + (d - c) * ax
            out = out or ((top + (bottom - top) * ay).toInt().coerceIn(0, 255) shl shift)
        }
        return out
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
