package com.armsx2.data.library

import android.content.Context
import android.content.SharedPreferences
import org.json.JSONArray

/**
 * Which apps may read the recently-played list, stored per calling package.
 *
 * The provider is exported without a permission — it has to be, for a third-party companion to
 * reach it at all — so "may this caller read the library" cannot be answered by the manifest and
 * has to be answered here, against the package Binder reports for the caller.
 *
 * Per-package rather than one global switch: the switch means "every app on the device, forever",
 * which is a far larger grant than the one the user actually wants to make, and it can only be
 * discovered by hunting through Settings. A grant recorded against one package can be requested
 * in context by the app that needs it (see RecentGamesAccessActivity) and revoked on its own.
 *
 * Written with commit(), not apply(): the reader is a DIFFERENT process and can be queried the
 * moment a grant returns, and apply() only guarantees the in-memory value.
 */
internal object RecentGamesAccess {

    const val PREFS_NAME = "ARMSX2"

    /** Package names granted read access, as a JSON array of strings. */
    const val KEY_GRANTED_PACKAGES = "library.shareRecentGames.packages"

    /** Package names whose prompt the user turned down, so the same prompt is not put in front
     *  of them on every single query. Held HERE rather than by the asking app: this app owns the
     *  settings switch, so it is the only one that can tell the difference between "still no" and
     *  "the user has since changed their mind" — and it clears this the moment the switch moves. */
    const val KEY_DECLINED_PACKAGES = "library.shareRecentGames.declined"

    fun prefs(context: Context): SharedPreferences =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun grantedPackages(prefs: SharedPreferences): Set<String> = decode(prefs, KEY_GRANTED_PACKAGES)

    private fun decode(prefs: SharedPreferences, key: String): Set<String> {
        val raw = prefs.getString(key, null) ?: return emptySet()
        return runCatching {
            val array = JSONArray(raw)
            buildSet {
                repeat(array.length()) { index ->
                    array.optString(index).takeIf { it.isNotBlank() }?.let(::add)
                }
            }
        }.getOrDefault(emptySet())
    }

    fun isGranted(prefs: SharedPreferences, packageName: String?): Boolean =
        packageName != null && packageName in grantedPackages(prefs)

    fun grant(prefs: SharedPreferences, packageName: String) {
        write(prefs, KEY_GRANTED_PACKAGES, grantedPackages(prefs) + packageName)
        // Allowing supersedes any earlier refusal from the same app.
        write(prefs, KEY_DECLINED_PACKAGES, declinedPackages(prefs) - packageName)
    }

    fun declinedPackages(prefs: SharedPreferences): Set<String> = decode(prefs, KEY_DECLINED_PACKAGES)

    fun isDeclined(prefs: SharedPreferences, packageName: String?): Boolean =
        packageName != null && packageName in declinedPackages(prefs)

    fun decline(prefs: SharedPreferences, packageName: String) {
        write(prefs, KEY_DECLINED_PACKAGES, declinedPackages(prefs) + packageName)
    }

    /** Forget every refusal, so the next app to ask gets a fresh prompt. Called when the sharing
     *  switch is touched: moving it is the user reconsidering, and a stale "no" recorded before
     *  that would otherwise keep a companion silently shut out with nothing to show for it. */
    fun clearDeclined(prefs: SharedPreferences) {
        write(prefs, KEY_DECLINED_PACKAGES, emptySet())
    }

    /** Drop every per-app grant. The settings switch is the one "stop sharing" control, so it has
     *  to clear the individual grants too, or turning it off would leave apps still reading. */
    fun revokeAll(prefs: SharedPreferences) {
        write(prefs, KEY_GRANTED_PACKAGES, emptySet())
    }

    private fun write(prefs: SharedPreferences, key: String, packages: Set<String>) {
        val encoded = JSONArray().apply { packages.sorted().forEach(::put) }.toString()
        runCatching { prefs.edit().putString(key, encoded).commit() }
    }
}
