package com.armsx2.ui.textures

import android.util.Log
import androidx.compose.runtime.mutableStateOf
import com.armsx2.BuildConfig
import com.armsx2.TextureCatalog
import com.armsx2.memcard.OnlineIcons
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.InputStream
import java.net.HttpURLConnection

/**
 * Popular Today for texture packs: the same anonymous download counter as Online Icons' (the
 * armsx2-icon-stats worker, tools/memcard-icons/stats-worker), on routes of its own. The app tells it
 * a pack's id when a pack the player chose has installed, and asks it for today's ten most downloaded.
 * The worker counts each pack once per person per day, by a salted hash of the day and their IP; the
 * IP is never stored.
 */
internal object TexturePackStats {
    private const val TAG = "TexturePackStats"
    private const val BASE = OnlineIcons.STATS + "textures/"
    private const val POPULAR = 10
    private val ID = Regex("^[a-z0-9][a-z0-9._-]{1,254}$")
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /** Today's most downloaded packs, most first, as the counter last said; null before it has. */
    val popular = mutableStateOf<List<String>?>(null)

    /** Whether the counter has been asked since the app started: with [popular] still null after,
     *  it could not be reached. */
    val asked = mutableStateOf(false)

    /** Asks the counter for today's most downloaded packs. Keeps the last answer when it can't. */
    suspend fun refreshPopular() {
        val list = withContext(Dispatchers.IO) {
            val conn = open(BASE + "popular") ?: return@withContext null
            try {
                if (conn.responseCode != HttpURLConnection.HTTP_OK) null
                else conn.inputStream.use { String(it.readBounded(64 * 1024), Charsets.UTF_8) }.lineSequence()
                    .mapNotNull { line -> line.trim().substringBefore(' ').takeIf { ID.matches(it) } }
                    .distinct().take(POPULAR).toList()
            } catch (e: Exception) {
                Log.w(TAG, "popular: ${e.message}")
                null
            } finally {
                conn.disconnect()
            }
        }
        if (list != null) popular.value = list
        asked.value = true
    }

    /** Tells the counter a pack the player chose has installed: its id, nothing else. Not retried or
     *  kept if it fails. */
    fun countDownload(id: String) {
        if (!ID.matches(id)) return
        scope.launch {
            val body = id.toByteArray()
            // The body goes out in configure: the redirect helper reads the response right after it.
            val conn = open(BASE + "hit", method = "POST") {
                doOutput = true
                setRequestProperty("Content-Type", "text/plain; charset=utf-8")
                outputStream.use { it.write(body) }
            } ?: return@launch
            runCatching { conn.responseCode }
            conn.disconnect()
        }
    }

    private fun open(url: String, method: String = "GET", configure: HttpURLConnection.() -> Unit = {}): HttpURLConnection? =
        TextureCatalog.RedirectingHttps.open(url, connectTimeoutMs = 15_000, readTimeoutMs = 30_000, tag = TAG) {
            requestMethod = method
            setRequestProperty("User-Agent", "ARMSX2/" + runCatching { BuildConfig.VERSION_NAME }.getOrDefault("dev"))
            configure()
        }

    private fun InputStream.readBounded(max: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        val buf = ByteArray(16 * 1024)
        while (true) {
            val n = read(buf)
            if (n < 0) break
            if (out.size() + n > max) throw java.io.IOException("larger than $max bytes")
            out.write(buf, 0, n)
        }
        return out.toByteArray()
    }
}
