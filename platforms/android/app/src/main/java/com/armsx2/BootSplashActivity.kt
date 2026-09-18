package com.armsx2

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.View
import android.widget.VideoView
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

/**
 * Boot splash: plays the intro video once per process, then hands off to Main. That is
 * the user's own intro when one has been chosen ([BootIntro]), else the bundled
 * res/raw/boot_intro.mp4. Tapping, the Back button, a hard timeout, and any playback error
 * all fall through to the app so a bad codec or slow decode never strands the user on a
 * black screen. The splash is opt-out via the "ui.bootLogo" preference (App settings,
 * default on) — when disabled it launches Main immediately.
 */
class BootSplashActivity : ComponentActivity() {
    private var launchedMain = false
    private var rootView: View? = null
    private val timeoutRunnable = Runnable { launchMainAndFinish() }
    // True while a user-chosen intro is the one playing. Cleared if it fails and we fall back.
    private var playingCustom = false

    override fun onCreate(savedInstanceState: Bundle?) {
        // The manifest theme (Theme.ARMSX2.Boot) already paints the window black,
        // matching the video's black FrameLayout — no per-theme override, so a
        // light-mode device never flashes white before the first decoded frame.
        super.onCreate(savedInstanceState)
        applyImmersiveUi()

        val prefs = getSharedPreferences("ARMSX2", MODE_PRIVATE)
        val bootLogoEnabled = prefs.getBoolean("ui.bootLogo", true)
        if (!bootLogoEnabled || playedThisProcess) {
            launchMainAndFinish()
            return
        }
        playedThisProcess = true

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = launchMainAndFinish()
        })

        setContentView(R.layout.activity_boot_splash)
        rootView = findViewById(R.id.boot_splash_root)
        val videoView = findViewById<VideoView?>(R.id.boot_splash_video)
        rootView?.apply {
            setOnClickListener { launchMainAndFinish() }
            postDelayed(timeoutRunnable, HARD_TIMEOUT_MS)
        }
        if (videoView != null) {
            val bundled = Uri.parse("android.resource://$packageName/${R.raw.boot_intro}")
            videoView.setOnClickListener { launchMainAndFinish() }
            // The user's own intro when one is on disk. Checked by file, not by preference:
            // Main has not initialised its prefs yet, and the file is the ground truth anyway.
            val custom = BootIntro.customFile(this).takeIf { it.length() > 0L }
            if (custom != null) {
                playingCustom = true
                videoView.setVideoPath(custom.absolutePath)
            } else {
                videoView.setVideoURI(bundled)
            }
            videoView.setOnPreparedListener { mp ->
                mp.isLooping = false
                if (playingCustom) {
                    // A chosen intro may run longer than the bundled one, and the hard timeout
                    // would cut it off mid-play. That timeout exists to catch a decode that never
                    // starts; once prepared, playback is proven, so give the video its own length
                    // (capped, in case a file misreports it) before the backstop fires. The
                    // bundled path is left exactly as it was.
                    rootView?.removeCallbacks(timeoutRunnable)
                    val runFor = (mp.duration.toLong().coerceAtLeast(0L) + 2_000L)
                        .coerceIn(HARD_TIMEOUT_MS, CUSTOM_MAX_MS)
                    rootView?.postDelayed(timeoutRunnable, runFor)
                }
                videoView.start()
            }
            videoView.setOnCompletionListener { launchMainAndFinish() }
            videoView.setOnErrorListener { _, _, _ ->
                if (playingCustom) {
                    // A chosen file the device cannot decode falls back to the stock intro
                    // rather than skipping, so a bad pick is visible as "not my video" instead
                    // of the intro silently vanishing. With a fresh hard timeout, since the
                    // failed attempt may already have spent part of it.
                    playingCustom = false
                    rootView?.removeCallbacks(timeoutRunnable)
                    rootView?.postDelayed(timeoutRunnable, HARD_TIMEOUT_MS)
                    videoView.setVideoURI(bundled)
                } else {
                    launchMainAndFinish()
                }
                true
            }
        } else {
            launchMainAndFinish()
        }
    }

    override fun onDestroy() {
        rootView?.removeCallbacks(timeoutRunnable)
        super.onDestroy()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) applyImmersiveUi()
    }

    private fun applyImmersiveUi() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    private fun launchMainAndFinish() {
        if (launchedMain) return
        launchedMain = true
        rootView?.removeCallbacks(timeoutRunnable)
        val launch = Intent(this, Main::class.java)
        intent?.let { source ->
            launch.action = source.action
            if (source.data != null || source.type != null) launch.setDataAndType(source.data, source.type)
            source.categories?.forEach(launch::addCategory)
            source.extras?.let(launch::putExtras)
            source.clipData?.let(launch::setClipData)
            launch.addFlags(
                source.flags and (
                    Intent.FLAG_GRANT_READ_URI_PERMISSION or
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                        Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or
                        Intent.FLAG_GRANT_PREFIX_URI_PERMISSION
                    ),
            )
        }
        launch.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        startActivity(launch)
        finish()
        // overrideActivityTransition is API 34 (Android 14); on 13 and below it
        // throws NoSuchMethodError (crashed the splash on the Retroid). Fall back to
        // the deprecated overridePendingTransition there.
        if (android.os.Build.VERSION.SDK_INT >= 34) {
            overrideActivityTransition(OVERRIDE_TRANSITION_CLOSE, 0, 0)
        } else {
            @Suppress("DEPRECATION")
            overridePendingTransition(0, 0)
        }
    }

    private companion object {
        var playedThisProcess = false
        const val HARD_TIMEOUT_MS = 6000L
        // Longest a custom intro may hold the app. Tap and Back always skip sooner.
        const val CUSTOM_MAX_MS = 60_000L
    }
}
