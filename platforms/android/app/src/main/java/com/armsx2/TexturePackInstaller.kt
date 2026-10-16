package com.armsx2

import android.content.Context
import android.net.Uri
import android.util.Log
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import java.io.BufferedInputStream
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.net.HttpURLConnection
import java.security.MessageDigest
import java.util.zip.ZipFile
import java.util.zip.ZipInputStream

/**
 * Downloads and installs a catalog texture pack into `<dataRoot>/textures/<SERIAL>/replacements`.
 *
 * Deliberately a plain foreground download rather than upstream's WorkManager service. That service
 * buys pause/resume across a reboot, at the cost of a WorkManager dependency, kotlinx-serialization,
 * three manifest permissions, a notification channel and an on-disk task store. The streaming
 * download and archive handling here follow [ShaderRepo], which already does this shape correctly.
 *
 * Two archive formats arrive here. Schema-1 GitHub packs are ZIP and extract through
 * [ZipFile]; B2 schema-2 packs are tar+zstd and stream through [ZstdInputStream] +
 * [TarTextureExtractor] — the compressed bytes are verified end to end first, so extraction never
 * sees data the catalog did not describe. Path policy and collision keys are shared with both
 * branches via [TextureArchivePath]; the format branch exists only after whole-archive
 * verification.
 *
 * Sizes here are unlike anything else the app downloads — the largest pack in the catalog is 1.5 GB,
 * where a controller skin is a few hundred KB. That drives three things the skin path never needed:
 * a free-space precheck, a digest verified while streaming, and an atomic replace that can roll back.
 */
object TexturePackInstaller {
    private const val TAG = "TexturePackInstaller"
    private const val PROGRESS_BYTES_STEP = 256L * 1024
    /** Peak usage is the archive plus its extracted contents plus the outgoing pack kept for
     *  rollback, so budget for well over the download alone. */
    private const val FREE_SPACE_SLACK = 512L * 1024 * 1024
    /** A removed pack's folder while its files are deleted. It sits beside the game folders so
     *  moving it there is one rename, and the leading dot keeps it from reading as a serial. */
    private const val REMOVED_PREFIX = ".deleting-"
    /** The native zstd decoder's own ceiling. */
    private const val MAX_LOCAL_TAR_BYTES = 16L shl 30
    private val ZSTD_MAGIC = byteArrayOf(0x28, 0xB5.toByte(), 0x2F, 0xFD.toByte())
    private val ZIP_MAGIC = byteArrayOf(0x50, 0x4B, 0x03, 0x04)

    sealed interface Progress {
        data class Downloading(val read: Long, val total: Long) : Progress
        data object Verifying : Progress
        data class Extracting(val done: Int, val total: Int) : Progress
        data object Installing : Progress
    }

    /** [textures] is filled in by [installLocal] only. */
    data class Outcome(val ok: Boolean, val error: String? = null, val textures: Int = 0)

