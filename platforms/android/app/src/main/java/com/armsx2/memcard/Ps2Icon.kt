package com.armsx2.memcard

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * A PS2 save icon (.ico / .icn): a triangle list with morph-target animation and a 128x128 texture.
 *
 * Layout, all little-endian:
 *  - header: magic 0x00010000, shape count, texture type, a float (1.0), vertex count.
 *  - per vertex: one xyzw position per shape (s16, 1/4096 units), a normal (s16 xyzw), a uv
 *    (s16, 1/4096) and an RGBA colour (u8, 0x80 = full).
 *  - animation: tag 1, frame length, speed (float), play offset, frame count; then per frame a
 *    shape id and a key count followed by that many (time, weight) float pairs.
 *  - texture: when bit 3 of the type is set, a u32 byte count and run-length data, else 128x128
 *    raw pixels. Pixels are 16-bit, 5 bits each of red, green, blue.
 *
 * Icon space has y pointing down; the icon stands on y = 0 and extends toward negative y.
 */
class Ps2Icon(
    val shapeCount: Int,
    val vertexCount: Int,
    /** Positions, [shape][vertex][xyz] flattened. */
    val positions: FloatArray,
    /** Normals, [vertex][xyz]. */
    val normals: FloatArray,
    /** Texture coordinates, [vertex][uv], 0..1. */
    val uvs: FloatArray,
    /** Vertex colours as 0..1 floats, [vertex][rgb]. */
    val colors: FloatArray,
    val frameLength: Int,
    val animSpeed: Float,
    val playOffset: Int,
    val frames: List<Frame>,
    /** 128x128 texels as 0xRRGGBB, or null when the icon carries none. */
    val texture: IntArray?,
) {
    class Frame(val shape: Int, val times: FloatArray, val weights: FloatArray)

    val animated: Boolean get() = shapeCount > 1 && frames.isNotEmpty() && frameLength > 1

    companion object {
        const val TEXTURE_SIZE = 128
        private const val MAX_VERTICES = 60_000

        fun parse(b: ByteArray?): Ps2Icon? = runCatching { parseOrThrow(b ?: return null) }.getOrNull()

        private fun parseOrThrow(b: ByteArray): Ps2Icon? {
            val bb = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN)
            if (b.size < 20 || bb.getInt(0) != 0x00010000) return null
            val shapes = bb.getInt(4)
            val texType = bb.getInt(8)
            val nv = bb.getInt(16)
            if (shapes !in 1..64 || nv !in 3..MAX_VERTICES) return null
            val per = shapes * 8 + 16
            if (20L + nv.toLong() * per > b.size) return null

            val positions = FloatArray(shapes * nv * 3)
            val normals = FloatArray(nv * 3)
            val uvs = FloatArray(nv * 2)
            val colors = FloatArray(nv * 3)
            var o = 20
            for (v in 0 until nv) {
                for (s in 0 until shapes) {
                    val p = (s * nv + v) * 3
                    positions[p] = bb.getShort(o) / 4096f
                    positions[p + 1] = bb.getShort(o + 2) / 4096f
                    positions[p + 2] = bb.getShort(o + 4) / 4096f
                    o += 8
                }
                normals[v * 3] = bb.getShort(o) / 4096f
                normals[v * 3 + 1] = bb.getShort(o + 2) / 4096f
                normals[v * 3 + 2] = bb.getShort(o + 4) / 4096f
                o += 8
                uvs[v * 2] = bb.getShort(o) / 4096f
                uvs[v * 2 + 1] = bb.getShort(o + 2) / 4096f
                o += 4
                for (c in 0 until 3) colors[v * 3 + c] = ((b[o + c].toInt() and 0xFF) / 128f).coerceAtMost(2f)
                o += 4
            }

            // Animation. An icon with no usable animation still draws: it just holds shape 0.
            var frameLength = 0
            var speed = 1f
            var offset = 0
            val frames = ArrayList<Frame>()
            if (o + 20 <= b.size && bb.getInt(o) == 1) {
                frameLength = bb.getInt(o + 4)
                speed = bb.getFloat(o + 8)
                offset = bb.getInt(o + 12)
                val count = bb.getInt(o + 16)
                o += 20
                for (f in 0 until count.coerceIn(0, 1024)) {
                    if (o + 8 > b.size) break
                    val shape = bb.getInt(o)
                    val keys = bb.getInt(o + 4)
                    o += 8
                    if (keys < 0 || keys > 4096 || o + keys * 8 > b.size) break
                    frames += Frame(
                        shape,
                        FloatArray(keys) { bb.getFloat(o + it * 8) },
                        FloatArray(keys) { bb.getFloat(o + it * 8 + 4) },
                    )
                    o += keys * 8
                }
            }

            return Ps2Icon(
                shapeCount = shapes,
                vertexCount = nv - nv % 3,
                positions = positions,
                normals = normals,
                uvs = uvs,
                colors = colors,
                frameLength = frameLength,
                animSpeed = if (speed.isFinite() && speed > 0f) speed else 1f,
                playOffset = offset,
                frames = frames,
                texture = texture(b, o, texType),
            )
        }

        private fun texture(b: ByteArray, start: Int, type: Int): IntArray? {
            val n = TEXTURE_SIZE * TEXTURE_SIZE
            val px = IntArray(n)
            var o = start
            fun u16(at: Int) = (b[at].toInt() and 0xFF) or ((b[at + 1].toInt() and 0xFF) shl 8)
            if (type and 8 != 0) {
                if (o + 4 > b.size) return null
                val size = Ps2MemoryCard.u32(b, o)
                o += 4
                val end = minOf(b.size, o + maxOf(0, size))
                var i = 0
                // Run-length: a code with the top bit set is followed by (0x10000 - code) literal
                // pixels; any other code repeats the one pixel after it that many times. A zero
                // code is padding with no pixel after it: Namco's encoder (Katamari Damacy) puts
                // one after every run and Sonic Riders has one mid-stream, and reading a pixel
                // there threw the rest of each texture out of step.
                while (i < n && o + 2 <= end) {
                    val code = u16(o)
                    o += 2
                    if (code == 0) continue
                    if (code and 0x8000 != 0) {
                        var count = 0x10000 - code
                        while (count-- > 0 && i < n && o + 2 <= end) { px[i++] = rgb(u16(o)); o += 2 }
                    } else {
                        if (o + 2 > end) break
                        val c = rgb(u16(o))
                        o += 2
                        var count = code
                        while (count-- > 0 && i < n) px[i++] = c
                    }
                }
                return if (i > 0) px else null
            }
            if (o + n * 2 > b.size) return null
            for (i in 0 until n) px[i] = rgb(u16(o + i * 2))
            return px
        }

        /** 16-bit texel, 5 bits each of red, green and blue from the low end, to 0xRRGGBB. */
        private fun rgb(p: Int): Int {
            val r = (p and 31) * 255 / 31
            val g = ((p shr 5) and 31) * 255 / 31
            val b = ((p shr 10) and 31) * 255 / 31
            return (r shl 16) or (g shl 8) or b
        }
    }
}
