package com.armsx2

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.MediaPlayer
import android.os.Build
import android.util.Log
import androidx.compose.runtime.mutableStateOf
import androidx.core.content.edit
import com.armsx2.runtime.MainActivityRuntime
import java.io.File

/**
 * Ambient background music for the game library — the "console dashboard" feel.
 *
 * DELIBERATELY LIBRARY-ONLY. It plays while no VM is running and stops the moment a
 * game boots, which is both what a PS2/Xbox dashboard does and what keeps it clear of
 * the emulator's own audio: SPU2 output goes through Oboe, and Android has been
 * observed reclaiming that stream when the VM pauses (issue #333). A second long-lived
 * stream competing over gameplay would land straight on top of that.
 *
 * Track: "Calm Ambient 1 (Synthwave 4k)" by cynicmusic (The Cynic Project), released
 * CC0 / public domain on OpenGameArt. CC0 imposes no attribution requirement, but the
 * author asks for credit and we give it — see the About screen.
 *
 * Three screens play their own track in its place ([playTheme]): the Icon Museum, "Another
 * August" by The Cynic Project (cynicmusic.com, pixelsphere.org), which asks for attribution,
 * given in the About screen and MC Icon Info; Online Icons, "Next to You", which doesn't; and the
 * texture screen, "November Snow" by The Cynic Project, CC0 from the Pixelsphere soundtrack, which
 * asks for the same attribution, given in the About screen.
 * Going in and out of them, the two tracks mix for a few seconds before the old one slowly fades.
 */
object LibraryMusic {
    private const val TAG = "LibraryMusic"
    private const val EnabledKey = "ui.libraryMusic"

    /** Default of 15% — the shipped 55% was reported as far too loud. Background
     *  music should sit well under the UI, not command it. User-adjustable via the
     *  slider in App settings; 0 = silent (the toggle is the real off switch). */
    private const val VolumeKey = "ui.libraryMusic.volume"
    private const val CustomNameKey = "ui.libraryMusic.customName"
    private const val DefaultVolumePercent = 15

    val enabled = mutableStateOf(true)

    /** 0..100, backs the App-settings slider. Applied live to the running player. */
    val volumePercent = mutableStateOf(DefaultVolumePercent)

    private fun gain(): Float = (volumePercent.value.coerceIn(0, 100)) / 100f

    /** Display name of the user's own track, or null when playing the bundled default.
     *  The app never ships or redistributes user tracks — this only plays a file the user
     *  chose from their own device, the same as importing a texture pack or a skin. */
    val customName = mutableStateOf<String?>(null)

    /** The imported track is copied here (extension-less; MediaPlayer sniffs the format), so
     *  playback doesn't depend on holding a SAF permission or the source file staying put. */
    private fun customFile(context: Context): File =
        File(File(context.filesDir, "librarymusic").apply { mkdirs() }, "track")

    private var player: MediaPlayer? = null
    private var focusRequest: AudioFocusRequest? = null

    /** A screen's own track playing in the library's place while it is open (the Icon Museum's,
     *  Online Icons'), or null for the library track. See [playTheme]. */
    private var theme: Int? = null

    /** Where the library track was when it last went quiet for a theme, so it goes on from there. */
    private var libraryPositionMs = 0

    // ---- going in and out of a screen's own track ----
    //
    // The Icon Museum's track mixes with the library's: the next fades in over MIX_IN_MS while the
    // one before plays on at full for MIX_HOLD_MS, then slowly fades out over MIX_OUT_MS. A quick
    // crossfade sounded abrupt to testers ("let the tracks mix for a bit before SLOWLY fading out
    // the previous track").
    private const val MIX_IN_MS = 2500L
    private const val MIX_HOLD_MS = 2500L
    private const val MIX_OUT_MS = 3000L