    /**
     * Blocking; call from a background dispatcher. [isCancelled] is polled throughout so the user
     * can abandon a multi-gigabyte transfer. Cancellation is honored only through extraction: once
     * the commit begins it runs to completion (or synchronous rollback), and the caller disables
     * its Cancel control when [Progress.Installing] is reported.
     */
    fun install(
        context: Context,
        pack: TextureCatalog.Pack,
        serial: String,
        onProgress: (Progress) -> Unit,
        isCancelled: () -> Boolean,
    ): Outcome {
        if (NativeApp.hasNoNativeBinary) {
            return Outcome(false, "The emulator core is not available on this device")
        }

        val root = File(MainActivityRuntime.assetCopyRoot(context))
        // Staging must share a filesystem with the destination, or the commit below turns from a
        // rename into a multi-gigabyte copy.
        val staging = File(root, ".texturepacks-tmp")
        staging.deleteRecursively()
        staging.mkdirs()

        try {
            // ZIP extraction roughly doubles the archive; tar+zstd is streamed, so peak use is the
            // archive plus its exact decompressed size, both of which the catalog declares.
            val needed = when (pack.format) {
                TextureCatalog.ArchiveFormat.TAR_ZSTD ->
                    pack.sizeBytes + pack.decompressedSizeBytes + FREE_SPACE_SLACK
                TextureCatalog.ArchiveFormat.ZIP ->
                    pack.sizeBytes * 2 + FREE_SPACE_SLACK
            }
            val free = runCatching { root.usableSpace }.getOrDefault(0L)
            if (free in 1 until needed) {
                return Outcome(false, "Needs ~${needed / 1024 / 1024} MB free, ${free / 1024 / 1024} MB available")
            }

            val archive = File(staging, "pack.download")
            val digest = MessageDigest.getInstance("SHA-256")

            // A single-file pack is one part, so there is one loop rather than two code paths.
            // Parts stream straight into the same archive in order: concatenating afterwards would
            // mean holding the pieces AND the joined file, i.e. double the disk for a 4 GB pack.
            val parts = pack.effectiveParts()
            val split = parts.size > 1
            var done = 0L
            for ((index, part) in parts.withIndex()) {
                val label = if (split) " (part ${index + 1} of ${parts.size})" else ""
                val partDigest = MessageDigest.getInstance("SHA-256")
                val n = download(
                    url = part.downloadUrl,
                    dest = archive,
                    append = index > 0,
                    overall = digest,
                    part = partDigest,
                    priorBytes = done,
                    grandTotal = pack.sizeBytes,
                    onProgress = onProgress,
                    isCancelled = isCancelled,
                )
                if (n < 0L) {
                    return if (isCancelled()) Outcome(false, null) else Outcome(false, "Download failed$label")
                }
                // Per-part checks name the bad piece, and stop us paying for the remaining parts of
                // a transfer that already cannot produce the right archive.
                if (n != part.sizeBytes) {
                    Log.w(TAG, "part ${index + 1} size ${n} != ${part.sizeBytes}")
                    return Outcome(false, "Size mismatch$label (expected ${part.sizeBytes}, got $n)")
                }
                val partActual = hex(partDigest.digest())
                if (!partActual.equals(part.sha256, ignoreCase = true)) {
                    Log.w(TAG, "part ${index + 1} sha256 mismatch: expected ${part.sha256} got $partActual")
                    return Outcome(false, "Checksum mismatch$label: the download was corrupted")
                }
                done += n
            }

            onProgress(Progress.Verifying)
            if (archive.length() != pack.sizeBytes) {
                return Outcome(false, "Size mismatch (expected ${pack.sizeBytes}, got ${archive.length()})")
            }
            // Still verified end to end even when every part passed: the parts can each be intact
            // and yet be the wrong parts, or joined in the wrong order.
            val actual = hex(digest.digest())
            if (!actual.equals(pack.sha256, ignoreCase = true)) {
                Log.w(TAG, "sha256 mismatch: expected ${pack.sha256} got $actual")
                return Outcome(false, "Checksum mismatch: the download was corrupted")
            }

            val extracted = File(staging, "out")
            extracted.mkdirs()
            val count = when (pack.format) {
                TextureCatalog.ArchiveFormat.TAR_ZSTD ->
                    extractTarZstd(archive, extracted, pack, onProgress, isCancelled)
                TextureCatalog.ArchiveFormat.ZIP ->
                    extractZip(archive, extracted, onProgress, isCancelled)
            }
            when (count) {
                is ExtractionResult.Cancelled -> return Outcome(false, null)
                is ExtractionResult.Failure -> return Outcome(false, count.reason)
                is ExtractionResult.Done ->
                    if (count.textures <= 0) {
                        return Outcome(false, "Archive contained no textures")
                    }
            }
            archive.delete()

            onProgress(Progress.Installing)
            val dest = File(File(root, "textures"), serial.uppercase())
            if (!commit(extracted, File(dest, "replacements"))) {
                return Outcome(false, "Could not write to the textures folder")
            }

            TexturePackInstallState.record(
                packId = pack.id,
                serial = serial.uppercase(),
                version = pack.version,
                name = pack.name,
                format = pack.format.wireName,
                schema = if (pack.format == TextureCatalog.ArchiveFormat.TAR_ZSTD) 2 else 1,
                archiveRevision = pack.archiveRevision,
                sha256 = pack.sha256,
            )
            return Outcome(true)
        } catch (e: Exception) {
            Log.w(TAG, "install failed: ${e.message}")
            return Outcome(false, e.message ?: "Install failed")
        } finally {
            staging.deleteRecursively()
        }
    }

