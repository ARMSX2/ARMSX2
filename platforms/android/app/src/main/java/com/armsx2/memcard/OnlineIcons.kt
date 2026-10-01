package com.armsx2.memcard

import android.content.Context
import android.util.Log
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import com.armsx2.BuildConfig
import com.armsx2.TextureCatalog
import com.armsx2.ZstdInputStream
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FilterInputStream
import java.io.InputStream
import java.net.HttpURLConnection

/**
 * Online Icons: the save icons of the PS2 Icon Open Database (ps2iodb.com, founded by Issun and
 * built by its contributors), for games with no save on the player's cards and no icon found on
 * their disc, and for the Icon Museum. The player picks what to download, one icon, the ones for
 * their games or the whole set, and can remove any of it again; what is downloaded stays on the
 * device, compressed, in an [OnlineIconStore]. tools/memcard-icons publishes the files at [BASE].
 */
object OnlineIcons {
    const val BASE = "https://icons.ps2ktxpak.net/"

    /** The download counter behind "Popular Today" (tools/memcard-icons/stats-worker): today's
     *  most downloaded icons, and where the app says which icons it downloaded on purpose. */
    const val STATS = "https://iconstats.ps2ktxpak.net/"
    private const val ZIP = "memcard-icons.zip"
    private const val TAG = "OnlineIcons"
    private const val DIR = "memcard_online"
    private const val PREVIEW_DIR = "memcard_online_preview"
    private const val KEY_ETAG = "library.onlineIcons.etag."
    private const val MAX_ICON_BYTES = 4 shl 20
    private const val MAX_LIST_BYTES = 16 shl 20
    private const val PROGRESS_STEP = 512L * 1024
    private const val PARALLEL = 8
    private const val POPULAR = 6
    private val HASH = Regex("[0-9a-f]{16}")

    sealed interface Status {
        data object Idle : Status
        /** A download running: [done] of [total], bytes of the whole set when [bytes], else icons.
         *  [total] is -1 when the server didn't say. */
        data class Working(val done: Long, val total: Long, val bytes: Boolean) : Status
        data class Failed(val why: String) : Status
    }

    /** What a download is doing, for the browser and the menu row. */
    val status = mutableStateOf<Status>(Status.Idle)

    /** Bumped when icons are downloaded or removed, or the catalog changes, so covers, the Museum
     *  and the browser look again. */
    val generation = mutableIntStateOf(0)

    /** One icon in the catalog: which game, which of its saves, and who contributed it. */
    class Entry(val hash: String, val title: String, val label: String, val contributors: String)

    @Volatile private var store: OnlineIconStore? = null
    @Volatile private var previews: File? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var job: Job? = null

    /** Decodes one zstd frame with the app's JNI decoder, the texture-pack installer's. */
    private val decode: (InputStream, Long) -> ByteArray = { input, max -> ZstdInputStream(input, max).use { it.readBytes() } }

    fun init(context: Context) {
        if (store != null) return
        store = OnlineIconStore(File(context.filesDir, DIR), decode)
        previews = File(context.cacheDir, PREVIEW_DIR)
    }

    // ---- what is on the device ----------------------------------------------------------------

    /** The icon for [serial], by hash, whether downloaded or not. */
    fun hashFor(serial: String): String? = store?.hashFor(serial)

    /** Every icon the set has, by title, downloaded or not. Empty before the first [syncLists]. */
    fun catalog(): List<Entry> = store?.catalog().orEmpty()

    fun isInstalled(hash: String): Boolean = store?.isInstalled(hash) == true
    fun installedHashes(): Set<String> = store?.installedHashes().orEmpty()
    fun installedBytes(): Long = store?.installedBytes() ?: 0L

    /** A downloaded icon's bytes, icon.sys (964 bytes) and then the icon, or null. */
    fun read(hash: String): ByteArray? = store?.read(hash)

    // ---- the server ---------------------------------------------------------------------------

    /** Fetches the index and the catalog when they changed. True when both are on the device,
     *  fetched now or before (offline, the last ones do). */
    suspend fun syncLists(): Boolean = withContext(Dispatchers.IO) {
        val s = store ?: return@withContext false
        var changed = false
        for (name in listOf(OnlineIconStore.INDEX, OnlineIconStore.CATALOG)) {
            val known = runCatching { MainActivityRuntime.prefs.getString(KEY_ETAG + name, null) }.getOrNull()
                ?.takeIf { File(s.dir, name).isFile }
            val conn = open(BASE + name) { if (known != null) setRequestProperty("If-None-Match", known) } ?: continue
            try {
                if (conn.responseCode != HttpURLConnection.HTTP_OK) continue // 304: the one here is current
                val bytes = conn.inputStream.use { it.readBounded(MAX_LIST_BYTES) }
                if (s.putList(name, bytes)) {
                    changed = true
                    runCatching { MainActivityRuntime.prefs.edit().putString(KEY_ETAG + name, conn.getHeaderField("ETag")).apply() }
                }
            } catch (e: Exception) {
                Log.w(TAG, "fetch $name", e)
            } finally {
                conn.disconnect()
            }
        }
        if (changed) withContext(Dispatchers.Main) { generation.intValue++ }
        File(s.dir, OnlineIconStore.INDEX).isFile && File(s.dir, OnlineIconStore.CATALOG).isFile
    }

