package com.armsx2.memcard

import java.io.IOException

/**
 * FLAC, as CHD's "cdfl" codec uses it: a hunk's sector bytes taken as 16-bit stereo samples at
 * 44.1 kHz and FLAC-compressed with a fixed block size. chdman picks whichever codec comes out
 * smallest for each hunk, so data hunks occasionally end up here too (one of Batman Vengeance's
 * 45,000, among its file-system sectors), which is why a data reader needs it.
 *
 * The stream in a hunk is bare FLAC frames; libchdr puts a STREAMINFO header in front for libFLAC,
 * which only matters for the frames' "use the stream's value" codes (16 bits, 44.1 kHz). Every
 * subframe type is decoded (constant, verbatim, fixed, LPC) with any channel decorrelation.
 */
internal object Flac {
    /** Decodes the FLAC frames at the start of [src] until [out] (big-endian 16-bit interleaved
     *  stereo, as libchdr writes it on a little-endian machine) is full; returns the bytes used. */
    fun decodeChdHunk(src: ByteArray, out: ByteArray, blockSize: Int): Int {
        val bits = Bits(src)
        val totalSamples = out.size / 4
        var done = 0
        while (done < totalSamples) {
            val (n, channels) = frame(bits, blockSize)
            val take = n.coerceAtMost(totalSamples - done)
            for (i in 0 until take) {
                for (c in 0 until 2) {
                    val s = channels[c][i]
                    val o = ((done + i) * 2 + c) * 2
                    out[o] = (s shr 8).toByte()
                    out[o + 1] = s.toByte()
                }
            }
            done += take
        }
        return bits.bytePosition()
    }

    private fun frame(bits: Bits, streamBlockSize: Int): Pair<Int, Array<IntArray>> {
        if (bits.read(14) != 0x3FFE) throw IOException("FLAC frame sync not found")
        bits.read(1) // reserved
        bits.read(1) // blocking strategy
        val blockCode = bits.read(4)
        val rateCode = bits.read(4)
        val channelCode = bits.read(4)
        val sizeCode = bits.read(3)
        bits.read(1) // reserved
        // Frame or sample number, UTF-8 style: the leading byte says how many follow.
        val lead = bits.read(8)
        val extra = when {
            lead and 0x80 == 0 -> 0
            lead and 0xE0 == 0xC0 -> 1
            lead and 0xF0 == 0xE0 -> 2
            lead and 0xF8 == 0xF0 -> 3
            lead and 0xFC == 0xF8 -> 4
            lead and 0xFE == 0xFC -> 5
            lead == 0xFE -> 6
            else -> throw IOException("FLAC frame number malformed")
        }
        repeat(extra) { bits.read(8) }
        val blockSize = when (blockCode) {
            0 -> streamBlockSize
            1 -> 192
            in 2..5 -> 576 shl (blockCode - 2)
            6 -> bits.read(8) + 1
            7 -> bits.read(16) + 1
            else -> 256 shl (blockCode - 8)
        }
        when (rateCode) {
            12 -> bits.read(8)
            13, 14 -> bits.read(16)
        }
        val sampleBits = when (sizeCode) {
            0 -> 16
            1 -> 8
            2 -> 12
            4 -> 16
            5 -> 20
            6 -> 24
            7 -> 32
            else -> throw IOException("FLAC sample size reserved")
        }
        bits.read(8) // header CRC-8

        val channels = if (channelCode < 8) channelCode + 1 else 2
        if (channels != 2) throw IOException("FLAC CHD hunks are stereo, not $channels channels")
        val data = Array(2) { IntArray(blockSize) }
        for (c in 0 until 2) {
            // The side channel carries one bit more.
            val side = (channelCode == 8 && c == 1) || (channelCode == 9 && c == 0) || (channelCode == 10 && c == 1)
            subframe(bits, data[c], blockSize, sampleBits + if (side) 1 else 0)
        }
        when (channelCode) {
            8 -> for (i in 0 until blockSize) data[1][i] = data[0][i] - data[1][i] // left, side
            9 -> for (i in 0 until blockSize) data[0][i] = data[0][i] + data[1][i] // side, right
            10 -> for (i in 0 until blockSize) { // mid, side
                val side = data[1][i]
                val mid = (data[0][i] shl 1) or (side and 1)
                data[0][i] = (mid + side) shr 1
                data[1][i] = (mid - side) shr 1
            }
        }
        bits.alignToByte()
        bits.read(16) // frame CRC-16
        return blockSize to data
    }