    /**
     * Removes the pack installed in [serialDirectory] (a game's `textures/<SERIAL>` folder): takes
     * it out of use in one step, then deletes its files. Returns true when no pack is left there.
     * Blocking; call from a background dispatcher.
     *
     * This used to be one deleteRecursively() over the game's folder. When a single file refused,
     * it stopped part way and left the pack half deleted, still where the core reads it, and
     * recorded as installed, so the catalog would not reinstall over it. Now the pack folder is
     * first renamed out of the game's folder into one beside it, which is one rename on the same
     * filesystem however many files the pack holds: the pack is out of use at once. Deleting the
     * files comes after, and whatever of that fails is finished by [sweepRemovedPacks] instead of
     * being left in use.
     *
     * Only the pack goes, along with the replacements.old an interrupted [commit] can leave.
     * Anything else in the game's folder, such as texture dumps, stays, and the folder goes once
     * it is empty.
     */
    fun removePack(serialDirectory: File): Boolean {
        val root = serialDirectory.parentFile ?: return false
        for (name in listOf("replacements", "replacements.old")) {
            val dir = File(serialDirectory, name)
            if (!dir.exists()) continue
            val removed = File(root, "$REMOVED_PREFIX${serialDirectory.name}-$name-${System.nanoTime()}")
            // The rename should always work here; if it does not, delete in place as before.
            val doomed = if (dir.renameTo(removed)) removed else dir
            if (!runCatching { doomed.deleteRecursively() }.getOrDefault(false)) {
                // deleteRecursively() says nothing about WHICH entry refused. Name a few.
                val left = runCatching {
                    doomed.walkTopDown().filter { it != doomed }.take(5).toList()
                }.getOrDefault(emptyList())
                Log.w(TAG, "delete ${doomed.absolutePath} incomplete (renamed=${doomed === removed}); " +
                    "survivors=${left.joinToString { it.relativeTo(doomed).path }}")
            }
        }
        if (serialDirectory.list()?.isEmpty() == true) serialDirectory.delete()
        return !File(serialDirectory, "replacements").exists()
    }

    /** Finishes deleting packs [removePack] could not delete completely. [texturesRoot] is the
     *  `textures` folder. Blocking; call from a background dispatcher. */
    fun sweepRemovedPacks(texturesRoot: File) {
        texturesRoot.listFiles().orEmpty()
            .filter { it.isDirectory && it.name.startsWith(REMOVED_PREFIX) }
            .forEach { runCatching { it.deleteRecursively() } }
    }