    /** The whole set's download size, or null when the server can't be reached. */
    suspend fun setSize(): Long? = withContext(Dispatchers.IO) {
        val conn = open(BASE + ZIP, method = "HEAD") ?: return@withContext null
        try {
            if (conn.responseCode == HttpURLConnection.HTTP_OK) conn.contentLengthLong.takeIf { it > 0 } else null
        } catch (e: Exception) {
            null
        } finally {
            conn.disconnect()
        }
    }

    /**
     * An icon's bytes for a preview: the downloaded one, else one fetched earlier for a preview,
     * else fetched now into the preview cache (which the system may clear; downloading the icon
     * afterwards takes it from there instead of fetching it again). Null when it can't be had.
     */
    suspend fun preview(hash: String): ByteArray? = withContext(Dispatchers.IO) {
        read(hash)?.let { return@withContext it }
        val s = store ?: return@withContext null
        val cached = previewFile(hash)
        cached?.takeIf { it.isFile }?.let { f ->
            runCatching { decode(f.inputStream(), MAX_ICON_BYTES.toLong()) }.getOrNull()?.let { return@withContext it }
        }
        val compressed = fetchIcon(hash) ?: return@withContext null
        val bytes = s.decodeChecked(hash, compressed) ?: return@withContext null
        cached?.let { f -> runCatching { f.parentFile?.mkdirs(); f.writeBytes(compressed) } }
        bytes
    }

    // ---- downloads ----------------------------------------------------------------------------

    /** Downloads the whole set in the background, as one file unpacked as it arrives, or icon by
     *  icon when the server hasn't got the one file; [status] follows it. What arrived stays if it
     *  is stopped. */
    fun installAll() = launch { s ->
        val conn = open(BASE + ZIP) ?: return@launch "can't reach the icon server"
        try {
            val code = conn.responseCode
            if (code == HttpURLConnection.HTTP_NOT_FOUND || code == HttpURLConnection.HTTP_FORBIDDEN) {
                conn.disconnect()
                if (!syncLists()) return@launch "can't reach the icon server"
                return@launch installEach(s, s.catalog().map { it.hash }, report = false)
            }
            if (code != HttpURLConnection.HTTP_OK) return@launch "the icon server said $code"
            val total = conn.contentLengthLong
            var shown = 0L
            val counted = object : FilterInputStream(conn.inputStream) {
                var done = 0L
                override fun read(b: ByteArray, off: Int, len: Int): Int = super.read(b, off, len).also { n ->
                    if (n > 0) {
                        done += n
                        if (done - shown >= PROGRESS_STEP) {
                            shown = done
                            status.value = Status.Working(done, total, bytes = true)
                        }
                    }
                }
            }
            val scopeActive = currentCoroutineContext()
            val icons = counted.use { s.installFromZip(it) { scopeActive.isActive } }
            currentCoroutineContext().ensureActive()
            if (icons == 0) "the download had no icons" else null
        } finally {
            conn.disconnect()
        }
    }

    /** Downloads these icons in the background, picked by the player, so they count toward
     *  Popular Today; [status] counts them. */
    fun install(hashes: Collection<String>) {
        if (hashes.none { !isInstalled(it) }) return
        launch { s -> installEach(s, hashes, report = true) }
    }

    /** Fetches each icon not on the device yet, [PARALLEL] at a time, taking any a preview already
     *  fetched from there, and when [report], tells the counter which arrived. Null when it went
     *  well, else why not. */
    private suspend fun installEach(s: OnlineIconStore, hashes: Collection<String>, report: Boolean): String? = coroutineScope {
        val todo = hashes.filterNot { s.isInstalled(it) }.distinct()
        val queue = java.util.concurrent.ConcurrentLinkedQueue(todo)
        val done = java.util.concurrent.atomic.AtomicInteger()
        val failed = java.util.concurrent.atomic.AtomicInteger()
        val arrived = java.util.concurrent.ConcurrentLinkedQueue<String>()
        status.value = Status.Working(0, todo.size.toLong(), bytes = false)
        (1..PARALLEL).map {
            async {
                while (true) {
                    ensureActive()
                    val hash = queue.poll() ?: break
                    val cached = previewFile(hash)?.takeIf { it.isFile }?.let { runCatching { it.readBytes() }.getOrNull() }
                    val ok = (cached != null && s.install(hash, cached)) || (fetchIcon(hash)?.let { s.install(hash, it) } == true)
                    if (ok) arrived += hash else failed.incrementAndGet()
                    previewFile(hash)?.delete()
                    val n = done.incrementAndGet()
                    if (n % 16 == 0 || n == todo.size) status.value = Status.Working(n.toLong(), todo.size.toLong(), bytes = false)
                }
            }
        }.awaitAll()
        if (report && arrived.isNotEmpty()) scope.launch { reportDownloads(arrived.toList()) }
        when {
            todo.isNotEmpty() && failed.get() == todo.size -> "can't reach the icon server"
            failed.get() > 0 -> "%d icons didn't download".format(failed.get())
            else -> null
        }
    }