    // Online Icons' track clashed with the library's in a mix, so those two go one after the other:
    // the one playing fades out, a moment's quiet, then the next fades in.
    private const val SEQ_OUT_MS = 1500L
    private const val SEQ_GAP_MS = 600L
    private const val SEQ_IN_MS = 2000L
    private val oneAfterTheOther = setOf(R.raw.online_icons_music)

    /** Which track [player] is: a theme's raw resource, or null for the library track. */
    private var playerTrack: Int? = null

    /** How loud [player] is now, as a share of [gain] (MediaPlayer can't say), and its fade in:
     *  from [inFrom], starting at [inStart] (which can be ahead, one after the other), over [inMs]. */
    private var playerLevel = 1f
    private var inFrom = 1f
    private var inStart = 0L
    private var inMs = MIX_IN_MS

    /** A track on its way out: it plays on at [from] until [start] + [hold], then fades to nothing
     *  over [outMs]. */
    private class Fading(val player: MediaPlayer, val track: Int?, val from: Float, val start: Long, val hold: Long, val outMs: Long) {
        var level = from
    }

    /** One after the other, the next track starts only when its fade in does, so the quiet before
     *  it doesn't use up the start of the song. */
    private var pendingContext: Context? = null
    private var pendingForce = false
    private val pendingStart = Runnable { pendingContext?.let { start(it, force = pendingForce) } }
    private val fading = ArrayList<Fading>()

    private val main = android.os.Handler(android.os.Looper.getMainLooper())
    private val fadeStep = object : Runnable {
        override fun run() {
            val now = android.os.SystemClock.uptimeMillis()
            val x = ((now - inStart).toFloat() / inMs).coerceIn(0f, 1f)
            playerLevel = inFrom + (1f - inFrom) * smooth(x)
            val each = fading.iterator()
            while (each.hasNext()) {
                val f = each.next()
                val y = ((now - f.start - f.hold).toFloat() / f.outMs).coerceIn(0f, 1f)
                f.level = f.from * kotlin.math.cos(y * Math.PI / 2).toFloat()
                if (y >= 1f) {
                    release(f.player, f.track)
                    each.remove()
                }
            }
            applyLevels()
            if (x < 1f || fading.isNotEmpty()) main.postDelayed(this, 20)
        }
    }

    /** Eases in and out: no sudden change at either end. */
    private fun smooth(x: Float) = x * x * (3f - 2f * x)

    /** True when we stopped for something temporary (a call, another app ducking us)
     *  and should resume ourselves when focus comes back — as opposed to being off. */
    private var pausedForFocus = false

    fun load() {
        enabled.value = MainActivityRuntime.prefs.getBoolean(EnabledKey, true)
        volumePercent.value = MainActivityRuntime.prefs.getInt(VolumeKey, DefaultVolumePercent)
        customName.value = MainActivityRuntime.prefs.getString(CustomNameKey, null)
    }

    /** Import a user-picked audio file as the library track, replacing the default. Copies it
     *  into app-private storage and restarts playback so the change is heard immediately.
     *  Returns true on success. */
    fun setCustomTrack(context: Context, uri: android.net.Uri, displayName: String): Boolean {
        val ok = runCatching {
            context.contentResolver.openInputStream(uri)?.use { ins ->
                customFile(context).outputStream().use { ins.copyTo(it) }
            } != null
        }.getOrDefault(false)
        if (!ok || customFile(context).length() == 0L) {
            customFile(context).delete()
            return false
        }
        customName.value = displayName
        MainActivityRuntime.prefs.edit { putString(CustomNameKey, displayName) }
        restart(context)
        return true
    }

    /** Drop the user track and go back to the bundled default. */
    fun clearCustomTrack(context: Context) {
        customFile(context).delete()
        customName.value = null
        MainActivityRuntime.prefs.edit { remove(CustomNameKey) }
        restart(context)
    }

