package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.File

/**
 * JVM tests for the streaming GNU-tar extraction contract (mirrors the producer validator's
 * suite in scratch/migration/test_tarcheck.py). The extractor takes a plain InputStream here —
 * the JNI-backed ZstdInputStream is exercised on-device by ZstdBridgeInstrumentedTest.
 */
class TarTextureExtractorTest {
    @get:Rule
    val tmp = TemporaryFolder()

    // ---- stream-building helpers ---------------------------------------------------------------

    private val RECORD = 512

    private fun header(
        name: ByteArray,
        size: Long = 0,
        typeflag: Byte = '0'.code.toByte(),
        magic: String = "ustar ",
    ): ByteArray {
        val h = ByteArray(RECORD)
        // GNU truncates the in-header name at 100 bytes when a long-name record supplies the
        // real path, so long names must clamp rather than overflow the field.
        name.copyOf(minOf(name.size, 100)).copyInto(h)
        "0000644\u0000".toByteArray(Charsets.US_ASCII).copyInto(h, 100)
        "0000000\u0000".toByteArray(Charsets.US_ASCII).copyInto(h, 108)
        "0000000\u0000".toByteArray(Charsets.US_ASCII).copyInto(h, 116)
        ("%011o".format(size) + "\u0000").toByteArray(Charsets.US_ASCII).copyInto(h, 124)
        "00000000000\u0000".toByteArray(Charsets.US_ASCII).copyInto(h, 136)
        " ".repeat(8).toByteArray(Charsets.US_ASCII).copyInto(h, 148)
        h[156] = typeflag
        magic.toByteArray(Charsets.US_ASCII).copyInto(h, 257)
        if (magic == "ustar ") " \u0000".toByteArray(Charsets.US_ASCII).copyInto(h, 263)
        else "00".toByteArray(Charsets.US_ASCII).copyInto(h, 263)
        val sum = h.sumOf { it.toInt() and 0xff }.toLong()
        ("%06o".format(sum) + "\u0000 ").toByteArray(Charsets.US_ASCII).copyInto(h, 148)
        return h
    }

    private fun header(name: String, size: Long = 0, typeflag: Byte = '0'.code.toByte()): ByteArray =
        header(name.toByteArray(Charsets.UTF_8), size, typeflag)

    private fun padded(payload: ByteArray): ByteArray {
        val pad = ((payload.size + RECORD - 1) / RECORD * RECORD - payload.size)
        return payload + ByteArray(pad)
    }

    private val END = ByteArray(RECORD * 20) // two end blocks + blocking-factor zero padding

    private val KTX_PAYLOAD = "KTX10".toByteArray() + ByteArray(107) // 112 bytes

    private fun extract(
        tar: ByteArray,
        expectedDecompressed: Long = tar.size.toLong(),
        expectedTextures: Int = 1,
        cancelled: Boolean = false,
    ): TarTextureExtractor.Outcome {
        val dest = tmp.newFolder()
        val extractor = TarTextureExtractor(
            dest = dest,
            expectedDecompressedBytes = expectedDecompressed,
            expectedTextureCount = expectedTextures,
            onProgress = { _, _ -> },
            isCancelled = { cancelled },
            raw = ByteArrayInputStream(tar),
        )
        return extractor.extract()
    }

    private fun assertFailure(outcome: TarTextureExtractor.Outcome, contains: String? = null) {
        assertTrue("expected Failure, was $outcome", outcome is TarTextureExtractor.Outcome.Failure)
        if (contains != null) {
            assertTrue(
                "expected reason containing '$contains', was $outcome",
                (outcome as TarTextureExtractor.Outcome.Failure).reason.contains(contains),
            )
        }
    }

    // ---- happy paths ----------------------------------------------------------------------------