    /**
     * Installs a .zip or .tar.zst pack the user picked from storage into
     * `<dataRoot>/textures/<SERIAL>/replacements`. Blocking; call from a background dispatcher.
     *
     * The format is read from the first bytes, not the name: a document provider's display name
     * can lack the extension. There is no catalog entry, so nothing is checked against declared
     * sizes or a digest, but the archive goes through the same path policy and tar rules as a
     * catalog install. Files are extracted to staging first and then moved over the existing
     * pack, so a failed archive leaves the game's textures as they were. Like the folder import,
     * this adds to an existing pack rather than replacing it, and the catalog's install record is
     * left alone.
     */
    fun installLocal(
        context: Context,
        uri: Uri,
        serial: String,
        onProgress: (Progress) -> Unit,
        isCancelled: () -> Boolean,
    ): Outcome {
        val root = File(MainActivityRuntime.assetCopyRoot(context))
        // Not install()'s staging folder: a catalog download can run at the same time.
        val staging = File(root, ".texturepacks-local-tmp")
        staging.deleteRecursively()
        staging.mkdirs()
        try {
            val resolver = context.contentResolver
            val magic = ByteArray(4)
            val got = resolver.openInputStream(uri)?.use { readUpTo(it, magic) }
                ?: return Outcome(false, "Could not open the selected file")
            val format = when {
                got == 4 && magic.contentEquals(ZSTD_MAGIC) -> TextureCatalog.ArchiveFormat.TAR_ZSTD
                got == 4 && magic.contentEquals(ZIP_MAGIC) -> TextureCatalog.ArchiveFormat.ZIP
                else -> return Outcome(false, "Not a .zip or .tar.zst texture pack")
            }
            if (format == TextureCatalog.ArchiveFormat.TAR_ZSTD && NativeApp.hasNoNativeBinary) {
                return Outcome(false, "The emulator core is not available on this device")
            }

            val extracted = File(staging, "out")
            extracted.mkdirs()
            val count = resolver.openInputStream(uri)?.use { raw ->
                when (format) {
                    TextureCatalog.ArchiveFormat.TAR_ZSTD -> {
                        // Nothing declares the unpacked size, so the most the zstd layer may emit is
                        // what the disk can hold.
                        val free = runCatching { root.usableSpace }.getOrDefault(0L)
                        val cap = (free - FREE_SPACE_SLACK).coerceIn(1L, MAX_LOCAL_TAR_BYTES)
                        extractTarZstdStream(raw, extracted, cap, null, null, onProgress, isCancelled)
                    }
                    TextureCatalog.ArchiveFormat.ZIP ->
                        extractZipStream(raw, extracted, onProgress, isCancelled)
                }
            } ?: return Outcome(false, "Could not open the selected file")
            when (count) {
                is ExtractionResult.Cancelled -> return Outcome(false, null)
                is ExtractionResult.Failure -> return Outcome(false, count.reason)
                is ExtractionResult.Done ->
                    if (count.textures <= 0) return Outcome(false, "Archive contained no textures")
            }

            onProgress(Progress.Installing)
            val dest = File(File(File(root, "textures"), serial.uppercase()), "replacements")
            if (!mergeInto(extracted, dest)) {
                return Outcome(false, "Could not write to the textures folder")
            }
            return Outcome(true, textures = (count as ExtractionResult.Done).textures)
        } catch (e: Exception) {
            Log.w(TAG, "local install failed: ${e.message}")
            return Outcome(false, e.message ?: "Install failed")
        } finally {
            staging.deleteRecursively()
        }
    }

    private sealed interface ExtractionResult {
        data class Done(val textures: Int) : ExtractionResult
        data object Cancelled : ExtractionResult
        data class Failure(val reason: String) : ExtractionResult
    }

    // ---- download -----------------------------------------------------------------------------

    /**
     * Streams one piece to [dest] while feeding both digests, so verification costs no second pass
     * over several gigabytes. [overall] spans the whole concatenated archive and [part] just this
     * piece. Returns the bytes written for this piece, or -1 on failure or cancellation.
     *
     * [priorBytes] and [grandTotal] exist so progress stays monotonic across a split download —
     * reporting each part's own 0..n would restart the bar at every boundary.
     */
    private fun download(
        url: String,
        dest: File,
        append: Boolean,
        overall: MessageDigest,
        part: MessageDigest,
        priorBytes: Long,
        grandTotal: Long,
        onProgress: (Progress) -> Unit,
        isCancelled: () -> Boolean,
    ): Long {
        // Manual HTTPS-only redirects (via TextureCatalog.RedirectingHttps): automatic following
        // would happily traverse an http:// hop on the way to the archive.
        val conn = TextureCatalog.RedirectingHttps.open(
            url, connectTimeoutMs = 20_000, readTimeoutMs = 30_000, tag = TAG,
        ) {
            requestMethod = "GET"
            setRequestProperty("User-Agent", userAgent())
            // Keep Content-Length honest so the progress bar means something.
            setRequestProperty("Accept-Encoding", "identity")
        } ?: return -1L
        try {
            if (conn.responseCode != HttpURLConnection.HTTP_OK) {
                Log.w(TAG, "download $url -> ${conn.responseCode}")
                return -1L
            }
            var read = 0L
            var reported = 0L
            conn.inputStream.use { input ->
                FileOutputStream(dest, append).use { out ->
                    val buf = ByteArray(256 * 1024)
                    while (true) {
                        if (isCancelled()) return -1L
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        overall.update(buf, 0, n)
                        part.update(buf, 0, n)
                        read += n
                        if (read - reported >= PROGRESS_BYTES_STEP) {
                            reported = read
                            onProgress(Progress.Downloading(priorBytes + read, grandTotal))
                        }
                    }
                }
            }
            onProgress(Progress.Downloading(priorBytes + read, grandTotal))
            return read
        } catch (e: Exception) {
            Log.w(TAG, "download $url failed: ${e.message}")
            return -1L
        } finally {
            conn.disconnect()
        }
    }

