package com.armsx2.memcard

import java.io.Closeable
import java.nio.ByteBuffer
import java.nio.channels.FileChannel

/** Random access to the bytes of a disc image file, wherever it lives. */
interface ByteSource : Closeable {
    val size: Long

    /** Reads up to [len] bytes at [pos] into [dst]; fewer only at the end of the file. */
    fun read(pos: Long, dst: ByteArray, off: Int, len: Int): Int

    fun read(pos: Long, len: Int): ByteArray {
        val b = ByteArray(len.coerceAtMost((size - pos).coerceIn(0, Int.MAX_VALUE.toLong()).toInt()).coerceAtLeast(0))
        val n = read(pos, b, 0, b.size)
        return if (n == b.size) b else b.copyOf(n)
    }
}

/** A file opened through a channel: a local file, or a content URI's descriptor. */
class ChannelSource(private val channel: FileChannel, private val onClose: () -> Unit = {}) : ByteSource {
    override val size: Long = channel.size()

    override fun read(pos: Long, dst: ByteArray, off: Int, len: Int): Int {
        val buf = ByteBuffer.wrap(dst, off, len)
        var done = 0
        while (done < len) {
            val n = channel.read(buf, pos + done)
            if (n <= 0) break
            done += n
        }
        return done
    }

    override fun close() {
        runCatching { channel.close() }
        onClose()
    }
}

/**
 * A PS2 disc's 2048-byte data sectors, from whatever container holds them: a plain ISO, a raw
 * 2352-byte BIN, or a CHD. Only reading, and only what is asked for.
 */
interface DiscImage : Closeable {
    val sectorCount: Long

    /** [count] sectors from [lba], shorter at the end of the disc. */
    fun read(lba: Long, count: Int): ByteArray

    /** Bytes read from storage so far (compressed bytes, for a CHD). */
    val bytesRead: Long

    /** [length] bytes of the disc starting at byte [pos] (the pos of a file, say, plus an offset). */
    fun readBytes(pos: Long, length: Int): ByteArray {
        if (length <= 0) return ByteArray(0)
        val first = pos / SECTOR
        val last = (pos + length - 1) / SECTOR
        val data = read(first, (last - first + 1).toInt())
        val start = (pos - first * SECTOR).toInt()
        val end = (start + length).coerceAtMost(data.size)
        return if (start >= end) ByteArray(0) else data.copyOfRange(start, end)
    }

    companion object {
        const val SECTOR = 2048

        /** The disc in [source], or null when it is neither a CHD nor an ISO9660 image. Takes
         *  ownership of [source]: closing the disc closes it. */
        fun open(source: ByteSource): DiscImage? {
            val head = source.read(0, 8)
            val disc = if (head.size == 8 && String(head, Charsets.ISO_8859_1) == "MComprHD") ChdDisc.open(source)
                else IsoDisc.open(source)
            if (disc == null) source.close()
            return disc
        }

        /** Where a disc's data sector sits in a CD-sized frame: the offset "CD001" (the ISO9660
         *  volume descriptor at sector 16) turns up at. Cooked 2048-byte sectors have it at 0,
         *  raw Mode 1 at 16, raw Mode 2 (the PS2's CD games) at 24. */
        internal fun dataOffsetOf(frame16: ByteArray, frameStart: Int = 0): Int? =
            intArrayOf(0, 24, 16).firstOrNull { o ->
                val p = frameStart + o
                p + 6 <= frame16.size && frame16[p] == 1.toByte() &&
                    String(frame16, p + 1, 5, Charsets.ISO_8859_1) == "CD001"
            }
    }
}

/** A plain ISO (2048-byte sectors) or a raw BIN (2352-byte sectors, Mode 1 or Mode 2). */
class IsoDisc private constructor(
    private val src: ByteSource,
    private val stride: Int,
    private val dataOffset: Int,
) : DiscImage {
    override val sectorCount: Long = src.size / stride
    override var bytesRead = 0L
        private set

    override fun read(lba: Long, count: Int): ByteArray {
        val n = count.toLong().coerceAtMost(sectorCount - lba).coerceAtLeast(0).toInt()
        if (stride == DiscImage.SECTOR) return src.read(lba * stride, n * stride).also { bytesRead += it.size }
        val raw = src.read(lba * stride, n * stride).also { bytesRead += it.size }
        val out = ByteArray(n * DiscImage.SECTOR)
        for (i in 0 until n) {
            val from = i * stride + dataOffset
            if (from + DiscImage.SECTOR <= raw.size) System.arraycopy(raw, from, out, i * DiscImage.SECTOR, DiscImage.SECTOR)
        }
        return out
    }

    override fun close() = src.close()

    companion object {
        fun open(src: ByteSource): IsoDisc? {
            for (stride in intArrayOf(DiscImage.SECTOR, 2352)) {
                val frame = src.read(16L * stride, stride)
                val offset = DiscImage.dataOffsetOf(frame) ?: continue
                if (stride == DiscImage.SECTOR && offset != 0) continue
                return IsoDisc(src, stride, offset)
            }
            return null
        }
    }
}
