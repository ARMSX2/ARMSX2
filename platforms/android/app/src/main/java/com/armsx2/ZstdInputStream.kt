package com.armsx2

import kr.co.iefriends.pcsx2.NativeApp
import java.io.IOException
import java.io.InputStream

/**
 * Streaming zstd decompression over the strict single-frame JNI decoder (plan 2026-09-06-0905,
 * R12-R13). Wraps one decoder handle for the lifetime of this stream.
 *
 * Contract enforced here, on top of the native limits:
 *  - exactly ONE frame; after it completes, the underlying stream must be at physical EOF — one
 *    more byte means trailing compressed data and fails the stream
 *  - total decompressed output is capped at [maxOutputBytes] natively; the caller checks
 *    [producedBytes] against the catalog's exact decompressed size at end of stream
 *  - a decoder error poisons the handle; every later read throws, and [close] still runs exactly
 *    one destroy (idempotent, safe from any path including failure and cancellation)
 *
 * Unconsumed input stays buffered natively across calls, so callers may feed arbitrary chunk
 * sizes; the JNI layer caps each call at 256 KiB in either direction, which is also what bounds
 * cancellation latency in the extractor.
 */
internal class ZstdInputStream(
    private val src: InputStream,
    maxOutputBytes: Long,
) : InputStream() {
    private val handle: Long = NativeApp.zstdDecoderCreate(maxOutputBytes)
    private val inBuf = ByteArray(CHUNK_BYTES)
    private val outBuf = ByteArray(CHUNK_BYTES)
    private var inLen = 0
    private var inPos = 0
    private var outPos = 0
    private var outEnd = 0
    private var frameDone = false
    private var eofVerified = false
    private var closed = false
    private var failure: String? = null
    private var produced = 0L

    val producedBytes: Long get() = produced

    private fun fail(why: String): Int {
        if (failure == null) failure = why
        throw IOException("zstd: $why")
    }

    private fun ensureOpen() {
        if (closed) throw IOException("zstd: stream closed")
        if (failure != null) throw IOException("zstd: ${failure}")
    }

    override fun read(): Int {
        val buf = ByteArray(1)
        return if (read(buf, 0, 1) < 0) -1 else buf[0].toInt() and 0xff
    }

    override fun read(b: ByteArray, off: Int, len: Int): Int {
        ensureOpen()
        if (len == 0) return 0
        while (true) {
            if (outPos < outEnd) {
                val n = minOf(len, outEnd - outPos)
                System.arraycopy(outBuf, outPos, b, off, n)
                outPos += n
                return n
            }
            if (frameDone) {
                if (eofVerified) return -1
                // Physical EOF must land exactly at the frame boundary; anything more is data we
                // refuse to interpret.
                eofVerified = true
                if (src.read(inBuf, 0, 1) >= 0) fail("trailing compressed bytes after frame")
                return -1
            }
            fillAndDecode()
        }
    }

    private fun fillAndDecode() {
        val status = LongArray(3)
        // Feed buffered (possibly unconsumed) input; drain output with what room remains.
        val outRoom = outBuf.size
        val producedNow: Int
        if (inPos < inLen) {
            producedNow = NativeApp.zstdDecoderDecode(
                handle, inBuf, inPos, inLen - inPos, outBuf, 0, outRoom, status,
            )
        } else {
            val n = src.read(inBuf, 0, inBuf.size)
            if (n < 0) fail("truncated frame: unexpected end of compressed input")
            inPos = 0
            inLen = n
            producedNow = NativeApp.zstdDecoderDecode(
                handle, inBuf, 0, inLen, outBuf, 0, outRoom, status,
            )
        }
        if (producedNow < 0) fail(failureMessageFromNative())
        inPos += status[0].toInt()
        outPos = 0
        outEnd = producedNow
        produced += producedNow
        if (status[2] == 1L) {
            if (status[1] == 0L && produced == 0L) fail("frame completed with no output")
            frameDone = true
        } else if (producedNow == 0 && status[0] == 0L) {
            fail("decoder made no progress")
        }
    }

    private fun failureMessageFromNative(): String =
        when {
            produced > 0 || inLen > 0 -> "decoder rejected the stream (corrupt, truncated, trailing data, or output cap)"
            else -> "decoder rejected the stream"
        }

    override fun close() {
        if (closed) return
        closed = true
        try {
            src.close()
        } finally {
            // Exactly one destroy per create, on every path, even after poison or cancellation.
            if (handle != 0L) NativeApp.zstdDecoderDestroy(handle)
        }
    }

    companion object {
        private const val CHUNK_BYTES = 256 * 1024
    }
}
