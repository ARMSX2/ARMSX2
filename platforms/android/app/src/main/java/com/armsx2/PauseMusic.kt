package com.armsx2

import android.content.Context
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.util.Log
import androidx.compose.runtime.mutableStateOf
import androidx.core.content.edit
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

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
 * No audio focus request and no deference to other playing audio — both would break it. On an
 * overlay pause the game keeps its audio DEVICE open and merely underruns to silence: pauseForOverlay
 * calls setOutputPauseSuppressed(true) so Android does not reclaim an idle low-latency stream and
 * stall the resume (#333). That means AudioManager reports the game's stream as "active" the whole
 * time the menu is up, even though nothing is audible — so an isMusicActive() check (as LibraryMusic
 * uses to stay out of Spotify's way) would make this never play at all. Instead we simply play a
 * second stream over the silent game one, and do not request focus so the game's own stream and its
 * resume are left completely untouched.
 */
object PauseMusic {
    private const val TAG = "PauseMusic"
    private const val EnabledKey = "pauseMusic.enabled"
    private const val VolumeKey = "pauseMusic.volumePercent"
    private const val CustomNameKey = "pauseMusic.customName"
    private const val DefaultVolumePercent = 45

    // Ease in from silence rather than slamming on at full volume, and ease back out so resuming the
    // game isn't a hard cut. Ambient track, so a long, gentle fade-in suits it.
    private const val FadeInMs = 1600L
    private const val FadeOutMs = 500L
    private const val FadeSteps = 32

    /** On by default — the menu was silent and this fills it; the toggle turns it off. */
    val enabled = mutableStateOf(true)
    val volumePercent = mutableStateOf(DefaultVolumePercent)
    /** Display name of an imported track, or null when using the bundled one. */
    val customName = mutableStateOf<String?>(null)

    private var player: MediaPlayer? = null

    // Drives the volume ramps. Main.immediate so setVolume lands on the same thread the player lives
    // on; SupervisorJob so one cancelled ramp never tears the scope down.
    private val scope = CoroutineScope(Dispatchers.Main.immediate + SupervisorJob())
    private var fadeJob: Job? = null

    private fun gain(): Float = (volumePercent.value.coerceIn(0, 100)) / 100f

    private fun cancelFade() {
        fadeJob?.cancel()
        fadeJob = null
    }

    /** Imported track, stored extension-less — MediaPlayer sniffs the container. */
    private fun customFile(context: Context): java.io.File =
        java.io.File(context.filesDir, "pause_music_custom")

    fun load() {
        enabled.value = MainActivityRuntime.prefs.getBoolean(EnabledKey, true)
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
        // A manual change is an explicit request for this level — take over any in-progress fade so
        // the slider doesn't fight the ramp.
        cancelFade()
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
     * Start playing (or resume a paused player).
     *
     * No isMusicActive() guard, unlike LibraryMusic — see the class header: the game's own audio
     * device stays open and silent while the menu is up, so that check would always see it as active
     * and this would never play.
     *
     * Built by hand rather than MediaPlayer.create() for the same reason as LibraryMusic: create()
     * prepares internally, so attributes set afterwards land on an already-prepared player and are
     * ignored — and those attributes are what make Android treat this as media.
     */
    fun start(context: Context) {
        if (!enabled.value) return
        if (player != null) {
            // Already have a player — paused in the background, or a fade-out we're interrupting by
            // reopening the menu. Cancel any ramp, make sure it's running, and go straight to full
            // volume (no dip): a resume from background was already at full, and a snap up on a quick
            // reopen reads as instant rather than abrupt.
            cancelFade()
            runCatching {
                player?.let {
                    if (!it.isPlaying) it.start()
                    it.setVolume(gain(), gain())
                }
            }
            return
        }
        val created = runCatching {
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
                setVolume(0f, 0f) // start silent; fadeIn() ramps up
                prepare()
                start()
                player = this
            }
        }.isSuccess
        if (created) fadeIn() else Log.w(TAG, "start failed")
    }

    /** Ramp from silence up to the set volume so the track eases in instead of slamming on. */
    private fun fadeIn() {
        cancelFade()
        fadeJob = scope.launch {
            for (i in 1..FadeSteps) {
                val v = gain() * i / FadeSteps // read gain() each step so a live volume change tracks
                runCatching { player?.setVolume(v, v) }
                delay(FadeInMs / FadeSteps) // throws on cancel, ending the ramp
            }
            runCatching { player?.setVolume(gain(), gain()) }
        }
    }

    /**
     * Fade out over [FadeOutMs], then release — the overlay closed, the game resumed, or the toggle
     * went off. Ownership of the player is handed to the fade coroutine immediately (player = null)
     * so a start() during the fade builds a fresh player and the two cross-fade; the try/finally
     * releases the old one even if that start() cancels this ramp, so nothing leaks.
     */
    fun stop() {
        val p = player ?: return
        player = null
        cancelFade()
        fadeJob = scope.launch {
            try {
                for (i in FadeSteps - 1 downTo 0) {
                    val v = gain() * i / FadeSteps
                    runCatching { p.setVolume(v, v) }
                    delay(FadeOutMs / FadeSteps)
                }
            } finally {
                runCatching { if (p.isPlaying) p.stop() }
                runCatching { p.release() }
            }
        }
    }

    /** Suspend without releasing — the app went to the background with the menu still up. */
    fun pause() {
        cancelFade()
        runCatching { player?.takeIf { it.isPlaying }?.pause() }
    }

    fun isPlaying(): Boolean = runCatching { player?.isPlaying == true }.getOrDefault(false)
}
