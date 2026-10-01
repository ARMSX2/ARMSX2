package com.armsx2.memcard

import java.io.IOException

/**
 * A raw LZMA decoder (no stream header), for the LZMA-compressed hunks of a CHD. It follows the
 * reference decoder in Igor Pavlov's LZMA specification, and decodes straight into the output:
 * a hunk is smaller than the dictionary it was compressed with, so the output is the whole window.
 *
 * Kotlin has no unsigned 32-bit arithmetic on Int, so the range coder's comparisons are unsigned
 * compares; additions and subtractions wrap exactly as the reference's UInt32 does.
 */
internal class Lzma private constructor(
    private val src: ByteArray,
    private var inPos: Int,
    private val inEnd: Int,
    private val out: ByteArray,
    private val lc: Int,
    private val lp: Int,
    private val pb: Int,
) {
    private var range = -1 // 0xFFFFFFFF
    private var code = 0
    private var outPos = 0

    private val literal = probs(0x300 shl (lc + lp))
    private val isMatch = probs(STATES shl POS_BITS_MAX)
    private val isRep = probs(STATES)
    private val isRepG0 = probs(STATES)
    private val isRepG1 = probs(STATES)
    private val isRepG2 = probs(STATES)
    private val isRep0Long = probs(STATES shl POS_BITS_MAX)
    private val posSlot = probs(LEN_TO_POS_STATES shl 6)
    private val posDecoders = probs(1 + FULL_DISTANCES - END_POS_MODEL_INDEX)
    private val align = probs(1 shl ALIGN_BITS)
    private val matchLen = LenDecoder()
    private val repLen = LenDecoder()

    private inner class LenDecoder {
        val choice = probs(2)
        val low = probs(1 shl (POS_BITS_MAX + 3))
        val mid = probs(1 shl (POS_BITS_MAX + 3))
        val high = probs(256)

        fun decode(posState: Int): Int = when {
            bit(choice, 0) == 0 -> tree(low, posState shl 3, 3)
            bit(choice, 1) == 0 -> 8 + tree(mid, posState shl 3, 3)
            else -> 16 + tree(high, 0, 8)
        }
    }

    // Past the end reads as zero, as in the reference: the range coder may look a byte or two
    // beyond the last symbol it needs.
    private fun nextByte(): Int = if (inPos < inEnd) src[inPos++].toInt() and 0xFF else 0

    private fun normalize() {
        if (Integer.compareUnsigned(range, TOP) < 0) {
            range = range shl 8
            code = (code shl 8) or nextByte()
        }
    }

    private fun bit(probs: ShortArray, i: Int): Int {
        val p = probs[i].toInt()
        val bound = (range ushr BIT_MODEL_TOTAL_BITS) * p
        return if (Integer.compareUnsigned(code, bound) < 0) {
            range = bound
            probs[i] = (p + ((BIT_MODEL_TOTAL - p) ushr MOVE_BITS)).toShort()
            normalize()
            0
        } else {
            range -= bound
            code -= bound
            probs[i] = (p - (p ushr MOVE_BITS)).toShort()
            normalize()
            1
        }
    }

    private fun directBits(count: Int): Int {
        var res = 0
        repeat(count) {
            range = range ushr 1
            code -= range
            val t = 0 - (code ushr 31)
            code += range and t
            normalize()
            res = (res shl 1) + (t + 1)
        }
        return res
    }

    private fun tree(probs: ShortArray, base: Int, bits: Int): Int {
        var m = 1
        repeat(bits) { m = (m shl 1) + bit(probs, base + m) }
        return m - (1 shl bits)
    }

    private fun reverseTree(probs: ShortArray, base: Int, bits: Int): Int {
        var m = 1
        var symbol = 0
        for (i in 0 until bits) {
            val b = bit(probs, base + m)
            m = (m shl 1) + b
            symbol = symbol or (b shl i)
        }
        return symbol
    }

    private fun literal(state: Int, rep0: Int) {
        val prev = if (outPos > 0) out[outPos - 1].toInt() and 0xFF else 0
        val litState = ((outPos and ((1 shl lp) - 1)) shl lc) + (prev ushr (8 - lc))
        val base = 0x300 * litState
        var symbol = 1
        if (state >= 7) {
            var matchByte = out[outPos - rep0 - 1].toInt() and 0xFF
            do {
                val matchBit = (matchByte ushr 7) and 1
                matchByte = matchByte shl 1
                val b = bit(literal, base + ((1 + matchBit) shl 8) + symbol)
                symbol = (symbol shl 1) or b
                if (matchBit != b) break
            } while (symbol < 0x100)
        }
        while (symbol < 0x100) symbol = (symbol shl 1) or bit(literal, base + symbol)
        out[outPos++] = (symbol - 0x100).toByte()
    }

    private fun distance(len: Int): Int {
        val lenState = len.coerceAtMost(LEN_TO_POS_STATES - 1)
        val slot = tree(posSlot, lenState shl 6, 6)
        if (slot < 4) return slot
        val numDirect = (slot ushr 1) - 1
        var dist = (2 or (slot and 1)) shl numDirect
        if (slot < END_POS_MODEL_INDEX) {
            dist += reverseTree(posDecoders, dist - slot, numDirect)
        } else {
            dist += directBits(numDirect - ALIGN_BITS) shl ALIGN_BITS
            dist += reverseTree(align, 0, ALIGN_BITS)
        }
        return dist
    }

    private fun run() {
        if (nextByte() != 0) throw IOException("LZMA stream starts badly")
        repeat(4) { code = (code shl 8) or nextByte() }
        var state = 0
        var rep0 = 0; var rep1 = 0; var rep2 = 0; var rep3 = 0
        while (outPos < out.size) {
            val posState = outPos and ((1 shl pb) - 1)
            if (bit(isMatch, (state shl POS_BITS_MAX) + posState) == 0) {
                literal(state, rep0)
                state = if (state < 4) 0 else if (state < 10) state - 3 else state - 6
                continue
            }
            var len: Int
            if (bit(isRep, state) != 0) {
                if (outPos == 0) throw IOException("LZMA repeat before any output")
                if (bit(isRepG0, state) == 0) {
                    if (bit(isRep0Long, (state shl POS_BITS_MAX) + posState) == 0) {
                        state = if (state < 7) 9 else 11
                        out[outPos] = out[outPos - rep0 - 1]
                        outPos++
                        continue
                    }
                } else {
                    val dist: Int
                    if (bit(isRepG1, state) == 0) {
                        dist = rep1
                    } else {
                        if (bit(isRepG2, state) == 0) {
                            dist = rep2
                        } else {
                            dist = rep3
                            rep3 = rep2
                        }
                        rep2 = rep1
                    }
                    rep1 = rep0
                    rep0 = dist
                }
                len = repLen.decode(posState)
                state = if (state < 7) 8 else 11
            } else {
                rep3 = rep2; rep2 = rep1; rep1 = rep0
                len = matchLen.decode(posState)
                state = if (state < 7) 7 else 10
                rep0 = distance(len)
                if (rep0 == -1) return // end marker
                if (rep0 < 0 || rep0 >= outPos) throw IOException("LZMA distance past the output")
            }
            len += MATCH_MIN_LEN
            var from = outPos - rep0 - 1
            val n = len.coerceAtMost(out.size - outPos)
            repeat(n) { out[outPos++] = out[from++] }
        }
    }

    companion object {
        private const val BIT_MODEL_TOTAL_BITS = 11
        private const val BIT_MODEL_TOTAL = 1 shl BIT_MODEL_TOTAL_BITS
        private const val MOVE_BITS = 5
        private const val TOP = 1 shl 24
        private const val POS_BITS_MAX = 4
        private const val STATES = 12
        private const val LEN_TO_POS_STATES = 4
        private const val ALIGN_BITS = 4
        private const val END_POS_MODEL_INDEX = 14
        private const val FULL_DISTANCES = 1 shl (END_POS_MODEL_INDEX shr 1)
        private const val MATCH_MIN_LEN = 2

        private fun probs(n: Int) = ShortArray(n) { (BIT_MODEL_TOTAL / 2).toShort() }

        /** Decodes [srcLen] bytes of raw LZMA at [srcOff] until [out] is full. CHD's LZMA hunks
         *  use lc=3, lp=0, pb=2, the defaults of the level they are written at. */
        fun decode(src: ByteArray, srcOff: Int, srcLen: Int, out: ByteArray, lc: Int = 3, lp: Int = 0, pb: Int = 2) {
            Lzma(src, srcOff, srcOff + srcLen, out, lc, lp, pb).run()
        }
    }
}
