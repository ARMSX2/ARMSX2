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
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.Closeable
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.net.HttpURLConnection
import java.util.zip.ZipFile

/**
 * Online Icons: the save icons of the PS2 Icon Open Database (ps2iodb.com, founded by Issun and
 * built by its contributors), for games with no save on the player's cards and no icon found on
 * their disc, and for the Icon Museum. Downloaded once, when the player asks, as one zip that is
 * kept as it is: each icon inside is its own zstd frame, read out and decoded only when it is
 * drawn, so the whole set stays compressed on the device. tools/memcard-icons builds the zip.
 */
object OnlineIcons {
    const val URL = "https://icons.ps2ktxpak.net/memcard-icons.zip"
    private const val TAG = "OnlineIcons"
    private const val DIR = "memcard_online"
    private const val FILE = "memcard-icons.zip"
    private const val KEY_ETAG = "library.onlineIcons.etag"
    private const val PROGRESS_STEP = 512L * 1024

    sealed interface Status {
        data object Idle : Status
        /** [total] is -1 when the server didn't say. */
        data class Downloading(val done: Long, val total: Long) : Status
        /** Downloaded; making sure it is a set of icons before it replaces the one there is. */
        data object Checking : Status
        data class Failed(val why: String) : Status
    }

    /** What the download is doing, for the prompt and the menu row. */
    val status = mutableStateOf<Status>(Status.Idle)

    /** Bumped when a set is installed or removed, so covers and the Museum pick it up. */
    val generation = mutableIntStateOf(0)

    /** One icon in the catalog: which game, which of its saves, and who contributed it. */
    class Entry(val hash: String, val title: String, val label: String, val contributors: String)

    @Volatile private var app: Context? = null
    private val lock = Any()
    private var current: OnlineIconSet? = null // guarded by lock
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var job: Job? = null

    /** Decodes one zstd frame with the app's JNI decoder, the texture-pack installer's. */
    private val decode: (InputStream, Long) -> ByteArray = { input, max -> ZstdInputStream(input, max).use { it.readBytes() } }

    fun init(context: Context) {
        if (app == null) app = context.applicationContext
    }

    private fun file(): File? = app?.let { File(File(it.filesDir, DIR), FILE) }

    /** Whether a set is on the device. Read [generation] alongside, to be told when it changes. */
    val installed: Boolean get() = file()?.isFile == true

    private fun set(): OnlineIconSet? = synchronized(lock) {
        current ?: file()?.takeIf { it.isFile }?.let { OnlineIconSet.open(it, decode) }?.also { current = it }
    }

    /** The icon for [serial] in the downloaded set, by its hash, or null. */
    fun hashFor(serial: String): String? = set()?.index?.get(serial.uppercase())

    /** Every icon in the downloaded set, by title, for the Icon Museum. */
    fun catalog(): List<Entry> = set()?.catalog.orEmpty()

    /** How many different icons the downloaded set has. */
    fun iconCount(): Int = set()?.catalog?.mapTo(HashSet()) { it.hash }?.size ?: 0

    /** An icon's bytes, icon.sys (964 bytes) and then the icon, or null. Reads the zip. */
    fun read(hash: String): ByteArray? = set()?.read(hash)

    /** The size and version of the set on the server, or null when it can't be reached. */
    class Remote(val bytes: Long, val etag: String?)

    suspend fun remote(): Remote? = withContext(Dispatchers.IO) {
        val conn = TextureCatalog.RedirectingHttps.open(URL, connectTimeoutMs = 10_000, readTimeoutMs = 10_000, tag = TAG) {
            requestMethod = "HEAD"
            setRequestProperty("User-Agent", userAgent())
            setRequestProperty("Accept-Encoding", "identity")
        } ?: return@withContext null
        try {
            if (conn.responseCode != HttpURLConnection.HTTP_OK) null
            else Remote(conn.contentLengthLong, conn.getHeaderField("ETag"))
        } catch (e: Exception) {
            null
        } finally {
            conn.disconnect()
        }
    }

    /** Whether the server's set is newer than the one on the device. */
    fun isUpdate(remote: Remote): Boolean {
        val mine = runCatching { MainActivityRuntime.prefs.getString(KEY_ETAG, null) }.getOrNull()
        return installed && remote.etag != null && remote.etag != mine
    }

    /** Downloads the set in the background; [status] follows it. Does nothing if one is running. */
    fun start() {
        val ctx = app ?: return
        if (job?.isActive == true) return
        job = scope.launch {
            setStatus(Status.Downloading(0, -1))
            val why = try {
                download(ctx)
            } catch (e: Exception) {
                if (e is kotlinx.coroutines.CancellationException) throw e // cancel() already reset status
                Log.w(TAG, "download failed", e)
                e.message ?: e.javaClass.simpleName
            }
            if (why == null) {
                setStatus(Status.Idle)
                withContext(Dispatchers.Main) { generation.intValue++ }
            } else {
                setStatus(Status.Failed(why))
            }
        }
    }

    /** Stops a download; the set there was, if any, stays. Call on the main thread. */
    fun cancel() {
        job?.cancel()
        status.value = Status.Idle
    }

    /** Deletes the downloaded set. Covers from it go back to box art. */
    fun remove() {
        synchronized(lock) {
            current?.close()
            current = null
            file()?.delete()
        }
        runCatching { MainActivityRuntime.prefs.edit().remove(KEY_ETAG).apply() }
        status.value = Status.Idle
        generation.intValue++
    }

