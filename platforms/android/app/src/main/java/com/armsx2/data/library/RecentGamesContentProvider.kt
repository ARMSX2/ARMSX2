package com.armsx2.data.library

import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.content.SharedPreferences
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import org.json.JSONArray

/**
 * Exposes the recently-played list over Binder IPC instead of only the
 * `recent_games.json` file [GameLibraryRepository] writes to systemDirPosix().
 * That path resolves to Android/data/<pkg>/files/ on the volume-choice model
 * (the one folder writable without extra permissions under scoped storage),
 * which the OS blocks from cross-app filesystem reads regardless of storage
 * permission grants. A ContentProvider sidesteps that: it runs in-process,
 * reads its own private SharedPreferences normally, and serves the data out
 * over IPC, no storage permissions needed on either side.
 *
 * Reads SharedPreferences directly rather than going through
 * [GameLibraryRepository]/MainActivityRuntime.prefs, which is only
 * initialized once the main activity has launched; a query from a companion
 * app before that point would otherwise crash or return nothing.
 *
 * Feature contributed by misantronic (PR #566); the opt-in gate in [query] was
 * added on merge.
 */
class RecentGamesContentProvider : ContentProvider() {

    companion object {
        private const val PREFS_NAME = "ARMSX2"
        private const val KEY_GAMES_CACHE = "gamesCache"
        private const val KEY_RECENT_URIS = "recentGameUris"
        private const val LAST_PLAYED_PREFIX = "playtime.last."
        private const val MAX_RECENT_LOOKUP = 12

        /** Master switch for this provider, default OFF — see the gate in [query].
         *  Lives in the same "ARMSX2" prefs file the app writes, so the App-settings
         *  toggle and this provider are reading one value and not two. */
        const val KEY_SHARE_ENABLED = "library.shareRecentGames"

        const val PATH_GAMES = "games"
        const val COLUMN_URI = "uri"
        const val COLUMN_TITLE = "title"
        const val COLUMN_SERIAL = "serial"
        const val COLUMN_EXT = "ext"
        const val COLUMN_PLATFORM = "platform"
        const val COLUMN_LAST_PLAYED = "lastPlayed"

        private val COLUMNS = arrayOf(
            COLUMN_URI,
            COLUMN_TITLE,
            COLUMN_SERIAL,
            COLUMN_EXT,
            COLUMN_PLATFORM,
            COLUMN_LAST_PLAYED,
        )
    }

    override fun onCreate(): Boolean = true

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor {
        val cursor = MatrixCursor(COLUMNS)
        val context = context ?: return cursor
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

        // ★ OPT-IN, and the gate has to be HERE rather than in the manifest.
        //
        // The provider is android:exported="true" with no permission, which it must be for a
        // third-party companion app to reach it at all — a signature-level permission would only
        // admit apps we sign. Exported and ungated means every app on the device, holding no
        // permissions of any kind, can read the library: titles, serials, last-played times, and
        // the file URIs, which carry the user's folder layout and often their real name.
        //
        // So sharing is the user's decision and it starts off, matching how the second-screen
        // panel and Discord presence are handled. An empty cursor rather than null: null is the
        // failure signal a ContentResolver caller has to special-case, and "sharing is off" is a
        // legitimate answer, not an error.
        if (!prefs.getBoolean(KEY_SHARE_ENABLED, false)) {
            return cursor
        }

        val recentUris = readRecentUris(prefs)
        if (recentUris.isEmpty()) {
            return cursor
        }

        val gamesByUri = readGamesCache(prefs)
        recentUris.take(MAX_RECENT_LOOKUP).forEach { uriString ->
            val game = gamesByUri[uriString] ?: return@forEach
            val lastPlayed = game.serial
                ?.let { serial -> prefs.getLong(LAST_PLAYED_PREFIX + serial, 0L) }
                ?.takeIf { it > 0L }

            cursor.addRow(
                arrayOf<Any?>(
                    game.uri,
                    game.title,
                    game.serial,
                    game.ext,
                    game.platform,
                    lastPlayed,
                )
            )
        }

        return cursor
    }

    override fun getType(uri: Uri): String? = null

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int = 0

    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?): Int = 0

    private data class CachedGame(val uri: String, val title: String, val serial: String?, val ext: String, val platform: String)

    private fun readRecentUris(prefs: SharedPreferences): List<String> {
        val raw = prefs.getString(KEY_RECENT_URIS, null) ?: return emptyList()
        return runCatching {
            val array = JSONArray(raw)
            List(array.length()) { array.getString(it) }
        }.getOrDefault(emptyList())
    }

    private fun readGamesCache(prefs: SharedPreferences): Map<String, CachedGame> {
        val raw = prefs.getString(KEY_GAMES_CACHE, null) ?: return emptyMap()
        return runCatching {
            val array = JSONArray(raw)
            buildMap {
                repeat(array.length()) { index ->
                    val item = array.getJSONObject(index)
                    val uriString = item.getString("uri")
                    put(
                        uriString,
                        CachedGame(
                            uri = uriString,
                            title = item.getString("title"),
                            serial = if (item.isNull("serial")) null else item.optString("serial").takeIf(String::isNotBlank),
                            ext = item.optString("ext").ifBlank { uriString.substringAfterLast('.', "").uppercase() },
                            platform = item.optString("platform"),
                        )
                    )
                }
            }
        }.getOrDefault(emptyMap())
    }
}
