package com.armsx2

import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream

/**
 * Streaming GNU-tar extraction for texture packs (plan 2026-09-06-0905, R14-R17, R23-R24).
 *
 * Accepts exactly the profile the migration publishes: `tar --format=gnu` streams containing the
 * root "./" directory, directories, regular files, and one bounded NUL-terminated GNU `L`
 * long-name record applying to the next entry. Everything else — links, devices, FIFOs, sparse
 * entries, pax records, base-256 fields — rejects the archive. Checksums, padding, the two end
 * blocks, and zero trailing padding are all validated, and the total decompressed byte count must
 * land exactly on [expectedDecompressedBytes] (which also caps what the zstd layer emits).
 *
 * Bytes flow straight from the zstd stream to per-file writes; no uncompressed tar file ever
 * exists. All work chunks are at most 256 KiB with a cancellation check between them.
 */
internal class TarTextureExtractor(
    private val dest: File,
    private val expectedDecompressedBytes: Long,
    private val expectedTextureCount: Int,
    private val onProgress: (done: Int, total: Int) -> Unit,
    private val isCancelled: () -> Boolean,
    raw: InputStream,
) {
    sealed interface Outcome {
        data class Success(val textureCount: Int) : Outcome
        data object Cancelled : Outcome
        data class Failure(val reason: String) : Outcome
    }

    private class Cancelled : Exception()

    private fun throwIfCancelled() {
        if (isCancelled()) throw Cancelled()
    }

    private val counting = CountingInputStream(BufferedInputStream(raw, RECORD))
    private val input = DataInputStream(counting)

    /** Extracts the whole stream. Returns one outcome; never throws (barring cancellation). */
    fun extract(): Outcome {
        var entries = 0
        var textures = 0
        var pendingLongName: String? = null
        val seenFiles = HashSet<String>()
        val seenDirs = HashSet<String>()
        val header = ByteArray(RECORD)
        val destCanonical = dest.canonicalPath + File.separator

        try {
            while (true) {
                throwIfCancelled()
                entries++
                if (entries > MAX_ENTRIES) return Outcome.Failure("tar: entry count exceeds $MAX_ENTRIES")
                input.readFully(header)
                if (header.contentEquals(ZERO_RECORD)) break // first end block

                if (!checksumOk(header)) return Outcome.Failure("tar: header checksum mismatch")
                val magic = String(header, 257, 6, Charsets.US_ASCII)
                if (magic != "ustar " && magic != "ustar\u0000") {
                    return Outcome.Failure("tar: unknown magic '$magic'")
                }
                val typeflag = header[156].toInt() and 0xff
                val size = parseOctal(header, 124, 12)
                    ?: return Outcome.Failure("tar: malformed size field (or base-256)")

                if (typeflag == TYPE_LONGNAME) {
                    if (pendingLongName != null) {
                        return Outcome.Failure("tar: GNU L record without a following entry")
                    }
                    if (size <= 0L || size > MAX_LONGNAME_BYTES) {
                        return Outcome.Failure("tar: GNU L payload must be 1..$MAX_LONGNAME_BYTES bytes")
                    }
                    val payload = ByteArray(size.toInt())
                    input.readFully(payload)
                    skipPadding(size)
                    if (payload[size.toInt() - 1] != 0.toByte()) {
                        return Outcome.Failure("tar: GNU L payload not NUL-terminated")
                    }
                    pendingLongName = String(payload, 0, payload.size - 1, Charsets.UTF_8)
                    continue
                }

                // GNU tar writes regular files with typeflag '0' (or NUL for old archives).
                val isDir = typeflag == TYPE_DIR
                val isRegular = typeflag == TYPE_FILE || typeflag == TYPE_FILE_NAME
                if (!isDir && !isRegular) {
                    return Outcome.Failure(
                        "tar: unsupported entry type ${typeflag.toChar()} " +
                            "(links/devices/pax are outside the producer contract)",
                    )
                }

                val name = pendingLongName
                    ?: run {
                        // Short name field: NUL-terminated within 100 bytes, or all 100.
                        var end = 0
                        while (end < 100 && header[end] != 0.toByte()) end++
                        String(header, 0, end, Charsets.UTF_8)
                    }
                pendingLongName = null

                val path = TextureArchivePath.validateTarPath(name)
                    ?: return Outcome.Failure("tar: rejected path ${name.take(80)}")
                if (path.isEmpty()) {
                    // The producer's root "./" directory; nothing to register, nothing to skip.
                    if (!isDir || size != 0L) return Outcome.Failure("tar: empty name on a non-root entry")
                    continue
                }

                val key = TextureArchivePath.collisionKey(path)
                if (key in seenFiles || key in seenDirs) {
                    return Outcome.Failure("tar: duplicate or colliding destination $path")
                }
                val parts = path.split("/")
                for (i in 1 until parts.size) {
                    val ancestor = TextureArchivePath.collisionKey(parts.subList(0, i).joinToString("/"))
                    if (ancestor in seenFiles) {
                        return Outcome.Failure("tar: file/directory ancestor conflict at ${parts.subList(0, i).joinToString("/")}")
                    }
                }

                if (isDir) {
                    if (size != 0L) return Outcome.Failure("tar: directory entry with payload: $path")
                    seenDirs.add(key)
                    continue
                }

                // Every regular file — texture or not — occupies a destination name, so all of
                // them take part in duplicate and ancestor-conflict detection.
                seenFiles.add(key)

                if (size > MAX_FILE_BYTES) return Outcome.Failure("tar: payload exceeds $MAX_FILE_BYTES bytes: $path")

                if (TextureArchivePath.isJunkEntry(path) || !TextureArchivePath.isTextureFile(path)) {
                    consumePayload(size)
                    continue
                }

                val rel = TextureArchivePath.replacementRelativePath(path)
                    ?: return Outcome.Failure("tar: cannot normalize path $path")
                val out = File(dest, rel)
                if (!out.canonicalPath.startsWith(destCanonical)) {
                    return Outcome.Failure("tar: escaped destination: $rel")
                }
                val relKey = TextureArchivePath.collisionKey(rel)
                // The archive key is already registered above; a second destination key only
                // conflicts when the normalized rel differs from it and someone else claimed it.
                if (relKey != key && relKey in seenFiles) {
                    return Outcome.Failure("tar: duplicate normalized destination $rel")
                }
                seenFiles.add(relKey)
                out.parentFile?.mkdirs()
                FileOutputStream(out).use { fileOut ->
                    var remaining = size
                    val buf = ByteArray(CHUNK_BYTES)
                    while (remaining > 0) {
                        throwIfCancelled()
                        val n = buf.size.toLong().coerceAtMost(remaining).toInt()
                        input.readFully(buf, 0, n)
                        fileOut.write(buf, 0, n)
                        remaining -= n
                    }
                    skipPadding(size)
                }
                textures++
                if (textures % 32 == 0) onProgress(textures, expectedTextureCount)
            }

            // Second end block, then only zero padding out to the blocking factor. The final -1
            // here is also what makes the zstd layer verify physical EOF at the frame boundary.
            val second = ByteArray(RECORD)
            input.readFully(second)
            if (!second.contentEquals(ZERO_RECORD)) return Outcome.Failure("tar: missing second end block")
            val tail = ByteArray(CHUNK_BYTES)
            while (true) {
                throwIfCancelled()
                val n = input.read(tail)
                if (n < 0) break
                for (i in 0 until n) {
                    if (tail[i] != 0.toByte()) return Outcome.Failure("tar: nonzero trailing padding")
                }
            }

            if (counting.bytesRead != expectedDecompressedBytes) {
                return Outcome.Failure(
                    "tar: decompressed size mismatch (stream ${counting.bytesRead}, " +
                        "catalog $expectedDecompressedBytes)",
                )
            }
            if (textures != expectedTextureCount) {
                return Outcome.Failure(
                    "tar: texture count mismatch (stream $textures, catalog $expectedTextureCount)",
                )
            }
            onProgress(textures, expectedTextureCount)
            return Outcome.Success(textures)
        } catch (e: Cancelled) {
            return Outcome.Cancelled
        } catch (e: IOException) {
            return Outcome.Failure("tar: ${e.message ?: "read error"}")
        } catch (e: SecurityException) {
            return Outcome.Failure("tar: ${e.message ?: "filesystem error"}")
        }
    }

    private fun skipPadding(size: Long) {
        val pad = (((size + RECORD - 1) / RECORD) * RECORD - size).toInt()
        if (pad > 0) input.readFully(ByteArray(pad))
    }

    private fun consumePayload(size: Long) {
        var remaining = size
        val buf = ByteArray(CHUNK_BYTES)
        while (remaining > 0) {
            throwIfCancelled()
            val n = buf.size.toLong().coerceAtMost(remaining).toInt()
            input.readFully(buf, 0, n)
            remaining -= n
        }
        skipPadding(size)
    }

    private fun parseOctal(record: ByteArray, off: Int, len: Int): Long? {
        if (record[off].toInt() and 0x80 != 0) return null // GNU base-256: out of contract
        var end = off
        while (end < off + len && record[end] != 0.toByte() && record[end] != ' '.code.toByte()) end++
        var start = off
        while (start < end && record[start] == ' '.code.toByte()) start++
        if (start == end) return 0
        return try {
            String(record, start, end - start, Charsets.US_ASCII).toLong(8)
        } catch (e: NumberFormatException) {
            null
        }
    }

    private fun checksumOk(header: ByteArray): Boolean {
        val stored = parseOctal(header, 148, 8) ?: return false
        var unsigned = 0L
        var signed = 0L
        for (i in header.indices) {
            val b = if (i in 148..155) 0x20 else (header[i].toInt() and 0xff)
            unsigned += b.toLong()
            signed += (if (b > 127) b - 256 else b).toLong()
        }
        return unsigned == stored || signed == stored
    }

    /**
     * Counts every byte that crossed the stream, so the extractor can hold the archive to the
     * catalog's exact decompressed size without reaching into the zstd layer. Sits under a
     * DataInputStream, whose readFully drains it via [read]. Pure java.io: the tar reader has no
     * Android or JNI dependency and runs under plain JVM unit tests.
     */
    private class CountingInputStream(input: InputStream) : java.io.FilterInputStream(input) {
        var bytesRead: Long = 0
            private set

        override fun read(b: ByteArray, off: Int, len: Int): Int {
            val n = super.read(b, off, len)
            if (n > 0) bytesRead += n
            return n
        }
    }

    companion object {
        private const val RECORD = 512
        private const val CHUNK_BYTES = 256 * 1024
        private const val MAX_ENTRIES = 100_000
        private const val MAX_FILE_BYTES = 512L * 1024 * 1024
        private const val MAX_LONGNAME_BYTES = 4097 // payload including the required NUL
        private const val TYPE_FILE = 0 // NUL
        private const val TYPE_FILE_NAME = '0'.code
        private const val TYPE_DIR = '5'.code
        private const val TYPE_LONGNAME = 'L'.code
        private val ZERO_RECORD = ByteArray(RECORD)
    }
}