    private fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02X".format(it) }

    // ---- extract: zip -------------------------------------------------------------------------

    /** Returns the number of texture files written, or 0 on failure. */
    private fun extractZip(
        archive: File,
        dest: File,
        onProgress: (Progress) -> Unit,
        isCancelled: () -> Boolean,
    ): ExtractionResult {
        return runCatching {
            ZipFile(archive).use { zf ->
                val entries = zf.entries().toList()
                    .filterNot { it.isDirectory || TextureArchivePath.isJunkEntry(it.name) }
                    .filter { TextureArchivePath.isTextureFile(it.name) }
                val total = entries.size
                var done = 0
                val destCanonical = dest.canonicalPath + File.separator
                val seen = HashSet<String>()
                for (entry in entries) {
                    if (isCancelled()) return ExtractionResult.Cancelled
                    val rel = TextureArchivePath.replacementRelativePath(entry.name) ?: continue
                    val out = File(dest, rel)
                    // Zip-slip: a crafted "../" entry would otherwise write anywhere the app can
                    // reach. Fail the whole install rather than skip — a pack containing one is not
                    // a pack we should be half-installing.
                    if (!out.canonicalPath.startsWith(destCanonical)) {
                        Log.w(TAG, "zip-slip entry rejected: ${entry.name}")
                        return ExtractionResult.Failure("Archive contained an unsafe path")
                    }
                    // Same collision rule the tar branch enforces: two entries that normalize to
                    // one destination (case, Unicode, or wrapper differences) must not silently
                    // last-write-wins over each other.
                    val key = TextureArchivePath.collisionKey(rel)
                    if (!seen.add(key)) {
                        Log.w(TAG, "duplicate destination rejected: $rel")
                        return ExtractionResult.Failure("Archive contains duplicate textures ($rel)")
                    }
                    out.parentFile?.mkdirs()
                    zf.getInputStream(entry).use { input ->
                        FileOutputStream(out).use { input.copyTo(it, 256 * 1024) }
                    }
                    done++
                    if (done % 32 == 0 || done == total) onProgress(Progress.Extracting(done, total))
                }
                ExtractionResult.Done(done)
            }
        }.getOrElse {
            Log.w(TAG, "extract failed: ${it.message}")
            ExtractionResult.Failure("Archive could not be read: ${it.message ?: "unknown error"}")
        }
    }

    // ---- extract: tar+zstd --------------------------------------------------------------------

    private fun extractTarZstd(
        archive: File,
        dest: File,
        pack: TextureCatalog.Pack,
        onProgress: (Progress) -> Unit,
        isCancelled: () -> Boolean,
    ): ExtractionResult = archive.inputStream().use { raw ->
        // The native layer caps output at the catalog's declared decompressed size; the tar
        // extractor additionally requires the stream to land on it exactly.
        extractTarZstdStream(
            raw, dest, pack.decompressedSizeBytes, pack.decompressedSizeBytes, pack.fileCount,
            onProgress, isCancelled,
        )
    }

    /** [expectedBytes] and [expectedTextures] are null for a local import, which has no catalog
     *  entry to hold them to; [maxOutputBytes] still bounds the zstd layer. */
    private fun extractTarZstdStream(
        raw: InputStream,
        dest: File,
        maxOutputBytes: Long,
        expectedBytes: Long?,
        expectedTextures: Int?,
        onProgress: (Progress) -> Unit,
        isCancelled: () -> Boolean,
    ): ExtractionResult {
        ZstdInputStream(raw, maxOutputBytes).use { zstd ->
            val extractor = TarTextureExtractor(
                dest = dest,
                expectedDecompressedBytes = expectedBytes,
                expectedTextureCount = expectedTextures,
                onProgress = { done, total -> onProgress(Progress.Extracting(done, total)) },
                isCancelled = isCancelled,
                raw = zstd,
            )
            return when (val outcome = extractor.extract()) {
                is TarTextureExtractor.Outcome.Success -> ExtractionResult.Done(outcome.textureCount)
                is TarTextureExtractor.Outcome.Cancelled -> ExtractionResult.Cancelled
                is TarTextureExtractor.Outcome.Failure -> {
                    Log.w(TAG, "tar extraction failed: ${outcome.reason}")
                    ExtractionResult.Failure(outcome.reason)
                }
            }
        }
    }

