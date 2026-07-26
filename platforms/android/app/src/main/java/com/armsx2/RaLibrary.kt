package com.armsx2

import android.content.Context
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Library-wide RetroAchievements progress: set sizes and unlock counts for every game, including
 * games that have never been launched.
 *
 * The core can only report achievements for the game it currently has loaded, so anything else has
 * to come from RetroAchievements. That does NOT mean a request per game — RA publishes the whole
 * console catalogue, hashes included, in one call:
 *
 *  1. `API_GetGameList` (i=21 PS2, f=1 with-achievements, h=1 with-hashes) — one request giving
 *     every PS2 set's size keyed by MD5. Cached on disk; set sizes barely ever change.
 *  2. `API_GetUserCompletionProgress` — paginated, giving NumAwarded / NumAwardedHardcore /
 *     MaxPossible per RA game id for everything the user has touched.
 *
 * Locally each library game is hashed via [NativeApp.getAchievementsHashForPath] (reads the disc's
 * boot ELF, no boot required) and matched against the catalogue by hash — the only reliable key,
 * since RA carries no PS2 serials and title matching would mismatch regional variants.
 *
 * Result: two network requests for an entire library, and a game with a set but no unlocks correctly
 * shows 0/N instead of nothing.
 *
 * Needs the user's RA **web API key**, which is a different credential from the emulator login token
 * (RA exposes it on the site's settings page). Without one this does nothing and the library falls
 * back to what was captured while playing.
 */
object RaLibrary {
    private const val PS2_CONSOLE_ID = 21           // rc_consoles.h: RC_CONSOLE_PLAYSTATION_2
    private const val API = "https://retroachievements.org/API"
    private const val CATALOG_FILE = "ra_ps2_catalog.json"
    private const val HASH_PREFIX = "ra.hash."
    private const val PROGRESS_PAGE = 500           // API max per page

    private const val KEY_WEB_API_KEY = "ra.webApiKey"
    private const val KEY_USER = "ra.webUserName"
    private const val KEY_LAST_SYNC = "ra.lastLibrarySync"
    private const val KEY_CATALOG_FETCHED = "ra.catalogFetchedAt"
    /** Automatic syncs are rate-limited to once a day; the manual button ignores this. */
    private const val SYNC_MIN_INTERVAL_MS = 24L * 60 * 60 * 1000
    /** Catalogue refresh interval. New sets appear on RA steadily but not by the hour. */
    private const val CATALOG_TTL_MS = 7L * 24 * 60 * 60 * 1000

    /** md5 (lowercase) -> RA game id, and RA game id -> achievement count. */
    private class Catalog(val hashToGame: Map<String, Int>, val gameToTotal: Map<Int, Int>)

    val syncing = androidx.compose.runtime.mutableStateOf(false)
    val lastResult = androidx.compose.runtime.mutableStateOf("")

    var webApiKey: String
        get() = runCatching { MainActivityRuntime.prefs.getString(KEY_WEB_API_KEY, "").orEmpty() }
            .getOrDefault("")
        set(value) {
            runCatching {
                MainActivityRuntime.prefs.edit().putString(KEY_WEB_API_KEY, value.trim()).apply()
            }
        }

    var userName: String
        get() = runCatching { MainActivityRuntime.prefs.getString(KEY_USER, "").orEmpty() }
            .getOrDefault("")
        set(value) {
            val v = value.trim()
            if (v.isEmpty() || v == userName) return
            runCatching { MainActivityRuntime.prefs.edit().putString(KEY_USER, v).apply() }
        }

    fun configured(): Boolean = webApiKey.isNotEmpty() && userName.isNotEmpty()

    /** Library paths + serials from the last scan, so a manual sync can run from the RA panel
     *  (which has no game list of its own). */
    @Volatile
    private var lastKnownGames: List<Pair<String, String>> = emptyList()

    /**
     * Called whenever the library list changes. Runs a sync at most once a day unless [force] —
     * this is a two-request operation on someone else's servers, not something to repeat on every
     * navigation, and set sizes change on the order of weeks.
     */
    fun onLibraryLoaded(games: List<GameInfo>, force: Boolean = false) {
        lastKnownGames = games.mapNotNull { g ->
            val serial = g.serial?.takeIf { it.isNotBlank() } ?: return@mapNotNull null
            val path = runCatching { DiscIdentity.nativePath(g.uri) }.getOrNull()
                ?.takeIf { it.isNotBlank() } ?: return@mapNotNull null
            path to serial
        }
        if (!configured() || lastKnownGames.isEmpty()) return
        val last = runCatching { MainActivityRuntime.prefs.getLong(KEY_LAST_SYNC, 0L) }.getOrDefault(0L)
        if (!force && (System.currentTimeMillis() - last) < SYNC_MIN_INTERVAL_MS) return
        syncInBackground()
    }

    /** Manual "sync now" — ignores the interval. Returns false when there is nothing to do. */
    fun syncNow(): Boolean {
        if (!configured() || lastKnownGames.isEmpty()) return false
        syncInBackground()
        return true
    }

    private fun syncInBackground() {
        if (syncing.value) return
        val context = MainActivityRuntime.instance?.applicationContext ?: return
        val games = lastKnownGames
        syncing.value = true
        kotlin.concurrent.thread(isDaemon = true, name = "ra-library-sync") {
            val result = runCatching { sync(context, games, userName) }
                .getOrElse { it.message ?: "sync failed" }
            runCatching {
                MainActivityRuntime.prefs.edit()
                    .putLong(KEY_LAST_SYNC, System.currentTimeMillis()).apply()
            }
            lastResult.value = result
            syncing.value = false
        }
    }

    /**
     * Blocking full sync over [games] (path to serial). Safe to call with no VM running only —
     * hashing repoints the global CDVD, and the native side refuses while a game is live.
     * [onProgress] receives (done, total) so the UI can show movement on a large library.
     */
    fun sync(
        context: Context,
        games: List<Pair<String, String>>,
        userName: String,
        onProgress: (Int, Int) -> Unit = { _, _ -> },
    ): String {
        val key = webApiKey
        if (key.isEmpty()) return "No RetroAchievements web API key set"
        if (userName.isEmpty()) return "Not signed in to RetroAchievements"

        val catalog = loadOrFetchCatalog(context, userName, key)
            ?: return "Could not fetch the RetroAchievements game list"
        val progress = fetchUserProgress(userName, key)
            ?: return "Could not fetch your RetroAchievements progress"

        var matched = 0
        games.forEachIndexed { index, (path, serial) ->
            onProgress(index, games.size)
            if (serial.isEmpty()) return@forEachIndexed
            // Hashing reads the disc, so remember it per serial — the image does not change.
            val hash = cachedHash(serial) ?: hashAndCache(serial, path) ?: return@forEachIndexed
            val gameId = catalog.hashToGame[hash] ?: return@forEachIndexed
            val total = catalog.gameToTotal[gameId] ?: return@forEachIndexed
            if (total <= 0) return@forEachIndexed
            // Absent from the progress list simply means nothing earned yet: 0 of N, not "unknown".
            val earned = progress[gameId]
            PlayTime.recordAchievements(serial, earned?.first ?: 0, earned?.second ?: 0, total)
            matched++
        }
        onProgress(games.size, games.size)
        return "$matched of ${games.size} games matched"
    }

    // ---- catalogue ---------------------------------------------------------------------------

    private fun loadOrFetchCatalog(context: Context, userName: String, key: String): Catalog? {
        val file = File(MainActivityRuntime.assetCopyRoot(context), CATALOG_FILE)
        val fetchedAt = runCatching { MainActivityRuntime.prefs.getLong(KEY_CATALOG_FETCHED, 0L) }
            .getOrDefault(0L)
        val fresh = file.isFile && (System.currentTimeMillis() - fetchedAt) < CATALOG_TTL_MS
        if (fresh) {
            parseCatalog(runCatching { file.readText() }.getOrNull().orEmpty())?.let { return it }
        }
        val body = get("$API/API_GetGameList.php?i=$PS2_CONSOLE_ID&f=1&h=1" +
            "&z=${enc(userName)}&y=${enc(key)}") ?: return parseCatalog(
            runCatching { file.readText() }.getOrNull().orEmpty()  // stale beats nothing
        )
        val parsed = parseCatalog(body) ?: return null
        runCatching {
            file.writeText(body)
            MainActivityRuntime.prefs.edit()
                .putLong(KEY_CATALOG_FETCHED, System.currentTimeMillis()).apply()
        }
        return parsed
    }

    private fun parseCatalog(body: String): Catalog? {
        if (body.isEmpty()) return null
        return runCatching {
            val arr = JSONArray(body)
            val hashToGame = HashMap<String, Int>(arr.length() * 2)
            val gameToTotal = HashMap<Int, Int>(arr.length())
            for (i in 0 until arr.length()) {
                val g = arr.optJSONObject(i) ?: continue
                val id = g.optInt("ID", g.optInt("id", 0))
                if (id == 0) continue
                val total = g.optInt("NumAchievements", g.optInt("numAchievements", 0))
                gameToTotal[id] = total
                val hashes = g.optJSONArray("Hashes") ?: g.optJSONArray("hashes") ?: continue
                for (h in 0 until hashes.length()) {
                    hashes.optString(h).takeIf { it.isNotEmpty() }
                        ?.let { hashToGame[it.lowercase()] = id }
                }
            }
            if (hashToGame.isEmpty()) null else Catalog(hashToGame, gameToTotal)
        }.getOrNull()
    }

    // ---- user progress ----------------------------------------------------------------------

    /** RA game id -> (softcore awarded, hardcore awarded). */
    private fun fetchUserProgress(userName: String, key: String): Map<Int, Pair<Int, Int>>? {
        val out = HashMap<Int, Pair<Int, Int>>()
        var offset = 0
        while (true) {
            val body = get("$API/API_GetUserCompletionProgress.php?u=${enc(userName)}" +
                "&c=$PROGRESS_PAGE&o=$offset&y=${enc(key)}") ?: return if (offset == 0) null else out
            val page = runCatching { JSONObject(body) }.getOrNull() ?: return out
            val results = page.optJSONArray("Results") ?: page.optJSONArray("results") ?: return out
            for (i in 0 until results.length()) {
                val r = results.optJSONObject(i) ?: continue
                if (r.optInt("ConsoleID", r.optInt("consoleId", -1)) != PS2_CONSOLE_ID) continue
                val id = r.optInt("GameID", r.optInt("gameId", 0))
                if (id == 0) continue
                out[id] = r.optInt("NumAwarded", r.optInt("numAwarded", 0)) to
                    r.optInt("NumAwardedHardcore", r.optInt("numAwardedHardcore", 0))
            }
            val total = page.optInt("Total", page.optInt("total", 0))
            offset += results.length()
            if (results.length() == 0 || offset >= total) break
        }
        return out
    }

    // ---- hashing ----------------------------------------------------------------------------

    private fun cachedHash(serial: String): String? = runCatching {
        MainActivityRuntime.prefs.getString(HASH_PREFIX + serial, null)?.takeIf { it.isNotEmpty() }
    }.getOrNull()

    private fun hashAndCache(serial: String, path: String): String? {
        val hash = runCatching { NativeApp.getAchievementsHashForPath(path) }.getOrNull()
            ?.lowercase()?.takeIf { it.length == 32 } ?: return null
        runCatching {
            MainActivityRuntime.prefs.edit().putString(HASH_PREFIX + serial, hash).apply()
        }
        return hash
    }

    // ---- http -------------------------------------------------------------------------------

    private fun enc(s: String): String =
        java.net.URLEncoder.encode(s, "UTF-8")

    private fun get(url: String): String? = runCatching {
        (URL(url).openConnection() as HttpURLConnection).run {
            requestMethod = "GET"
            connectTimeout = 15_000
            readTimeout = 30_000
            setRequestProperty("Accept", "application/json")
            try {
                if (responseCode != HttpURLConnection.HTTP_OK) null
                else inputStream.bufferedReader().use { it.readText() }
            } finally {
                disconnect()
            }
        }
    }.getOrNull()
}
