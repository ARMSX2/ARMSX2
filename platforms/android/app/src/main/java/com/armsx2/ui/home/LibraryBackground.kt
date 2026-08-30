package com.armsx2.ui.home

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.runtime.mutableStateOf
import com.armsx2.runtime.MainActivityRuntime

/**
 * Optional user-chosen library background (#9). Stores a persisted content URI;
 * when unset the library falls back to the bundled default XMB-wave still image
 * (R.drawable.library_bg_xmb, drawn in HomeScreen). The user can pick a still image
 * or an animated GIF/WebP (Coil handles both); `clear()` reverts to the default.
 * (The background used to be a looping MP4 but that hurt performance, so it's a
 * static image now.)
 */
object LibraryBackground {
    private const val PREF = "library.background.uri"
    private const val PREF_ANIM = "library.background.animated2d"
    private const val PREF_FLURRY = "library.background.flurry"
    private const val PREF_FLURRY_PRESET = "library.background.flurry.preset"
    private const val PREF_SAVER_KIND = "library_saver_kind"
    private const val PREF_RSS_PRESET = "library_rss_preset"

    /**
     * Which saver is CURRENTLY running, written synchronously before its GL thread starts and
     * cleared when that thread exits in an orderly way. See [armSaver].
     */
    private const val PREF_ARMED = "library.saver.armed"
    val uri = mutableStateOf<String?>(null)

    /**
     * Force the lightweight 2D animated background ([LibraryWaveBackground]) even on devices where
     * the GLES3 XMB wave ([XmbGlView]) would run — i.e. let capable devices opt into the same
     * backdrop Mali / GL-fail devices already get. A user preference (#Luminz). Off = the GL wave.
     * No effect when a custom background image is set (that overrides everything), and no effect for
     * the devices that already fall back to the 2D wave.
     */
    val animated2D = mutableStateOf(false)

    /**
     * Draw an animated screensaver ([SaverGlView]) instead of the XMB wave. Which one is
     * [saverKind]; this is just the on/off.
     *
     * Takes precedence over [animated2D] and, like it, is overridden by a custom background
     * image. Off by default: it is a live particle simulation, and the library's animated
     * background has already been walked back once on performance grounds -- ARMSX2 shipped a
     * looping video here and removed it in 2.5.9 for exactly that reason. Opt-in keeps the
     * default cost where it is.
     */
    val flurry = mutableStateOf(false)

    /** Preset for the above. 99 = pick one at random each time the library opens. */
    val flurryPreset = mutableStateOf(99)

    /**
     * Which saver [flurry] runs: 0 = Flurry, then the Really Slick savers in the order of the
     * table in savers_jni.cpp -- 1 = Flux, 2 = Plasma, 3 = SolarWinds, 4 = Hyperspace, 5 = Lattice, 6 = Skyrocket.
     *
     * Flurry is Calum Robinson's (BSD-3-clause); the rest are Terry Welsh's Really Slick
     * Screensavers (GPL-2.0-or-later). They share the toggle above because only one background
     * can run at a time, and a single "animated background: on" reads better than one switch
     * per saver.
     */
    val saverKind = mutableStateOf(0)

    /** Preset for whichever Really Slick saver is selected, 1..6. 99 = pick one each time. */
    val rssPreset = mutableStateOf(99)

    /**
     * Set at startup when the previous run died with a saver on screen. Holds the [saverKind] that
     * was running so the library can say which one, and so the user knows their background was
     * turned off deliberately rather than forgotten. Read once and cleared by the reader.
     */
    val crashedSaver = mutableStateOf<Int?>(null)

    /** Display name for a [saverKind], for the message above. Matches the settings list. */
    fun saverName(kind: Int): String = when (kind) {
        1 -> "Flux"; 2 -> "Plasma"; 3 -> "SolarWinds"
        4 -> "Hyperspace"; 5 -> "Lattice"; 6 -> "Skyrocket"
        else -> "Flurry"
    }