    /** Returns null when the new set is in place, else why not. */
    private suspend fun download(ctx: Context): String? {
        val dir = File(ctx.filesDir, DIR).apply { mkdirs() }
        val part = File(dir, "$FILE.part")
        part.delete()
        try {
            val conn = TextureCatalog.RedirectingHttps.open(URL, connectTimeoutMs = 20_000, readTimeoutMs = 30_000, tag = TAG) {
                requestMethod = "GET"
                setRequestProperty("User-Agent", userAgent())
                // Keep Content-Length honest so the percentage means something.
                setRequestProperty("Accept-Encoding", "identity")
            } ?: return "can't reach the icon server"
            val etag: String?
            try {
                if (conn.responseCode != HttpURLConnection.HTTP_OK) return "the icon server said ${conn.responseCode}"
                etag = conn.getHeaderField("ETag")
                val total = conn.contentLengthLong
                var done = 0L
                var shown = 0L
                conn.inputStream.use { input ->
                    FileOutputStream(part).use { out ->
                        val buf = ByteArray(256 * 1024)
                        while (true) {
                            currentCoroutineContext().ensureActive()
                            val n = input.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                            done += n
                            if (done - shown >= PROGRESS_STEP) {
                                shown = done
                                setStatus(Status.Downloading(done, total))
                            }
                        }
                    }
                }
                if (total > 0 && done != total) return "the download was cut short"
                setStatus(Status.Downloading(done, total))
            } finally {
                conn.disconnect()
            }

            // A file that doesn't open as a set, or whose icons don't read, never replaces the set
            // there is.
            setStatus(Status.Checking)
            val ok = OnlineIconSet.open(part, decode)?.use { check ->
                val sample = check.index.values.firstOrNull()?.let { check.read(it) }
                sample != null && Ps2IconSys.parse(sample) != null &&
                    Ps2Icon.parse(sample.copyOfRange(Ps2IconSys.SIZE, sample.size)) != null
            } == true
            if (!ok) return "the download isn't a set of icons"
            synchronized(lock) {
                current?.close()
                current = null
                val target = File(dir, FILE)
                if (!part.renameTo(target)) return "couldn't save the icons"
            }
            runCatching { MainActivityRuntime.prefs.edit().putString(KEY_ETAG, etag).apply() }
            return null
        } finally {
            part.delete() // gone after a rename; what is left of a failed or cancelled one otherwise
        }
    }

    private suspend fun setStatus(s: Status) = withContext(Dispatchers.Main) { status.value = s }

    private fun userAgent(): String = "ARMSX2/" + runCatching { BuildConfig.VERSION_NAME }.getOrDefault("dev")
}

/**
 * One downloaded set: the zip, kept open, with its index and catalog read out of it. The zip holds
 * memcard-icons/index.txt.zst ("SERIAL HASH" lines), catalog.txt.zst ("HASH<tab>title<tab>which
 * save<tab>contributors" for every icon) and icons/HASH.zst (icon.sys, then the icon), each a zstd
 * frame; the zip itself stores them without compressing again.
 */
internal class OnlineIconSet private constructor(
    private val zip: ZipFile,
    val index: Map<String, String>,
    val catalog: List<OnlineIcons.Entry>,
    private val decode: (InputStream, Long) -> ByteArray,
) : Closeable {
    /** An icon's bytes, or null when the set hasn't got it. */
    fun read(hash: String): ByteArray? = synchronized(zip) {
        val entry = zip.getEntry("${ROOT}icons/$hash.zst") ?: return null
        runCatching { decode(zip.getInputStream(entry), MAX_ICON_BYTES) }.getOrNull()
    }

    override fun close() = synchronized(zip) { zip.close() }

    companion object {
        private const val ROOT = "memcard-icons/"
        private const val MAX_TEXT_BYTES = 16L shl 20
        private const val MAX_ICON_BYTES = 4L shl 20

        /** Opens [file] as a set, or null when it isn't one (no index, or an empty one). */
        fun open(file: File, decode: (InputStream, Long) -> ByteArray): OnlineIconSet? {
            val zip = runCatching { ZipFile(file) }.getOrNull() ?: return null
            try {
                fun text(name: String): String? =
                    zip.getEntry(ROOT + name)?.let { String(decode(zip.getInputStream(it), MAX_TEXT_BYTES), Charsets.UTF_8) }
                val index = HashMap<String, String>()
                text("index.txt.zst")?.lineSequence()?.forEach { line ->
                    val space = line.indexOf(' ')
                    if (space > 0 && !line.startsWith("#")) index[line.substring(0, space).uppercase()] = line.substring(space + 1).trim()
                }
                if (index.isEmpty()) {
                    zip.close()
                    return null
                }
                val catalog = text("catalog.txt.zst")?.lineSequence()?.mapNotNull { line ->
                    val p = line.split('\t')
                    if (p.size >= 2 && p[0].isNotBlank()) OnlineIcons.Entry(p[0], p[1], p.getOrElse(2) { "" }, p.getOrElse(3) { "" }) else null
                }?.toList().orEmpty()
                return OnlineIconSet(zip, index, catalog, decode)
            } catch (e: Exception) {
                zip.close()
                return null
            }
        }
    }
}
