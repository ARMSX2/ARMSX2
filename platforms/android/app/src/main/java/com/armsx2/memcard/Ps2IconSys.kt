package com.armsx2.memcard

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * A save's icon.sys: how the PS2 browser shows it. Its title, the four corner colours of the
 * background gradient, the three lights and the ambient light the icon is shaded with, and which
 * icon file to draw (the normal one, and the ones for the copy and delete animations).
 */
class Ps2IconSys(
    val title: String,
    /** Background colours as 0xRRGGBB, in the order upper-left, upper-right, lower-left, lower-right. */
    val background: IntArray,
    /** How opaque that background is over the browser's own, 0..128. */
    val backgroundAlpha: Int,
    /** Three light directions, xyz each, in icon space. */
    val lightDirections: Array<FloatArray>,
    /** Three light colours, rgb each, 0..1. */
    val lightColors: Array<FloatArray>,
    val ambient: FloatArray,
    val iconNormal: String,
    val iconCopy: String,
    val iconDelete: String,
) {
    companion object {
        const val SIZE = 964

        fun parse(b: ByteArray?): Ps2IconSys? {
            if (b == null || b.size < SIZE || String(b, 0, 4, Charsets.US_ASCII) != "PS2D") return null
            val bb = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN)
            // Colours are stored per channel on the PS2's 0..128 scale, 128 being full intensity.
            fun channel(o: Int) = (bb.getInt(o).coerceIn(0, 255) * 255 / 128).coerceAtMost(255)
            val background = IntArray(4) { i ->
                val o = 0x10 + i * 16
                (channel(o) shl 16) or (channel(o + 4) shl 8) or channel(o + 8)
            }
            fun vec(o: Int) = floatArrayOf(bb.getFloat(o), bb.getFloat(o + 4), bb.getFloat(o + 8))
            return Ps2IconSys(
                title = title(b),
                background = background,
                backgroundAlpha = bb.getInt(0x0C).coerceIn(0, 128),
                lightDirections = Array(3) { vec(0x50 + it * 16) },
                lightColors = Array(3) { vec(0x80 + it * 16) },
                ambient = vec(0xB0),
                iconNormal = name(b, 0x104),
                iconCopy = name(b, 0x144),
                iconDelete = name(b, 0x184),
            )
        }

        private fun name(b: ByteArray, o: Int): String {
            var end = o
            while (end < o + 64 && b[end].toInt() != 0) end++
            return String(b, o, end - o, Charsets.US_ASCII)
        }

        /** The title, in Shift-JIS on the card, usually full-width ("Ｓｐｉｄｅｒ－Ｍａｎ"). NFKC brings
         *  that back to ordinary text. The u16 at 0x06 is where the second line starts. */
        private fun title(b: ByteArray): String = runCatching {
            // A zero byte ends it: no Shift-JIS character contains one, and some games leave junk
            // after the end (a Resident Evil 4 save does).
            var end = 0xC0
            while (end < 0xC0 + 68 && b[end].toInt() != 0) end++
            val raw = b.copyOfRange(0xC0, end)
            val split = (Ps2MemoryCard.u16(b, 0x06)).coerceIn(0, raw.size)
            val sjis = java.nio.charset.Charset.forName("Shift_JIS")
            val first = String(raw, 0, split, sjis)
            val second = String(raw, split, raw.size - split, sjis)
            val joined = if (second.isBlank() || first.isBlank()) first + second else "$first $second"
            java.text.Normalizer.normalize(joined, java.text.Normalizer.Form.NFKC).trim().replace(Regex("""\s+"""), " ")
        }.getOrDefault("")
    }
}
