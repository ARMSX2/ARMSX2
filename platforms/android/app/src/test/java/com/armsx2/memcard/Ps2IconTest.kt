package com.armsx2.memcard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class Ps2IconTest {
    /** A one-triangle icon whose texture is [rle] (compressed, type 0xF). */
    private fun icon(rle: ShortArray): ByteArray {
        val b = ByteBuffer.allocate(20 + 3 * 24 + 20 + 4 + rle.size * 2).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(0x00010000).putInt(1).putInt(0xF).putFloat(1f).putInt(3)
        repeat(3) { v ->
            b.putShort((v * 4096).toShort()).putShort((-4096).toShort()).putShort(0).putShort(0) // position
            b.putShort(0).putShort((-4096).toShort()).putShort(0).putShort(0) // normal
            b.putShort(0).putShort(0) // uv
            b.put(0x80.toByte()).put(0x80.toByte()).put(0x80.toByte()).put(0x80.toByte()) // colour
        }
        b.putInt(1).putInt(1).putFloat(1f).putInt(0).putInt(0) // animation: no frames
        b.putInt(rle.size * 2)
        for (s in rle) b.putShort(s)
        return b.array()
    }

    private val red = 0x001F.toShort() // 5 bits of red
    private val blue = 0x7C00.toShort()

    @Test
    fun runsAndLiteralsFillTheTexture() {
        // 16382 red, then two literal pixels: blue, red.
        val tex = Ps2Icon.parse(icon(shortArrayOf(16382, red, 0xFFFE.toShort(), blue, red)))?.texture
        assertNotNull(tex)
        assertEquals(0xFF0000, tex!![0])
        assertEquals(0x0000FF, tex[16382])
        assertEquals(0xFF0000, tex[16383])
    }

    @Test
    fun zeroCodesArePaddingNotRuns() {
        // Namco's layout: every run followed by a zero word, the last one's left off.
        val tex = Ps2Icon.parse(icon(shortArrayOf(8192, red, 0, 8192, blue)))?.texture
        assertNotNull(tex)
        assertEquals(0xFF0000, tex!![8191])
        assertEquals(0x0000FF, tex[8192])
        assertEquals(0x0000FF, tex[16383])
    }
}
