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
    class Options(
        /** Turn about the vertical axis, radians, or null to choose the best-lit of a few turns.
         *  The browser spins icons, and each save lights its icon from its own directions, so no
         *  one angle suits every icon: a front-facing card lit only from the side sits in the dark
         *  part of the spin (the ESPN NFL 2K5 box). */
        val yaw: Float? = null,
        /** Animation time in frames, for icons that morph. */
        val time: Float = 0f,
        /** How much of the tile the icon may fill, 0..1. */
        val fill: Float = 0.82f,
        /** Renders at this many times the size and averages down, to smooth the edges. */
        val supersample: Int = 2,
        /** Least light any surface gets. The console shades with the save's own ambient, which
         *  is often dim (0.25); on a still cover that leaves unlit faces nearly black. */
        val ambientFloor: Float = 0.45f,
    )

    private val YAW_CANDIDATES = floatArrayOf(0.35f, -0.35f, 0f, 0.7f, -0.7f)

    /** ARGB pixels, [width] x [height]. */
    fun render(icon: Ps2Icon, sys: Ps2IconSys?, width: Int, height: Int, options: Options = Options()): IntArray {
        val yaw = options.yaw ?: YAW_CANDIDATES.maxByOrNull { draw(icon, sys, 36, 52, it, options, 1).iconLuma }!!
        return draw(icon, sys, width, height, yaw, options, options.supersample.coerceIn(1, 4)).pixels
    }

    private class Drawn(val pixels: IntArray, val iconLuma: Float)

    /**
     * Icon space is x right, y down, z into the screen, with the camera on the -z side: that is
     * the side a save's own logo reads correctly from (ESPN's, not mirrored). The icon.sys light
     * directions point the way the light travels, so a surface is lit by the reverse of each.
     */
    private fun draw(icon: Ps2Icon, sys: Ps2IconSys?, width: Int, height: Int, yaw: Float, options: Options, ss: Int): Drawn {
        val w = width * ss
        val h = height * ss
        val color = IntArray(w * h)
        val depth = FloatArray(w * h) { Float.NEGATIVE_INFINITY }
        background(color, w, h, sys)

        val nv = icon.vertexCount
        val pos = blend(icon, options.time)
        val cy = cos(yaw)
        val sy = sin(yaw)

        // Frame every shape at once, so an animating icon doesn't bob as it changes size.
        var minX = Float.MAX_VALUE; var maxX = -Float.MAX_VALUE
        var minY = Float.MAX_VALUE; var maxY = -Float.MAX_VALUE
        val allShapes = icon.positions
        for (i in 0 until icon.shapeCount * icon.vertexCount) {
            val x = allShapes[i * 3]; val y = allShapes[i * 3 + 1]; val z = allShapes[i * 3 + 2]
            val rx = x * cy + z * sy
            minX = min(minX, rx); maxX = max(maxX, rx)
            minY = min(minY, y); maxY = max(maxY, y)
        }
        val span = max(maxX - minX, maxY - minY).coerceAtLeast(1e-3f)
        val scale = min(w, h) * options.fill / span
        val midX = (minX + maxX) / 2f
        val midY = (minY + maxY) / 2f

        // Transform and light each vertex once.
        val sx = FloatArray(nv); val syy = FloatArray(nv); val sz = FloatArray(nv)
        val lit = FloatArray(nv * 3)
        val floor = options.ambientFloor
        val amb = sys?.ambient ?: floatArrayOf(0.55f, 0.55f, 0.55f)
        for (v in 0 until nv) {
            val x = pos[v * 3]; val y = pos[v * 3 + 1]; val z = pos[v * 3 + 2]
            val rx = x * cy + z * sy
            val rz = -x * sy + z * cy
            sx[v] = w / 2f + (rx - midX) * scale
            syy[v] = h / 2f + (y - midY) * scale
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
        // How well lit the icon itself came out, for picking a turn: mean luma over its pixels.
        var sum = 0f; var n = 0
        for (i in color.indices) if (depth[i] != Float.NEGATIVE_INFINITY) {
            val p = color[i]
            sum += 0.299f * ((p shr 16) and 0xFF) + 0.587f * ((p shr 8) and 0xFF) + 0.114f * (p and 0xFF)
            n++
        }
        val luma = if (n == 0) 0f else sum / n
        return Drawn(if (ss == 1) color else downsample(color, w, h, ss), luma)
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

    private fun downsample(src: IntArray, w: Int, h: Int, ss: Int): IntArray {
        val ow = w / ss; val oh = h / ss
        val out = IntArray(ow * oh)
        val n = ss * ss
        for (y in 0 until oh) for (x in 0 until ow) {
            var r = 0; var g = 0; var b = 0
            for (dy in 0 until ss) for (dx in 0 until ss) {
                val p = src[(y * ss + dy) * w + x * ss + dx]
                r += (p shr 16) and 0xFF; g += (p shr 8) and 0xFF; b += p and 0xFF
            }
            out[y * ow + x] = 0xFF000000.toInt() or ((r / n) shl 16) or ((g / n) shl 8) or (b / n)
        }
        return out
    }
}