    /** Stop and re-start so a track/volume change is heard now (only replays in the library).
     *  Forces past the "is another app playing?" guard: we just stopped our OWN stream, which
     *  AudioManager can still briefly report as active, and this is a user-initiated change
     *  (picking or resetting the track) so we always want it heard — without force, Reset went
     *  silent until a game boot/exit cleared the transient (KamFretoZ). */
    private fun restart(context: Context) {
        stop(context)
        start(context, force = true)
    }

    /** Set the music volume (0..100) and apply it live to a playing track. */
    fun setVolume(percent: Int) {
        val p = percent.coerceIn(0, 100)
        volumePercent.value = p
        MainActivityRuntime.prefs.edit { putInt(VolumeKey, p) }
        applyLevels()
    }

    /** True while the track is actually audible — drives the cold-start retry below. */
    fun isPlaying(): Boolean = runCatching { player?.isPlaying == true }.getOrDefault(false)

    fun set(context: Context, value: Boolean) {
        enabled.value = value
        MainActivityRuntime.prefs.edit { putBoolean(EnabledKey, value) }
        if (value) start(context) else stop(context)
    }

    /** Always the application's AudioManager. Focus belongs to the AudioManager that asked for it,
     *  so asking through another context's (the screen's, then the app's for the delayed start one
     *  after the other) made us a second client: the system took focus from the first, our own
     *  listener heard AUDIOFOCUS_LOSS and released the track that had just started. */
    private fun audioManager(context: Context): AudioManager? =
        context.applicationContext.getSystemService(Context.AUDIO_SERVICE) as? AudioManager

    /**
     * True when another app is playing something the user is listening to: music, a podcast,
     * a video. Deliberately narrower than AudioManager.isMusicActive(), which is true for ANY
     * live stream on the media channel, games included: an emulator left in the background with
     * a game paused keeps its silent stream open for hours, and that alone kept ARMSX3's library
     * silent all afternoon on the Odin 3 (2026-09-23; ARMSX3 has the same fix).
     *
     * The usage alone cannot tell: game streams come as USAGE_GAME or USAGE_MEDIA (our own SPU2
     * output sets none, so it is USAGE_MEDIA). What media apps do and emulators do not is say
     * what they play: music, a film, speech (Spotify, YouTube, podcast apps, and our own splash
     * and pause-menu tracks all set it). A raw game stream leaves the content type unknown, so
     * it never counts, and neither does our own SPU2 output still closing after a game exits.
     * Apps are only given the players that are live right now, so no state check is needed.
     */
    private fun otherMediaPlaying(am: AudioManager): Boolean =
        runCatching {
            am.activePlaybackConfigurations.any {
                val attributes = it.audioAttributes
                attributes.usage in MEDIA_USAGES && attributes.contentType in MEDIA_CONTENT
            }
        }.getOrElse { am.isMusicActive }

    private val MEDIA_USAGES = setOf(AudioAttributes.USAGE_MEDIA, AudioAttributes.USAGE_UNKNOWN)
    private val MEDIA_CONTENT = setOf(
        AudioAttributes.CONTENT_TYPE_MUSIC,
        AudioAttributes.CONTENT_TYPE_MOVIE,
        AudioAttributes.CONTENT_TYPE_SPEECH,
    )