    /**
     * Crash-loop breaker.
     *
     * The savers are native GL code, and native GL code can take the process down in ways no
     * `runCatching` can see -- a SIGSEGV in a driver, or a hang that Android resolves by killing
     * us. Because the choice is persisted and the library is the FIRST screen, a saver that dies
     * on startup dies again on every launch: the app never gets far enough for anyone to reach
     * Settings and switch it off. The only escape is clearing app data, which on Android takes
     * the memory cards and save states with it. A user hit exactly that and lost their saves.
     *
     * So the setting arms itself before the GL thread starts and disarms when that thread exits
     * normally. Finding it still armed at startup means last run ended while a saver was on
     * screen -- the background is switched off and the user is told which one did it. The write
     * must be commit() rather than apply(): apply() is asynchronous, and the whole point is that
     * the process may be about to die.
     */
    fun armSaver() {
        runCatching {
            MainActivityRuntime.prefs.edit().putInt(PREF_ARMED, saverKind.value).commit()
        }
    }

    /** Orderly teardown -- the saver ran without taking the process with it. Idempotent. */
    fun disarmSaver() {
        runCatching { MainActivityRuntime.prefs.edit().remove(PREF_ARMED).apply() }
    }

    private var loaded = false

    fun ensureLoaded() {
        if (loaded) return
        loaded = true
        uri.value = runCatching { MainActivityRuntime.prefs.getString(PREF, null) }.getOrNull()
        animated2D.value = runCatching { MainActivityRuntime.prefs.getBoolean(PREF_ANIM, false) }.getOrDefault(false)
        flurry.value = runCatching { MainActivityRuntime.prefs.getBoolean(PREF_FLURRY, false) }.getOrDefault(false)
        flurryPreset.value = runCatching { MainActivityRuntime.prefs.getInt(PREF_FLURRY_PRESET, 99) }.getOrDefault(99)
        saverKind.value = runCatching { MainActivityRuntime.prefs.getInt(PREF_SAVER_KIND, 0) }.getOrDefault(0)
        rssPreset.value = runCatching { MainActivityRuntime.prefs.getInt(PREF_RSS_PRESET, 99) }.getOrDefault(99)

        // Still armed = the previous run died with a saver up. Break the loop (see armSaver).
        val armed = runCatching { MainActivityRuntime.prefs.getInt(PREF_ARMED, -1) }.getOrDefault(-1)
        if (armed >= 0) {
            crashedSaver.value = armed
            setFlurry(false)
            disarmSaver()
        }
    }

    fun setAnimated2D(on: Boolean) {
        animated2D.value = on
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_ANIM, on).apply() }
    }

    fun setFlurry(on: Boolean) {
        flurry.value = on
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_FLURRY, on).apply() }
    }

    fun setFlurryPreset(preset: Int) {
        flurryPreset.value = preset
        runCatching { MainActivityRuntime.prefs.edit().putInt(PREF_FLURRY_PRESET, preset).apply() }
    }

    fun setSaverKind(kind: Int) {
        saverKind.value = kind
        runCatching { MainActivityRuntime.prefs.edit().putInt(PREF_SAVER_KIND, kind).apply() }
    }

    fun setRssPreset(preset: Int) {
        rssPreset.value = preset
        runCatching { MainActivityRuntime.prefs.edit().putInt(PREF_RSS_PRESET, preset).apply() }
    }

    /**
     * What the library should actually run right now, with 99 ("random") resolved to a concrete
     * preset. Called once when the view is created, so random means once per library open
     * rather than once per frame.
     */
    fun currentSpec(): SaverSpec = when (val kind = saverKind.value) {
        in 1..6 -> SaverSpec.Rss(
            effect = kind - 1,  // indexes the table in savers_jni.cpp
            preset = rssPreset.value.let { if (it in 1..6) it else (1..6).random() },
        )
        else -> SaverSpec.Flurry(flurryPreset.value)
    }

    fun set(context: Context, value: Uri) {
        runCatching {
            context.contentResolver.takePersistableUriPermission(value, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        uri.value = value.toString()
        runCatching { MainActivityRuntime.prefs.edit().putString(PREF, value.toString()).apply() }
    }

    fun clear() {
        uri.value = null
        runCatching { MainActivityRuntime.prefs.edit().remove(PREF).apply() }
    }
}
