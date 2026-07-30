package com.armsx2

import android.content.Context
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.util.Log
import androidx.compose.runtime.mutableStateOf
import androidx.core.content.edit
import com.armsx2.runtime.MainActivityRuntime

/**
 * Music for the in-game pause menu, which is otherwise silent — and people sit in it, browsing
 * settings or reading achievements, for minutes at a time.
 *
 * Deliberately NOT [LibraryMusic] with a different gate. That one refuses to play unless
 * `eState == STOPPED`, which is exactly the opposite of what this needs, and the two have opposite
 * lifetimes: the library track runs for as long as you are in the library, this one for as long as
 * the pause overlay is up. Sharing one player would mean one of them constantly fighting the
 * other's start conditions.
 *
 * Audio focus is deliberately NOT requested. The game's own audio is already suspended while paused
 * (`SPU2::SetOutputPaused`), so there is nothing to duck, and grabbing focus for a menu jingle would
 * stop whatever the player has going in another app. If they are listening to something else, this
 * simply stays quiet — see the isMusicActive check.
 */
object PauseMusic {
    private const val TAG = "PauseMusic"
    private const val EnabledKey = "pauseMusic.enabled"
    private const val VolumeKey = "pauseMusic.volumePercent"
    private const val CustomNameKey = "pauseMusic.customName"
    private const val DefaultVolumePercent = 45

    /** Off by default: unexpected audio when you open a menu is startling, so it is opt-in. */
    val enabled = mutableStateOf(false)
    val volumePercent = mutableStateOf(DefaultVolumePercent)
    /** Display name of an imported track, or null when using the bundled one. */
    val customName = mutableStateOf<String?>(null)

    private var player: MediaPlayer? = null

    private fun gain(): Float = (volumePercent.value.coerceIn(0, 100)) / 100f

    /** Imported track, stored extension-less — MediaPlayer sniffs the container. */
    private fun customFile(context: Context): java.io.File =
        java.io.File(context.filesDir, "pause_music_custom")

    fun load() {
        enabled.value = MainActivityRuntime.prefs.getBoolean(EnabledKey, false)
        volumePercent.value = MainActivityRuntime.prefs.getInt(VolumeKey, DefaultVolumePercent)
        customName.value = MainActivityRuntime.prefs.getString(CustomNameKey, null)
    }

    fun set(context: Context, value: Boolean) {
        enabled.value = value
        MainActivityRuntime.prefs.edit { putBoolean(EnabledKey, value) }
        // Start immediately if a menu is already up — the most likely place to flip this toggle is
        // the in-game Settings screen, which is inGameScreen (overlayVisible is false by then), so
        // checking only the pause menu would have made the switch look dead exactly where it's used.
        if (!value) {
            stop()
        } else if (com.armsx2.ui.WindowImpl.overlayVisible.value ||
            com.armsx2.ui.WindowImpl.inGameScreen.value != null
        ) {
            start(context)
        }
    }

    fun setVolume(percent: Int) {
        val p = percent.coerceIn(0, 100)
        volumePercent.value = p
        MainActivityRuntime.prefs.edit { putInt(VolumeKey, p) }
        runCatching { player?.setVolume(gain(), gain()) }
    }

    fun setCustomTrack(context: Context, uri: android.net.Uri, displayName: String): Boolean {
        val ok = runCatching {
            context.contentResolver.openInputStream(uri)?.use { input ->
                customFile(context).outputStream().use { input.copyTo(it) }
            } != null
        }.getOrDefault(false)
        if (ok) {
            customName.value = displayName
            MainActivityRuntime.prefs.edit { putString(CustomNameKey, displayName) }
            restart(context)
        }
        return ok
    }

    fun clearCustomTrack(context: Context) {
        runCatching { customFile(context).delete() }
        customName.value = null
        MainActivityRuntime.prefs.edit { remove(CustomNameKey) }
        restart(context)
    }

    private fun restart(context: Context) {
        val wasPlaying = player != null
        stop()
        if (wasPlaying) start(context)
    }

    /**
     * Start, if the pause overlay is up and nothing else is playing.
     *
     * Built by hand rather than MediaPlayer.create() for the same reason as LibraryMusic: create()
     * prepares internally, so attributes set afterwards land on an already-prepared player and are
     * ignored — and those attributes are what make Android treat this as media.
     */
    fun start(context: Context) {
        if (!enabled.value) return
        if (player != null) {
            runCatching { player?.takeIf { !it.isPlaying }?.start() }
            return
        }
        // Defer to whatever the player already has going in another app.
        val am = context.getSystemService(Context.AUDIO_SERVICE) as? android.media.AudioManager
        if (am?.isMusicActive == true) {
            Log.i(TAG, "another app is playing audio; not starting pause music")
            return
        }
        runCatching {
            MediaPlayer().apply {
                setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build()
                )
                val custom = customFile(context)
                if (customName.value != null && custom.length() > 0L) {
                    setDataSource(custom.absolutePath)
                } else {
                    setDataSource(
                        context,
                        android.net.Uri.parse("android.resource://${context.packageName}/${R.raw.pause_music}"),
                    )
                }
                isLooping = true
                setVolume(gain(), gain())
                prepare()
                start()
                player = this
            }
        }.onFailure { Log.w(TAG, "start failed", it) }
    }

    /** Stop and release — the overlay closed, the game resumed, or the toggle went off. */
    fun stop() {
        player?.let { p ->
            runCatching { if (p.isPlaying) p.stop() }
            runCatching { p.release() }
        }
        player = null
    }

    /** Suspend without releasing — the app went to the background with the menu still up. */
    fun pause() {
        runCatching { player?.takeIf { it.isPlaying }?.pause() }
    }

    fun isPlaying(): Boolean = runCatching { player?.isPlaying == true }.getOrDefault(false)
}
