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
     * Calum Robinson's 2002 Flurry screensaver as the live backdrop, ported from ARMSX3.
     *
     * Off by default, and deliberately so: it is a particle simulation rather than a still, and
     * this library already shipped a looping video once and lost it in 2.5.9 when the continuous
     * decode turned out to cost real performance. Opt-in for the same reason.
     */
    val flurry = mutableStateOf(false)

    /** Preset for the above. 99 = pick one at random each time the library opens. */
    val flurryPreset = mutableStateOf(99)

    private var loaded = false

    fun ensureLoaded() {
        if (loaded) return
        loaded = true
        uri.value = runCatching { MainActivityRuntime.prefs.getString(PREF, null) }.getOrNull()
        animated2D.value = runCatching { MainActivityRuntime.prefs.getBoolean(PREF_ANIM, false) }.getOrDefault(false)
        flurry.value = runCatching { MainActivityRuntime.prefs.getBoolean(PREF_FLURRY, false) }.getOrDefault(false)
        flurryPreset.value = runCatching { MainActivityRuntime.prefs.getInt(PREF_FLURRY_PRESET, 99) }.getOrDefault(99)
    }

    fun setFlurry(on: Boolean) {
        flurry.value = on
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_FLURRY, on).apply() }
    }

    fun setFlurryPreset(preset: Int) {
        flurryPreset.value = preset
        runCatching { MainActivityRuntime.prefs.edit().putInt(PREF_FLURRY_PRESET, preset).apply() }
    }

    fun setAnimated2D(on: Boolean) {
        animated2D.value = on
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_ANIM, on).apply() }
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
