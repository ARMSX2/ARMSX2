package com.armsx2

import android.util.Log
import androidx.test.platform.app.InstrumentationRegistry
import kr.co.iefriends.pcsx2.NativeApp
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest

/**
 * Streams a REAL migration-produced tar.zst fixture through [ZstdInputStream] +
 * [TarTextureExtractor] on-device and checks every extracted file byte-for-byte against a
 * manifest. Fixtures are pushed by adb and passed as instrumentation arguments, because there
 * is no zstd tooling on the device:
 *
 *   am instrument -e fixturePath <tar.zst> -e manifestPath <manifest> \
 *     -e sha256 <hex> -e decompressedSizeBytes <n> -e fileCount <n> ...
 */
class TarZstdStreamInstrumentedTest {
    private fun sha256(file: File): String =
        MessageDigest.getInstance("SHA-256").run {
            file.inputStream().use { input ->
                val buf = ByteArray(256 * 1024)
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    update(buf, 0, n)
                }
            }
            digest().joinToString("") { "%02x".format(it) }
        }

    @Test
    fun realPackExtractsByteExactly() {
        assertFalse("emucore not loaded on this device", NativeApp.hasNoNativeBinary)
        val args = InstrumentationRegistry.getArguments()
        val keys = args.keySet().toList()
        Log.i("TarZstdStreamTest", "instrumentation args: $keys")
        for (k in keys) Log.i("TarZstdStreamTest", "  $k = ${args.get(k)}")
        val fixturePath = args.getString("fixturePath")!!
        val manifestPath = args.getString("manifestPath")!!
        val expectedSha = args.getString("sha256")!!
        val expectedDecompressed = args.getString("decompressedSizeBytes")!!.toLong()
        val expectedCount = args.getString("fileCount")!!.toInt()

        val fixture = File(fixturePath)
        assertTrue("fixture missing: $fixturePath", fixture.isFile)
        // The pushed fixture must be exactly what the producer published.
        assertEquals(expectedSha, sha256(fixture))

        val outDir = File(
            InstrumentationRegistry.getInstrumentation().targetContext.getExternalFilesDir(null),
            "tarzstd-test-out",
        ).apply { deleteRecursively(); mkdirs() }

        val start = System.currentTimeMillis()
        fixture.inputStream().use { raw ->
            ZstdInputStream(raw, expectedDecompressed).use { zstd ->
                val extractor = TarTextureExtractor(
                    dest = outDir,
                    expectedDecompressedBytes = expectedDecompressed,
                    expectedTextureCount = expectedCount,
                    onProgress = { _, _ -> },
                    isCancelled = { false },
                    raw = zstd,
                )
                val outcome = extractor.extract()
                assertTrue("extraction failed: $outcome", outcome is TarTextureExtractor.Outcome.Success)
            }
        }
        val elapsedMs = System.currentTimeMillis() - start
        Log.i("TarZstdStreamTest", "extracted $expectedCount textures in ${elapsedMs}ms")

        // Byte-exact comparison against the producer's manifest.
        val manifest = File(manifestPath).readLines().filter { it.isNotBlank() }
        assertEquals(expectedCount, manifest.size)
        var verified = 0
        for (line in manifest) {
            val (rel, expected) = line.split(" ", limit = 2)
            val f = File(outDir, rel)
            assertTrue("missing extracted file: $rel", f.isFile)
            assertEquals("hash mismatch for $rel", expected, sha256(f))
            verified++
        }
        assertEquals(expectedCount, verified)

        // Nothing extra was created.
        val extracted = outDir.walkTopDown().filter { it.isFile }.map { it.relativeTo(outDir).path }.toSet()
        assertEquals(manifest.map { it.split(" ", limit = 2)[0] }.toSet(), extracted)
        outDir.deleteRecursively()
    }

    /** Streams one large frame whose declared window log is 27 and reports peak RSS growth. */
    @Test
    fun windowLog27MemoryProbe() {
        assertFalse("emucore not loaded on this device", NativeApp.hasNoNativeBinary)
        val args = InstrumentationRegistry.getArguments()
        val rawPath = args.getString("probePath") ?: return // only with probe args
        val raw = File(rawPath)
        assertTrue("probe fixture missing: $rawPath", raw.isFile)
        val size = raw.length()

        fun vmHwmKb(): Long {
            val status = File("/proc/self/status").readText()
            return status.lineSequence().first { it.startsWith("VmHWM") }
                .split(Regex("\\s+"))[1].toLong()
        }

        val hwmBefore = vmHwmKb()
        var decoded = 0L
        raw.inputStream().use { input ->
            ZstdInputStream(input, size).use { zstd ->
                val buf = ByteArray(256 * 1024)
                while (true) {
                    val n = zstd.read(buf)
                    if (n < 0) break
                    decoded += n
                }
            }
        }
        val hwmAfter = vmHwmKb()
        val growthMb = (hwmAfter - hwmBefore) / 1024
        Log.i(
            "TarZstdStreamTest",
            "window-27 probe: decoded $decoded bytes in ${(hwmAfter - hwmBefore)} KB peak RSS growth",
        )
        // The probe file is exactly 300 MiB of /dev/urandom; a short or long read means the
        // frame/EOF contract broke. The wrapper would already have failed a trailing byte.
        assertEquals(300L * 1024 * 1024, decoded)
        // The decoder's own footprint is one 128 MiB window plus buffers; anything far above
        // ~256 MiB growth means the bridge is hoarding memory and the gate fails.
        assertTrue("RSS grew ${growthMb} MiB; expected <= 256 MiB", growthMb <= 256)
    }

    @Test
    fun corruptedFixtureIsRejected() {
        assertFalse("emucore not loaded on this device", NativeApp.hasNoNativeBinary)
        val args = InstrumentationRegistry.getArguments()
        val fixturePath = args.getString("fixturePath") ?: return // only run with fixture args
        val fixture = File(fixturePath)
        assertTrue(fixture.isFile)
        val corrupt = File(fixture.parentFile, "corrupt.tar.zst")
        val bytes = fixture.readBytes()
        bytes[bytes.size / 2] = (bytes[bytes.size / 2].toInt() xor 0x5a).toByte()
        corrupt.writeBytes(bytes)

        val outDir = File(
            InstrumentationRegistry.getInstrumentation().targetContext.getExternalFilesDir(null),
            "tarzstd-test-corrupt",
        ).apply { deleteRecursively(); mkdirs() }

        val expectedDecompressed = args.getString("decompressedSizeBytes")!!.toLong()
        val expectedCount = args.getString("fileCount")!!.toInt()
        corrupt.inputStream().use { raw ->
            ZstdInputStream(raw, expectedDecompressed).use { zstd ->
                val extractor = TarTextureExtractor(
                    dest = outDir, expectedDecompressedBytes = expectedDecompressed,
                    expectedTextureCount = expectedCount, onProgress = { _, _ -> },
                    isCancelled = { false }, raw = zstd,
                )
                val outcome = extractor.extract()
                assertTrue(
                    "corruption must be rejected, was $outcome",
                    outcome is TarTextureExtractor.Outcome.Failure ||
                        outcome is TarTextureExtractor.Outcome.Cancelled,
                )
            }
        }
        corrupt.delete()
        outDir.deleteRecursively()
    }
}