    // ---- Popular Today ---------------------------------------------------------------------------

    /** Today's most downloaded icons, most first, as the counter last said; null before it has. */
    val popular = mutableStateOf<List<String>?>(null)

    /** Asks the counter for today's most downloaded icons. Keeps the last answer when it can't. */
    suspend fun refreshPopular() {
        val list = withContext(Dispatchers.IO) {
            val conn = open(STATS + "popular") ?: return@withContext null
            try {
                if (conn.responseCode != HttpURLConnection.HTTP_OK) null
                else conn.inputStream.use { String(it.readBounded(64 * 1024), Charsets.UTF_8) }.lineSequence()
                    .mapNotNull { line -> line.trim().substringBefore(' ').takeIf { HASH.matches(it) } }.distinct().take(POPULAR).toList()
            } catch (e: Exception) {
                null
            } finally {
                conn.disconnect()
            }
        }
        if (list != null) popular.value = list
    }

    /** Tells the counter which icons the player downloaded on purpose: their hashes, nothing else.
     *  One request however many; nothing is retried or kept if it fails. */
    private fun reportDownloads(hashes: List<String>) {
        val body = hashes.take(100).joinToString("\n").toByteArray()
        // The body goes out in configure: the redirect helper reads the response right after it.
        val conn = open(STATS + "hit", method = "POST") {
            doOutput = true
            setRequestProperty("Content-Type", "text/plain; charset=utf-8")
            outputStream.use { it.write(body) }
        } ?: return
        runCatching { conn.responseCode }
        conn.disconnect()
    }

    fun uninstall(hash: String) {
        store?.uninstall(hash)
        generation.intValue++
    }

    fun uninstallAll() {
        store?.uninstallAll()
        generation.intValue++
    }

    /** Stops a download; what it already brought stays. Call on the main thread. */
    fun cancel() {
        job?.cancel()
        status.value = Status.Idle
    }

    val busy: Boolean get() = job?.isActive == true

    /** Runs one download job at a time. [work] returns null when it went well, else why not. */
    private fun launch(work: suspend (OnlineIconStore) -> String?) {
        val s = store ?: return
        if (job?.isActive == true) return
        job = scope.launch {
            status.value = Status.Working(0, -1, bytes = true)
            val why = try {
                work(s)
            } catch (e: Exception) {
                if (e is kotlinx.coroutines.CancellationException) throw e // cancel() reset status
                Log.w(TAG, "download failed", e)
                e.message ?: e.javaClass.simpleName
            } finally {
                withContext(kotlinx.coroutines.NonCancellable + Dispatchers.Main) { generation.intValue++ }
            }
            withContext(Dispatchers.Main) { status.value = if (why == null) Status.Idle else Status.Failed(why) }
        }
    }

    private fun previewFile(hash: String): File? = previews?.let { File(it, "$hash.zst") }

    private fun fetchIcon(hash: String): ByteArray? {
        val conn = open(BASE + "icons/$hash.zst") ?: return null
        return try {
            if (conn.responseCode == HttpURLConnection.HTTP_OK) conn.inputStream.use { it.readBounded(MAX_ICON_BYTES) } else null
        } catch (e: Exception) {
            Log.w(TAG, "fetch icon $hash", e)
            null
        } finally {
            conn.disconnect()
        }
    }

    /** HTTPS only, redirects included, the way the texture-pack installer fetches. */
    private fun open(url: String, method: String = "GET", configure: HttpURLConnection.() -> Unit = {}): HttpURLConnection? =
        TextureCatalog.RedirectingHttps.open(url, connectTimeoutMs = 15_000, readTimeoutMs = 30_000, tag = TAG) {
            requestMethod = method
            setRequestProperty("User-Agent", "ARMSX2/" + runCatching { BuildConfig.VERSION_NAME }.getOrDefault("dev"))
            // Keep Content-Length honest so the percentage means something.
            setRequestProperty("Accept-Encoding", "identity")
            configure()
        }

    private fun InputStream.readBounded(max: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        val buf = ByteArray(64 * 1024)
        while (true) {
            val n = read(buf)
            if (n < 0) break
            if (out.size() + n > max) throw java.io.IOException("larger than $max bytes")
            out.write(buf, 0, n)
        }
        return out.toByteArray()
    }
}
