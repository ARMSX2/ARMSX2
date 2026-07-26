package com.armsx2

import android.content.Context
import android.util.Log
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.zip.ZipFile

/**
 * Downloads and installs a catalog texture pack into `<dataRoot>/textures/<SERIAL>/replacements`.
 *
 * Deliberately a plain foreground download rather than upstream's WorkManager service. That service
 * buys pause/resume across a reboot, at the cost of a WorkManager dependency, kotlinx-serialization,
 * three manifest permissions, a notification channel and an on-disk task store. The streaming
 * download and zip handling here follow [ShaderRepo], which already does this shape correctly.
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

    sealed interface Progress {
        data class Downloading(val read: Long, val total: Long) : Progress
        data object Verifying : Progress
        data class Extracting(val done: Int, val total: Int) : Progress
        data object Installing : Progress
    }

    data class Outcome(val ok: Boolean, val error: String? = null)

    /**
     * Blocking; call from a background dispatcher. [isCancelled] is polled throughout so the user
     * can abandon a multi-gigabyte transfer.
     */
    fun install(
        context: Context,
        pack: TextureCatalog.Pack,
        serial: String,
        onProgress: (Progress) -> Unit,
        isCancelled: () -> Boolean,
    ): Outcome {
        val root = File(MainActivityRuntime.assetCopyRoot(context))
        // Staging must share a filesystem with the destination, or the commit below turns from a
        // rename into a multi-gigabyte copy.
        val staging = File(root, ".texturepacks-tmp")
        staging.deleteRecursively()
        staging.mkdirs()

        try {
            val needed = pack.sizeBytes * 2 + FREE_SPACE_SLACK
            val free = runCatching { root.usableSpace }.getOrDefault(0L)
            if (free in 1 until needed) {
                return Outcome(false, "Needs ~${needed / 1024 / 1024} MB free, ${free / 1024 / 1024} MB available")
            }

            val archive = File(staging, "pack.zip")
            val digest = MessageDigest.getInstance("SHA-256")
            if (!download(pack.downloadUrl, archive, digest, onProgress, isCancelled)) {
                return if (isCancelled()) Outcome(false, null) else Outcome(false, "Download failed")
            }

            onProgress(Progress.Verifying)
            if (archive.length() != pack.sizeBytes) {
                return Outcome(false, "Size mismatch (expected ${pack.sizeBytes}, got ${archive.length()})")
            }
            val actual = digest.digest().joinToString("") { "%02X".format(it) }
            if (!actual.equals(pack.sha256, ignoreCase = true)) {
                Log.w(TAG, "sha256 mismatch: expected ${pack.sha256} got $actual")
                return Outcome(false, "Checksum mismatch — the download was corrupted")
            }

            val extracted = File(staging, "out")
            extracted.mkdirs()
            val count = extract(archive, extracted, onProgress, isCancelled)
            if (count <= 0) {
                return if (isCancelled()) Outcome(false, null) else Outcome(false, "Archive contained no textures")
            }
            archive.delete()

            onProgress(Progress.Installing)
            val dest = File(File(root, "textures"), serial.uppercase())
            if (!commit(extracted, File(dest, "replacements"))) {
                return Outcome(false, "Could not write to the textures folder")
            }

            TexturePackInstallState.record(pack.id, serial.uppercase(), pack.version, pack.name)
            return Outcome(true)
        } catch (e: Exception) {
            Log.w(TAG, "install failed: ${e.message}")
            return Outcome(false, e.message ?: "Install failed")
        } finally {
            staging.deleteRecursively()
        }
    }

    // ---- download -----------------------------------------------------------------------------

    /** Streams to [dest] while feeding [digest], so verification costs no second pass over 1.5 GB. */
    private fun download(
        url: String,
        dest: File,
        digest: MessageDigest,
        onProgress: (Progress) -> Unit,
        isCancelled: () -> Boolean,
    ): Boolean {
        var conn: HttpURLConnection? = null
        try {
            conn = (URL(url).openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = 20_000
                // Per-read rather than whole-transfer: a gigabyte on a slow link is legitimately
                // long, but a stalled socket still fails fast.
                readTimeout = 30_000
                instanceFollowRedirects = true
                setRequestProperty("User-Agent", userAgent())
                // Keep Content-Length honest so the progress bar means something.
                setRequestProperty("Accept-Encoding", "identity")
            }
            if (conn.responseCode != HttpURLConnection.HTTP_OK) {
                Log.w(TAG, "download $url -> ${conn.responseCode}")
                return false
            }
            val total = conn.contentLengthLong
            var read = 0L
            var reported = 0L
            conn.inputStream.use { input ->
                FileOutputStream(dest).use { out ->
                    val buf = ByteArray(256 * 1024)
                    while (true) {
                        if (isCancelled()) return false
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        digest.update(buf, 0, n)
                        read += n
                        if (read - reported >= PROGRESS_BYTES_STEP) {
                            reported = read
                            onProgress(Progress.Downloading(read, total))
                        }
                    }
                }
            }
            onProgress(Progress.Downloading(read, total))
            return read > 0
        } catch (e: Exception) {
            Log.w(TAG, "download $url failed: ${e.message}")
            return false
        } finally {
            conn?.disconnect()
        }
    }

    // ---- extract ------------------------------------------------------------------------------

    /** Returns the number of texture files written, or 0 on failure. */
    private fun extract(
        archive: File,
        dest: File,
        onProgress: (Progress) -> Unit,
        isCancelled: () -> Boolean,
    ): Int {
        return runCatching {
            ZipFile(archive).use { zf ->
                val entries = zf.entries().toList()
                    .filterNot { it.isDirectory || isJunkEntry(it.name) }
                    .filter { isTextureFile(it.name) }
                val total = entries.size
                var done = 0
                val destCanonical = dest.canonicalPath + File.separator
                for (entry in entries) {
                    if (isCancelled()) return 0
                    val rel = replacementRelativePath(entry.name) ?: continue
                    val out = File(dest, rel)
                    // Zip-slip: a crafted "../" entry would otherwise write anywhere the app can
                    // reach. Fail the whole install rather than skip — a pack containing one is not
                    // a pack we should be half-installing.
                    if (!out.canonicalPath.startsWith(destCanonical)) {
                        Log.w(TAG, "zip-slip entry rejected: ${entry.name}")
                        return 0
                    }
                    out.parentFile?.mkdirs()
                    zf.getInputStream(entry).use { input ->
                        FileOutputStream(out).use { input.copyTo(it, 256 * 1024) }
                    }
                    done++
                    if (done % 32 == 0 || done == total) onProgress(Progress.Extracting(done, total))
                }
                done
            }
        }.getOrElse {
            Log.w(TAG, "extract failed: ${it.message}")
            0
        }
    }

    /**
     * Strips whatever wrapper the archive uses so files land directly in `replacements/`.
     *
     * Packs are published three ways: rooted at the textures themselves, wrapped in `<SERIAL>/`,
     * and wrapped in `<SERIAL>/replacements/`. GitHub's own zips add a `name-<40 hex>/` root on top.
     * Take everything after the last `replacements/` segment, else after a `<SERIAL>/` segment,
     * else drop a single GitHub-style root.
     */
    private fun replacementRelativePath(name: String): String? {
        val norm = name.replace('\\', '/').trimStart('/')
        if (norm.isEmpty()) return null
        val parts = norm.split('/').filter { it.isNotEmpty() && it != "." }
        if (parts.isEmpty()) return null

        val repIdx = parts.indexOfLast { it.equals("replacements", ignoreCase = true) }
        if (repIdx >= 0 && repIdx < parts.size - 1) return parts.drop(repIdx + 1).joinToString("/")

        val serialIdx = parts.indexOfLast { Regex("^[A-Za-z]{4}-?[0-9]{5}$").matches(it) }
        if (serialIdx >= 0 && serialIdx < parts.size - 1) return parts.drop(serialIdx + 1).joinToString("/")

        if (parts.size > 1 && Regex("^.+-[0-9a-f]{40}$").matches(parts[0])) {
            return parts.drop(1).joinToString("/")
        }
        return parts.joinToString("/")
    }

    /** The core only loads PNG and DDS; anything else is a readme or a stray thumbnail. */
    private fun isTextureFile(name: String): Boolean {
        val lower = name.lowercase()
        return lower.endsWith(".png") || lower.endsWith(".dds")
    }

    private fun isJunkEntry(name: String): Boolean {
        val lower = name.lowercase()
        return lower.startsWith("__macosx/") || lower.contains("/__macosx/") ||
            lower.endsWith("/.ds_store") || lower == ".ds_store" || lower.endsWith("/thumbs.db")
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

    private fun userAgent(): String = "ARMSX2/" + runCatching {
        NativeApp.getBuildVersion()
    }.getOrNull().orEmpty().ifEmpty { "dev" }
}