    @Test
    fun singleKtxFileExtracts() {
        val tar = header("replacements/a.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) + END
        val outcome = extract(tar)
        assertEquals(TarTextureExtractor.Outcome.Success(1), outcome)
    }

    @Test
    fun rootDotDirectoryAccepted() {
        val tar = header("./", 0, '5'.code.toByte()) +
            header("./replacements/a.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) + END
        assertEquals(TarTextureExtractor.Outcome.Success(1), extract(tar))
    }

    @Test
    fun junkAndNonTexturesConsumedNotCounted() {
        val tar = header("replacements/a.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) +
            header("replacements/notes.txt", 11) + padded("hello world".toByteArray()) +
            header("__macosx/junk", 4) + padded("junk".toByteArray()) +
            END
        assertEquals(TarTextureExtractor.Outcome.Success(1), extract(tar))
    }

    @Test
    fun filesExtractToDisk() {
        val tar = header("replacements/nested/b.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) + END
        val dest = tmp.newFolder()
        val extractor = TarTextureExtractor(
            dest, tar.size.toLong(), 1, { _, _ -> }, { false }, ByteArrayInputStream(tar),
        )
        assertEquals(TarTextureExtractor.Outcome.Success(1), extractor.extract())
        val out = File(dest, "nested/b.ktx")
        assertTrue(out.isFile)
        assertTrue(out.readBytes().contentEquals(KTX_PAYLOAD))
    }

    // ---- malformed input ------------------------------------------------------------------------

    @Test
    fun badChecksumRejected() {
        val h = header("a.ktx")
        h[149] = '9'.code.toByte()
        assertFailure(extract(h + END), "checksum")
    }

    @Test
    fun base256SizeRejected() {
        val h = header("a.ktx")
        h[124] = (0x80 or 0x40).toByte()
        recomputeChecksum(h) // isolate the size rejection from the checksum check
        assertFailure(extract(h + END), "size")
    }

    private fun recomputeChecksum(h: ByteArray) {
        // The checksum convention sums the header with the checksum field treated as spaces;
        // this header already carries a stored value, so restore the spaces first.
        for (i in 148..155) h[i] = 0x20
        val sum = h.sumOf { it.toInt() and 0xff }.toLong()
        ("%06o".format(sum) + "\u0000 ").toByteArray(Charsets.US_ASCII).copyInto(h, 148)
    }

    @Test
    fun absoluteAndTraversalPathsRejected() {
        assertFailure(extract(header("/etc/passwd") + END), "path")
        assertFailure(extract(header("replacements/../../x.ktx") + END), "path")
    }

    @Test
    fun emptyComponentRejected() {
        assertFailure(extract(header("a//b.ktx") + END), "path")
    }

    @Test
    fun unsupportedTypesRejected() {
        for (tf in byteArrayOf('1'.code.toByte(), '2'.code.toByte(), '3'.code.toByte(),
            '4'.code.toByte(), '6'.code.toByte(), '7'.code.toByte(), 'x'.code.toByte(),
            'g'.code.toByte())) {
            assertFailure(extract(header("link", 0, tf)), "unsupported entry type")
        }
    }

    @Test
    fun duplicateAndCaseCollisionsRejected() {
        val two = header("a.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) +
            header("a.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) + END
        assertFailure(extract(two), "colliding")

        val case = header("File.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) +
            header("file.KTX", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) + END
        assertFailure(extract(case), "colliding")
    }

    @Test
    fun ancestorConflictRejected() {
        val tar = header("replacements", 0, '0'.code.toByte()) +
            header("replacements/x.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) + END
        assertFailure(extract(tar), "ancestor")
    }

    @Test
    fun missingEndBlocksRejected() {
        assertFailure(extract(header("a.ktx")), "read") // stream ends mid-stream
    }

    @Test
    fun nonzeroTrailingPaddingRejected() {
        val bad = END.copyOf().also { it[END.size - 1] = 1 }
        assertFailure(extract(header("a.ktx") + bad), "padding")
    }

    @Test
    fun longNameAppliesOnceAndHonorsLimit() {
        val longName = "replacements/" + "l".repeat(130) + ".ktx"
        val payload = longName.toByteArray(Charsets.UTF_8) + 0
        val good = header("./L", payload.size.toLong(), 'L'.code.toByte()) + padded(payload) +
            header(longName, KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) + END
        assertEquals(TarTextureExtractor.Outcome.Success(1), extract(good))

        val oversized = "x".repeat(4097) + "\u0000"
        val bad = header("./L", oversized.length.toLong(), 'L'.code.toByte()) +
            padded(oversized.toByteArray(Charsets.UTF_8)) + END
        assertFailure(extract(bad), "GNU L")
    }

    @Test
    fun doubleLongNameRejected() {
        val payload = "a\u0000".toByteArray()
        val tar = header("./L", payload.size.toLong(), 'L'.code.toByte()) + padded(payload) +
            header("./L", payload.size.toLong(), 'L'.code.toByte()) + padded(payload) + END
        assertFailure(extract(tar), "GNU L")
    }

    // ---- accounting -----------------------------------------------------------------------------

    @Test
    fun decompressedSizeMustMatchExactly() {
        val tar = header("replacements/a.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) + END
        assertFailure(extract(tar, expectedDecompressed = tar.size + 512L), "size mismatch")
    }

    @Test
    fun textureCountMustMatch() {
        val tar = header("replacements/a.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) +
            header("replacements/b.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) + END
        assertFailure(extract(tar, expectedTextures = 3), "count mismatch")
    }

    @Test
    fun emptyTarIsWellFormedButCountsWrong() {
        // A zero-entry archive is structurally valid; the texture-count check is what rejects it.
        assertFailure(extract(END, expectedTextures = 1), "count mismatch")
    }

    @Test
    fun cancellationIsObserved() {
        val tar = header("replacements/a.ktx", KTX_PAYLOAD.size.toLong()) + padded(KTX_PAYLOAD) + END
        assertEquals(TarTextureExtractor.Outcome.Cancelled, extract(tar, cancelled = true))
    }
}
