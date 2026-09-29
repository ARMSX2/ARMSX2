package com.armsx2.memcard

import java.io.IOException
import java.util.zip.Inflater

/**
 * A CHD (MAME's "compressed hunks of data") disc image, version 5, the kind chdman writes for CDs
 * and for PS2 DVDs stored the same way: hunks of 8 frames, each frame a 2352-byte sector plus 96
 * bytes of subcode, compressed hunk by hunk with LZMA ("cdlz"), Deflate ("cdzl") or FLAC ("cdfl").
 *
 * Written from libchdr's reader (the one PCSX2 uses). Only what reading data sectors needs: the
 * subcode and the regenerated sync/ECC bytes of raw sectors are never looked at, so they are not
 * rebuilt, except by [verifyHunk], which rebuilds a hunk whole to check it against its CRC.
 */
class ChdDisc private constructor(
    private val src: ByteSource,
    private val hunkBytes: Int,
    private val unitBytes: Int,
    private val codecs: Array<String>,
    private val types: ByteArray,
    private val lengths: IntArray,
    private val offsets: LongArray,
    private val crcs: ShortArray,
    override val sectorCount: Long,
) : DiscImage {
    private val framesPerHunk = hunkBytes / FRAME
    private var dataOffset = 0
    override var bytesRead = 0L
        private set

    /** Hunks decoded, by codec name (and "none", "self"), for measuring. */
    val decodedByCodec = HashMap<String, Int>()

    // The last few hunks' sector data (framesPerHunk x 2352 bytes each).
    private val cache = object : LinkedHashMap<Int, ByteArray>(32, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<Int, ByteArray>?) = size > 24
    }

    override fun read(lba: Long, count: Int): ByteArray {
        val n = count.toLong().coerceAtMost(sectorCount - lba).coerceAtLeast(0).toInt()
        val out = ByteArray(n * DiscImage.SECTOR)
        for (i in 0 until n) {
            val frame = lba + i
            val data = sectors((frame / framesPerHunk).toInt())
            val from = (frame % framesPerHunk).toInt() * SECTOR_RAW + dataOffset
            System.arraycopy(data, from, out, i * DiscImage.SECTOR, DiscImage.SECTOR)
        }
        return out
    }

    override fun close() = src.close()

    private fun sectors(hunk: Int): ByteArray = cache[hunk] ?: decode(hunk, whole = false).also { cache[hunk] = it }

    /**
     * One hunk: its sectors back to back (framesPerHunk x 2352), or with [whole] the hunk exactly
     * as stored before compression (frames of sector + subcode, sync and ECC restored where the
     * compressor took them out; null when that would need the ECC rebuilt).
     */
    private fun decode(hunk: Int, whole: Boolean): ByteArray {
        val type = types[hunk].toInt()
        when (type) {
            in 0..3 -> {
                val comp = src.read(offsets[hunk], lengths[hunk]).also { bytesRead += it.size }
                val codec = codecs[type]
                decodedByCodec.merge(codec, 1, Int::plus)
                return when (codec) {
                    "cdlz", "cdzl" -> cdBase(comp, lzma = codec == "cdlz", whole)
                    "cdfl" -> cdFlac(comp, whole)
                    else -> throw IOException("CHD codec '$codec' is not supported")
                }
            }
            NONE -> {
                decodedByCodec.merge("none", 1, Int::plus)
                val raw = src.read(offsets[hunk], hunkBytes).also { bytesRead += it.size }
                if (whole) return raw
                val out = ByteArray(framesPerHunk * SECTOR_RAW)
                for (f in 0 until framesPerHunk) System.arraycopy(raw, f * FRAME, out, f * SECTOR_RAW, SECTOR_RAW)
                return out
            }
            SELF -> {
                decodedByCodec.merge("self", 1, Int::plus)
                val target = offsets[hunk].toInt()
                if (target == hunk || target !in types.indices) throw IOException("CHD hunk $hunk refers to itself")
                return if (whole) decode(target, true) else sectors(target)
            }
            else -> throw IOException("CHD hunk $hunk needs a parent CHD")
        }
    }

    // cdlz / cdzl: [ECC bitmap][length of the base stream][base stream: all sectors][subcode stream]
    private fun cdBase(comp: ByteArray, lzma: Boolean, whole: Boolean): ByteArray {
        val eccBytes = (framesPerHunk + 7) / 8
        val lengthBytes = if (hunkBytes < 65536) 2 else 3
        var baseLen = ((comp[eccBytes].toInt() and 0xFF) shl 8) or (comp[eccBytes + 1].toInt() and 0xFF)
        if (lengthBytes > 2) baseLen = (baseLen shl 8) or (comp[eccBytes + 2].toInt() and 0xFF)
        val header = eccBytes + lengthBytes
        val sectors = ByteArray(framesPerHunk * SECTOR_RAW)
        if (lzma) Lzma.decode(comp, header, baseLen, sectors) else inflate(comp, header, baseLen, sectors)
        if (!whole) return sectors
        val subcode = ByteArray(framesPerHunk * SUBCODE)
        inflate(comp, header + baseLen, comp.size - header - baseLen, subcode)
        return interleave(sectors, subcode, ecc = comp, eccStart = 0)
    }

    // cdfl: [FLAC frames: all sectors as 16-bit stereo samples][subcode, Deflate]
    private fun cdFlac(comp: ByteArray, whole: Boolean): ByteArray {
        val sectors = ByteArray(framesPerHunk * SECTOR_RAW)
        val used = Flac.decodeChdHunk(comp, sectors, blockSize = flacBlockSize(sectors.size))
        if (!whole) return sectors
        val subcode = ByteArray(framesPerHunk * SUBCODE)
        inflate(comp, used, comp.size - used, subcode)
        return interleave(sectors, subcode, ecc = null, eccStart = 0)
    }

    /** Frames of sector + subcode, as stored before compression; throws when a frame's sync and
     *  ECC would have to be rebuilt (only [verifyHunk] asks, and it skips those). */
    private fun interleave(sectors: ByteArray, subcode: ByteArray, ecc: ByteArray?, eccStart: Int): ByteArray {
        val out = ByteArray(hunkBytes)
        for (f in 0 until framesPerHunk) {
            if (ecc != null && (ecc[eccStart + f / 8].toInt() shr (f % 8)) and 1 != 0) throw EccRebuildNeeded()
            System.arraycopy(sectors, f * SECTOR_RAW, out, f * FRAME, SECTOR_RAW)
            System.arraycopy(subcode, f * SUBCODE, out, f * FRAME + SECTOR_RAW, SUBCODE)
        }
        return out
    }

    private class EccRebuildNeeded : IOException("ECC would need rebuilding")

    /** Checks hunk [hunk] against the CRC the CHD stores for it: true (matches), false
     *  (doesn't), or null (can't tell without rebuilding ECC, or it is a copy of another). */
    fun verifyHunk(hunk: Int): Boolean? {
        val type = types[hunk].toInt()
        if (type != NONE && type !in 0..3) return null
        val whole = try {
            decode(hunk, whole = true)
        } catch (_: EccRebuildNeeded) {
            return null
        }
        return crc16(whole, 0, whole.size) == (crcs[hunk].toInt() and 0xFFFF)
    }

    val hunkCount: Int get() = types.size

    /** How many hunks use each compression, from the map. */
    fun mapStats(): Map<String, Int> {
        val out = HashMap<String, Int>()
        for (t in types) out.merge(
            when (val i = t.toInt()) { in 0..3 -> codecs[i]; NONE -> "none"; SELF -> "self"; else -> "parent" }, 1, Int::plus,
        )
        return out
    }

    companion object {
        private const val SECTOR_RAW = 2352
        private const val SUBCODE = 96
        private const val FRAME = SECTOR_RAW + SUBCODE

        // Map entry types (libchdr's COMPRESSION_*).
        private const val NONE = 4
        private const val SELF = 5
        private const val PARENT = 6
        private const val RLE_SMALL = 7
        private const val RLE_LARGE = 8
        private const val SELF_0 = 9
        private const val SELF_1 = 10
        private const val PARENT_SELF = 11
        private const val PARENT_0 = 12
        private const val PARENT_1 = 13

        fun open(src: ByteSource): ChdDisc? = runCatching { openOrThrow(src) }.getOrNull()

        private fun openOrThrow(src: ByteSource): ChdDisc? {
            val h = src.read(0, 124)
            if (h.size < 124 || String(h, 0, 8, Charsets.ISO_8859_1) != "MComprHD") return null
            if (be32(h, 12) != 5) return null // v3/v4 are rare for discs; not read
            val codecs = Array(4) { i -> String(h, 16 + i * 4, 4, Charsets.ISO_8859_1).trimEnd('\u0000') }
            val logical = be64(h, 32)
            val mapOffset = be64(h, 40)
            val hunkBytes = be32(h, 56)
            val unitBytes = be32(h, 60)
            if (hunkBytes <= 0 || hunkBytes % FRAME != 0 || unitBytes != FRAME) return null // not a CD-style CHD
            val hunkCount = ((logical + hunkBytes - 1) / hunkBytes).toInt()
            if (codecs.all { it.isEmpty() }) return null // an uncompressed CHD keeps a plain map; not read

            // The map: a header, then Huffman-coded entry types and bit-packed lengths/offsets/CRCs.
            val mh = src.read(mapOffset, 16)
            val mapBytes = be32(mh, 0)
            var cur = be48(mh, 4)
            val mapCrc = be16(mh, 10)
            val lengthBits = mh[12].toInt() and 0xFF
            val selfBits = mh[13].toInt() and 0xFF
            val parentBits = mh[14].toInt() and 0xFF
            val bits = BitReader(src.read(mapOffset + 16, mapBytes))
            val huffman = Huffman(16, 8).apply { importTreeRle(bits) }
            val types = ByteArray(hunkCount)
            var repeat = 0
            var last = 0
            for (i in 0 until hunkCount) {
                if (repeat > 0) {
                    types[i] = last.toByte()
                    repeat--
                    continue
                }
                when (val v = huffman.decode(bits)) {
                    RLE_SMALL -> { types[i] = last.toByte(); repeat = 2 + huffman.decode(bits) }
                    RLE_LARGE -> {
                        types[i] = last.toByte()
                        repeat = 2 + 16 + (huffman.decode(bits) shl 4)
                        repeat += huffman.decode(bits)
                    }
                    else -> { types[i] = v.toByte(); last = v }
                }
            }
            val lengths = IntArray(hunkCount)
            val offsets = LongArray(hunkCount)
            val crcs = ShortArray(hunkCount)
            var lastSelf = 0L
            var lastParent = 0L
            for (i in 0 until hunkCount) {
                var offset = cur
                var length = 0
                var crc = 0
                when (types[i].toInt()) {
                    0, 1, 2, 3 -> { length = bits.read(lengthBits); cur += length; crc = bits.read(16) }
                    NONE -> { length = hunkBytes; cur += length; crc = bits.read(16) }
                    SELF -> { offset = bits.read(selfBits).toLong(); lastSelf = offset }
                    PARENT -> { offset = bits.read(parentBits).toLong(); lastParent = offset }
                    SELF_1, SELF_0 -> {
                        if (types[i].toInt() == SELF_1) lastSelf++
                        types[i] = SELF.toByte()
                        offset = lastSelf
                    }
                    PARENT_SELF -> {
                        types[i] = PARENT.toByte()
                        offset = i.toLong() * hunkBytes / unitBytes
                        lastParent = offset
                    }
                    PARENT_1, PARENT_0 -> {
                        if (types[i].toInt() == PARENT_1) lastParent += hunkBytes / unitBytes
                        types[i] = PARENT.toByte()
                        offset = lastParent
                    }
                }
                offsets[i] = offset
                lengths[i] = length
                crcs[i] = crc.toShort()
            }
            // The map's own CRC covers it laid out as libchdr keeps it: 12 bytes per hunk.
            val raw = ByteArray(hunkCount * 12)
            for (i in 0 until hunkCount) {
                val o = i * 12
                raw[o] = types[i]
                raw[o + 1] = (lengths[i] ushr 16).toByte(); raw[o + 2] = (lengths[i] ushr 8).toByte(); raw[o + 3] = lengths[i].toByte()
                for (k in 0 until 6) raw[o + 4 + k] = (offsets[i] ushr (40 - 8 * k)).toByte()
                raw[o + 10] = (crcs[i].toInt() ushr 8).toByte(); raw[o + 11] = crcs[i].toByte()
            }
            if (crc16(raw, 0, raw.size) != mapCrc) throw IOException("CHD map CRC mismatch")

            val disc = ChdDisc(src, hunkBytes, unitBytes, codecs, types, lengths, offsets, crcs, logical / unitBytes)
            // Where in a frame the 2048 data bytes sit: 0 for cooked sectors (DVDs), 24 for the
            // raw Mode 2 sectors of the PS2's CD games.
            val frame16 = disc.sectors((16 / disc.framesPerHunk))
            disc.dataOffset = DiscImage.dataOffsetOf(frame16, (16 % disc.framesPerHunk) * SECTOR_RAW) ?: return null
            return disc
        }

        private fun inflate(src: ByteArray, off: Int, len: Int, out: ByteArray) {
            val inflater = Inflater(true)
            try {
                inflater.setInput(src, off, len)
                var done = 0
                while (done < out.size) {
                    val n = inflater.inflate(out, done, out.size - done)
                    if (n == 0 && (inflater.finished() || inflater.needsInput() || inflater.needsDictionary())) break
                    done += n
                }
                if (done != out.size) throw IOException("CHD Deflate stream short: $done of ${out.size}")
            } finally {
                inflater.end()
            }
        }

        // libchdr's cdfl_codec_blocksize: a quarter of the bytes, halved until at most a sector.
        private fun flacBlockSize(bytes: Int): Int {
            var b = bytes / 4
            while (b > SECTOR_RAW) b /= 2
            return b
        }

        private fun be16(b: ByteArray, o: Int) = ((b[o].toInt() and 0xFF) shl 8) or (b[o + 1].toInt() and 0xFF)
        private fun be32(b: ByteArray, o: Int) = (be16(b, o) shl 16) or be16(b, o + 2)
        private fun be48(b: ByteArray, o: Int) = (be16(b, o).toLong() shl 32) or (be32(b, o + 2).toLong() and 0xFFFFFFFFL)
        private fun be64(b: ByteArray, o: Int) = (be32(b, o).toLong() shl 32) or (be32(b, o + 4).toLong() and 0xFFFFFFFFL)

        private val CRC16 = IntArray(256) { i ->
            var c = i shl 8
            repeat(8) { c = if (c and 0x8000 != 0) (c shl 1) xor 0x1021 else c shl 1 }
            c and 0xFFFF
        }

        /** CRC-16/CCITT (init 0xFFFF), as CHD uses for its map and hunks. */
        fun crc16(b: ByteArray, off: Int, len: Int): Int {
            var crc = 0xFFFF
            for (i in off until off + len) crc = ((crc shl 8) xor CRC16[((crc ushr 8) xor (b[i].toInt() and 0xFF)) and 0xFF]) and 0xFFFF
            return crc
        }
    }
}