    /**
     * Begin playing, if we should. No-ops when the setting is off, a VM is running, or
     * another app is already playing music, a podcast or a video.
     *
     * The [otherMediaPlaying] check is the difference between a nice touch and a hostile one:
     * without it, opening the app over someone's podcast or Spotify starts a second
     * stream on top of theirs. Deferring to whoever is already playing costs us nothing
     * — the user can still toggle it on explicitly.
     */
    fun start(context: Context, force: Boolean = false) {
        if (!enabled.value) return
        if (MainActivityRuntime.eState.value != EmuState.STOPPED) return
        if (player != null) { resume(); return }
        val am = audioManager(context)
        // Skip the "someone else is playing" deference when forced — a user-initiated track
        // change is an explicit request to hear our music now (see restart()).
        if (!force && am != null && otherMediaPlaying(am)) {
            Log.i(TAG, "another app is playing audio; not starting library music")
            return
        }
        if (am != null && !requestFocus(am)) return

        // Built by hand rather than MediaPlayer.create(): create() does setDataSource +
        // prepare internally, so attributes set afterwards land on an already-prepared
        // player and are ignored. Android routes and ducks by those attributes, so
        // getting them applied before prepare() is what makes this behave like media
        // rather than a system sound.
        runCatching {
            MediaPlayer().apply {
                setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build()
                )
                val custom = customFile(context)
                val themeTrack = theme
                if (themeTrack != null) {
                    setDataSource(context, android.net.Uri.parse("android.resource://${context.packageName}/$themeTrack"))
                } else if (customName.value != null && custom.length() > 0L) {
                    setDataSource(custom.absolutePath)
                } else {
                    setDataSource(
                        context,
                        android.net.Uri.parse("android.resource://${context.packageName}/${R.raw.library_music}"),
                    )
                }
                isLooping = true
                // Silent at first when a crossfade brings it in (crossTo), full otherwise.
                setVolume(gain() * playerLevel, gain() * playerLevel)
                prepare()
                if (themeTrack == null && libraryPositionMs > 0) {
                    runCatching { seekTo(libraryPositionMs) }
                    libraryPositionMs = 0
                }
                start()
                player = this
                playerTrack = themeTrack
            }
        }.onFailure { Log.w(TAG, "start failed", it) }
    }

    /**
     * Plays [track] (a raw resource) in the library track's place while a screen that has its own
     * is open: the Icon Museum, Online Icons. Same switch, volume and deference to other apps'
     * audio as the library track: it only takes over from our own music, never starts over
     * someone's podcast. [endTheme] hands back, and the library track goes on where it was.
     */
    fun playTheme(context: Context, track: Int) {
        if (theme == track) return
        theme = track
        crossTo(context)
    }

    /** Back to the library track, from where it was, when [track] is still the one playing. */
    fun endTheme(context: Context, track: Int) {
        if (theme != track) return
        theme = null
        crossTo(context)
    }

    /**
     * Goes over to whatever should play now ([theme], else the library track). With the Icon
     * Museum the next fades in while the one playing mixes with it at full, then slowly fades out;
     * with Online Icons the one playing fades out, and after a moment's quiet the next fades in.
     * Back and forth quickly, a track still on its way out comes back from where it had got to,
     * and nothing restarts.
     */
    private fun crossTo(context: Context) {
        val want = theme
        val now = android.os.SystemClock.uptimeMillis()
        main.removeCallbacks(pendingStart)
        // Our own music going means the swap is ours to make; starting past the other-media
        // check, as restart() does, since our stream may still be reported.
        val ours = player != null || fading.isNotEmpty()
        val leaving = player?.let { playerTrack }
        val sequential = want in oneAfterTheOther || leaving in oneAfterTheOther
        var waitMs = 0L
        player?.let { p ->
            if (runCatching { p.isPlaying }.getOrDefault(false)) {
                fading += if (sequential) {
                    waitMs = SEQ_OUT_MS + SEQ_GAP_MS
                    Fading(p, playerTrack, playerLevel, now, 0L, SEQ_OUT_MS)
                } else {
                    // At full it mixes first; caught while still fading in, it fades out from there.
                    Fading(p, playerTrack, playerLevel, now, if (playerLevel >= 0.99f) MIX_HOLD_MS else 0L, MIX_OUT_MS)
                }
            } else {
                release(p, playerTrack)
            }
        }
        player = null
        pausedForFocus = false
        inMs = if (sequential) SEQ_IN_MS else MIX_IN_MS
        val back = fading.firstOrNull { it.track == want }
        if (back != null) {
            fading.remove(back)
            player = back.player
            playerTrack = want
            inFrom = back.level
            playerLevel = back.level
            inStart = now
        } else {
            inFrom = 0f
            playerLevel = 0f
            inStart = now + waitMs
            if (waitMs > 0) {
                pendingContext = context.applicationContext
                pendingForce = ours
                main.postDelayed(pendingStart, waitMs)
            } else {
                start(context, force = ours)
            }
        }
        main.removeCallbacks(fadeStep)
        main.post(fadeStep)
    }

    private fun applyLevels() {
        val g = gain()
        runCatching { player?.setVolume(g * playerLevel, g * playerLevel) }
        for (f in fading) runCatching { f.player.setVolume(g * f.level, g * f.level) }
    }

    /** Lets a player go; the library track's place is kept, so it goes on from there next time. */
    private fun release(p: MediaPlayer, track: Int?) {
        if (track == null) runCatching { libraryPositionMs = p.currentPosition }
        runCatching { if (p.isPlaying) p.stop() }
        runCatching { p.release() }
    }

    private fun releaseFading() {
        for (f in fading) release(f.player, f.track)
        fading.clear()
    }

    /** Ends the fades at once, where they were going: the old tracks gone, the new at full. */
    private fun settleFade() {
        main.removeCallbacks(fadeStep)
        main.removeCallbacks(pendingStart)
        releaseFading()
        playerLevel = 1f
        inFrom = 1f
        applyLevels()
    }

    /** Stop and release. Called when a game boots and when the toggle goes off. */
    fun stop(context: Context) {
        pausedForFocus = false
        main.removeCallbacks(fadeStep)
        main.removeCallbacks(pendingStart)
        releaseFading()
        playerLevel = 1f
        inFrom = 1f
        player?.let { p ->
            runCatching { if (p.isPlaying) p.stop() }
            runCatching { p.release() }
        }
        player = null
        audioManager(context)?.let { abandonFocus(it) }
    }

    /** Suspend without releasing — app backgrounded. */
    fun pause() {
        settleFade()
        runCatching { player?.takeIf { it.isPlaying }?.pause() }
    }

    fun resume() {
        if (!enabled.value) return
        if (MainActivityRuntime.eState.value != EmuState.STOPPED) return
        runCatching { player?.takeIf { !it.isPlaying }?.start() }
    }

    // ---- audio focus ----

    private val focusListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_LOSS -> {
                // Permanent: another app took over for good. Drop the player entirely
                // rather than sitting paused forever holding a decoder.
                pausedForFocus = false
                main.removeCallbacks(fadeStep)
                main.removeCallbacks(pendingStart)
                releaseFading()
                playerLevel = 1f
                inFrom = 1f
                player?.let { p ->
                    runCatching { if (p.isPlaying) p.stop() }
                    runCatching { p.release() }
                }
                player = null
            }
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                // Arm self-resume on GAIN whenever we own an enabled player — not only when
                // it's live right now. onPause() may have paused it just before this fires, and
                // keying on isPlaying then left the flag false so GAIN never resumed the duck.
                if (player != null && enabled.value) { pausedForFocus = true; pause() }
            }
            AudioManager.AUDIOFOCUS_GAIN -> {
                if (pausedForFocus) { pausedForFocus = false; resume() }
            }
        }
    }

    private fun requestFocus(am: AudioManager): Boolean {
        val granted = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val req = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build()
                )
                .setOnAudioFocusChangeListener(focusListener)
                .build()
            focusRequest = req
            am.requestAudioFocus(req)
        } else {
            @Suppress("DEPRECATION")
            am.requestAudioFocus(focusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN)
        }
        return granted == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
    }

    private fun abandonFocus(am: AudioManager) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            focusRequest?.let { runCatching { am.abandonAudioFocusRequest(it) } }
            focusRequest = null
        } else {
            @Suppress("DEPRECATION")
            runCatching { am.abandonAudioFocus(focusListener) }
        }
    }
}