    private fun subframe(bits: Bits, out: IntArray, n: Int, sampleBits: Int) {
        if (bits.read(1) != 0) throw IOException("FLAC subframe padding bit set")
        val type = bits.read(6)
        var wasted = 0
        if (bits.read(1) == 1) {
            wasted = 1
            while (bits.read(1) == 0) wasted++
        }
        val bps = sampleBits - wasted
        when {
            type == 0 -> out.fill(bits.readSigned(bps), 0, n)
            type == 1 -> for (i in 0 until n) out[i] = bits.readSigned(bps)
            type in 8..12 -> {
                val order = type - 8
                for (i in 0 until order) out[i] = bits.readSigned(bps)
                residual(bits, out, n, order)
                for (i in order until n) {
                    out[i] += when (order) {
                        0 -> 0
                        1 -> out[i - 1]
                        2 -> 2 * out[i - 1] - out[i - 2]
                        3 -> 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3]
                        else -> 4 * out[i - 1] - 6 * out[i - 2] + 4 * out[i - 3] - out[i - 4]
                    }
                }
            }
            type >= 32 -> {
                val order = type - 31
                for (i in 0 until order) out[i] = bits.readSigned(bps)
                val precision = bits.read(4) + 1
                if (precision == 16) throw IOException("FLAC LPC precision invalid")
                val shift = bits.readSigned(5)
                val coefs = IntArray(order) { bits.readSigned(precision) }
                residual(bits, out, n, order)
                for (i in order until n) {
                    var sum = 0L
                    for (j in 0 until order) sum += coefs[j].toLong() * out[i - j - 1]
                    out[i] += (sum shr shift).toInt()
                }
            }
            else -> throw IOException("FLAC subframe type $type reserved")
        }
        if (wasted > 0) for (i in 0 until n) out[i] = out[i] shl wasted
    }

    /** Rice-coded residual into out[order until n], to be added to the prediction. */
    private fun residual(bits: Bits, out: IntArray, n: Int, order: Int) {
        val method = bits.read(2)
        if (method > 1) throw IOException("FLAC residual method reserved")
        val paramBits = if (method == 0) 4 else 5
        val escape = (1 shl paramBits) - 1
        val partitionOrder = bits.read(4)
        val partitions = 1 shl partitionOrder
        var i = order
        for (p in 0 until partitions) {
            val count = (n shr partitionOrder) - if (p == 0) order else 0
            val param = bits.read(paramBits)
            if (param == escape) {
                val raw = bits.read(5)
                repeat(count) { out[i++] = if (raw == 0) 0 else bits.readSigned(raw) }
            } else {
                repeat(count) {
                    var q = 0
                    while (bits.read(1) == 0) q++
                    val v = (q shl param) or (if (param > 0) bits.read(param) else 0)
                    out[i++] = (v ushr 1) xor -(v and 1)
                }
            }
        }
    }

    /** MSB-first bits of a byte array. */
    private class Bits(private val data: ByteArray) {
        private var pos = 0 // in bits

        fun read(n: Int): Int {
            var v = 0
            for (k in 0 until n) {
                val byte = pos ushr 3
                if (byte >= data.size) throw IOException("FLAC input ended early")
                v = (v shl 1) or ((data[byte].toInt() shr (7 - (pos and 7))) and 1)
                pos++
            }
            return v
        }

        fun readSigned(n: Int): Int {
            if (n == 0) return 0
            val v = read(n)
            return if (n < 32 && v and (1 shl (n - 1)) != 0) v - (1 shl n) else v
        }

        fun alignToByte() {
            pos = (pos + 7) and 7.inv()
        }

        fun bytePosition(): Int = (pos + 7) ushr 3
    }
}