/** MSB-first bit reader, as libchdr's bitstream. */
internal class BitReader(private val data: ByteArray) {
    private var buffer = 0
    private var bits = 0
    private var pos = 0

    fun peek(n: Int): Int {
        if (n == 0) return 0
        if (n > bits) {
            while (bits <= 24) {
                if (pos < data.size) buffer = buffer or ((data[pos].toInt() and 0xFF) shl (24 - bits))
                pos++
                bits += 8
            }
        }
        return buffer ushr (32 - n)
    }

    fun remove(n: Int) {
        buffer = if (n >= 32) 0 else buffer shl n
        bits -= n
    }

    fun read(n: Int): Int = peek(n).also { remove(n) }
}

/** libchdr's canonical Huffman decoder, with its RLE-coded tree. */
internal class Huffman(private val numCodes: Int, private val maxBits: Int) {
    private val lengths = IntArray(numCodes)
    private val codes = IntArray(numCodes)
    private val lookup = IntArray(1 shl maxBits)

    fun importTreeRle(bits: BitReader) {
        val fieldBits = when {
            maxBits >= 16 -> 5
            maxBits >= 8 -> 4
            else -> 3
        }
        var cur = 0
        while (cur < numCodes) {
            var nodeBits = bits.read(fieldBits)
            if (nodeBits != 1) {
                lengths[cur++] = nodeBits
            } else {
                nodeBits = bits.read(fieldBits)
                if (nodeBits == 1) {
                    lengths[cur++] = nodeBits
                } else {
                    var repeat = bits.read(fieldBits) + 3
                    if (repeat + cur > numCodes) throw IOException("CHD Huffman tree overruns")
                    while (repeat-- > 0) lengths[cur++] = nodeBits
                }
            }
        }
        // Canonical codes from the lengths.
        val histogram = IntArray(33)
        for (c in 0 until numCodes) {
            if (lengths[c] > maxBits) throw IOException("CHD Huffman code too long")
            histogram[lengths[c]]++
        }
        var start = 0
        for (len in 32 downTo 1) {
            val next = (start + histogram[len]) shr 1
            if (len != 1 && next * 2 != start + histogram[len]) throw IOException("CHD Huffman tree inconsistent")
            histogram[len] = start
            start = next
        }
        for (c in 0 until numCodes) if (lengths[c] > 0) codes[c] = histogram[lengths[c]]++
        for (c in 0 until numCodes) {
            val n = lengths[c]
            if (n == 0) continue
            val value = (c shl 5) or (n and 0x1F)
            val shift = maxBits - n
            for (i in (codes[c] shl shift) until ((codes[c] + 1) shl shift)) lookup[i] = value
        }
    }

    fun decode(bits: BitReader): Int {
        val v = lookup[bits.peek(maxBits)]
        bits.remove(v and 0x1F)
        return v ushr 5
    }
}