    // ---- extract: local zip -------------------------------------------------------------------

    /**
     * [extractZip] for a zip read from a document provider, which gives a stream rather than a
     * file. Same path policy; the entry count is not known up front, so progress has no total.
     */
    private fun extractZipStream(
        raw: InputStream,
        dest: File,
        onProgress: (Progress) -> Unit,
        isCancelled: () -> Boolean,
    ): ExtractionResult {
        return runCatching {
            ZipInputStream(BufferedInputStream(raw)).use { zin ->
                var done = 0
                val destCanonical = dest.canonicalPath + File.separator
                val seen = HashSet<String>()
                while (true) {
                    val entry = zin.nextEntry ?: break
                    if (isCancelled()) return ExtractionResult.Cancelled
                    if (entry.isDirectory || TextureArchivePath.isJunkEntry(entry.name) ||
                        !TextureArchivePath.isTextureFile(entry.name)
                    ) continue
                    val rel = TextureArchivePath.replacementRelativePath(entry.name) ?: continue
                    val out = File(dest, rel)
                    if (!out.canonicalPath.startsWith(destCanonical)) {
                        Log.w(TAG, "zip-slip entry rejected: ${entry.name}")
                        return ExtractionResult.Failure("Archive contained an unsafe path")
                    }
                    if (!seen.add(TextureArchivePath.collisionKey(rel))) {
                        Log.w(TAG, "duplicate destination rejected: $rel")
                        return ExtractionResult.Failure("Archive contains duplicate textures ($rel)")
                    }
                    out.parentFile?.mkdirs()
                    FileOutputStream(out).use { zin.copyTo(it, 256 * 1024) }
                    done++
                    if (done % 32 == 0) onProgress(Progress.Extracting(done, 0))
                }
                ExtractionResult.Done(done)
            }
        }.getOrElse {
            Log.w(TAG, "extract failed: ${it.message}")
            ExtractionResult.Failure("Archive could not be read: ${it.message ?: "unknown error"}")
        }
    }

    /** Fills [buf] from [input] as far as the stream allows; returns the bytes read. */
    private fun readUpTo(input: InputStream, buf: ByteArray): Int {
        var n = 0
        while (n < buf.size) {
            val r = input.read(buf, n, buf.size - n)
            if (r < 0) break
            n += r
        }
        return n
    }

    // ---- commit -------------------------------------------------------------------------------

    /**
     * Swaps [staged] into [target], keeping the previous contents until the new ones are in place.
     * A failed rename mid-way would otherwise leave the user with no textures at all.
     */
    private fun commit(staged: File, target: File): Boolean {
        val backup = File(target.parentFile, "${target.name}.old")
        backup.deleteRecursively()
        target.parentFile?.mkdirs()
        val hadPrevious = target.exists()
        if (hadPrevious && !target.renameTo(backup)) {
            Log.w(TAG, "could not move existing replacements aside")
            return false
        }
        if (!staged.renameTo(target)) {
            // Put the old pack back rather than leaving the game with nothing.
            if (hadPrevious) backup.renameTo(target)
            Log.w(TAG, "could not move staged pack into place")
            return false
        }
        backup.deleteRecursively()
        return true
    }

    /**
     * Moves every file under [staged] to the same relative path under [target], replacing files of
     * the same name and keeping the rest. Both are on one filesystem, so each move is a rename.
     */
    private fun mergeInto(staged: File, target: File): Boolean {
        if (!target.isDirectory && !target.mkdirs()) return false
        for (file in staged.walkTopDown().filter(File::isFile)) {
            val out = File(target, file.relativeTo(staged).path)
            out.parentFile?.mkdirs()
            if (out.exists() && !out.delete()) return false
            if (!file.renameTo(out)) {
                Log.w(TAG, "could not move ${file.name} into place")
                return false
            }
        }
        // The size cache in the texture screen is keyed on this folder's mtime, and a rename deep
        // inside it does not always change that.
        target.setLastModified(System.currentTimeMillis())
        return true
    }

    private fun userAgent(): String = "ARMSX2/" + runCatching {
        NativeApp.getBuildVersion()
    }.getOrNull().orEmpty().ifEmpty { "dev" }
}
