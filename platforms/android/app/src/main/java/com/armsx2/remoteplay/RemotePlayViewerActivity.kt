// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.addCallback
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.changedToDownIgnoreConsumed
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.ui.touch.RemotePlayTouchControls
import com.armsx2.ui.touch.clearRemotePlayPadSink
import com.armsx2.ui.touch.setRemotePlayPadSink
import org.json.JSONObject
import org.webrtc.ArmsxSurfaceDecoder
import org.webrtc.ArmsxVideoOutput
import org.webrtc.DataChannel
import org.webrtc.MediaConstraints
import org.webrtc.PeerConnection
import org.webrtc.RendererCommon
import org.webrtc.RtpTransceiver
import org.webrtc.SessionDescription
import org.webrtc.SurfaceViewRenderer
import org.webrtc.VideoSink
import org.webrtc.VideoTrack
import java.util.concurrent.LinkedBlockingDeque
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.roundToInt

private const val TAG = "RemotePlay"

/** On the same Wi-Fi the host answers at once; give up after this. */
private const val WIFI_CONNECT_TIMEOUT_MS = 30_000L

/** Online, the friend still has to receive the reply and paste it. */
private const val INVITE_CONNECT_TIMEOUT_MS = 5 * 60_000L

/** With a lobby code the host collects the answer within a couple of seconds. */
private const val LOBBY_CONNECT_TIMEOUT_MS = 60_000L

/** Tap to reveal: how long the exit button stays, and how long the touch controls stay untouched. */
private const val EXIT_SHOWN_MS = 4_000L
private const val TOUCH_HIDE_MS = 15_000L

/**
 * Stick and trigger movement goes to the host at most this often (125 times a second); a button
 * press or release goes at once. Input reaches the app the moment the controller reports it, which
 * for some built-in controllers is hundreds of times a second while a stick moves, and every
 * packet sent takes its turn on the Wi-Fi channel the video needs.
 */
private const val ANALOG_SEND_MS = 8L

/** How a viewer reaches its host. */
internal sealed interface ViewerMode {
    data class Wifi(val host: String, val port: Int) : ViewerMode

    /** From a pasted invite; [offer] is the decoded invite. */
    data class Invite(val offer: JSONObject) : ViewerMode

    /** From a six-character code through the lobby service. */
    data class Lobby(val code: String) : ViewerMode
}

/**
 * The viewer: a friend's game on this screen, played as player 2 with this device's controller.
 * Nothing is emulated here, so it needs no BIOS, firmware or game, only a way to reach the host:
 * the same Wi-Fi, or an invite code.
 */
class RemotePlayViewerActivity : ComponentActivity() {
    companion object {
        private const val EXTRA_HOST = "host"
        private const val EXTRA_PORT = "port"
        private const val EXTRA_NAME = "name"
        private const val EXTRA_OFFER = "offer"
        private const val EXTRA_LOBBY = "lobby"

        /** Join a host on this Wi-Fi. */
        fun start(context: Context, host: String, port: Int, name: String) {
            launch(
                context,
                Intent(context, RemotePlayViewerActivity::class.java)
                    .putExtra(EXTRA_HOST, host)
                    .putExtra(EXTRA_PORT, port)
                    .putExtra(EXTRA_NAME, name),
            )
        }

        /** Join from a pasted invite. [offer] is the decoded invite (RemotePlayProtocol.decodeCode). */
        fun startWithInvite(context: Context, offer: JSONObject) {
            launch(context, Intent(context, RemotePlayViewerActivity::class.java).putExtra(EXTRA_OFFER, offer.toString()))
        }

        /** Join with a six-character code, already normalized (RemotePlayLobby.normalize). */
        fun startWithLobby(context: Context, code: String) {
            launch(context, Intent(context, RemotePlayViewerActivity::class.java).putExtra(EXTRA_LOBBY, code))
        }

        private fun launch(context: Context, intent: Intent) {
            if (context !is android.app.Activity) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(intent)
        }
    }

    // The picture: decoded straight onto a plain view where the device allows (ArmsxSurfaceDecoder),
    // otherwise drawn by WebRTC's renderer, which also takes over if that fails mid-session.
    private var output: ArmsxVideoOutput? = null
    private var renderer: SurfaceViewRenderer? = null
    private val videoView = mutableStateOf<View?>(null)
    private var session: ViewerSession? = null
    private val main = Handler(Looper.getMainLooper())

    // Shown over the black screen until the first frame arrives.
    private val status = mutableStateOf("")
    private val playing = mutableStateOf(false)
    private val hintVisible = mutableStateOf(false)
    private val leaveAsked = mutableStateOf(false)

    // Said over the picture, so a stalled or still picture is not a mystery: the connection is
    // trying to recover, or the host paused the game.
    private val interrupted = mutableStateOf(false)
    private val hostPaused = mutableStateOf(false)

    // Online: the reply code the player sends back, once it is ready.
    private val inviteMode = mutableStateOf(false)
    private val reply = mutableStateOf<String?>(null)
    private val connected = mutableStateOf(false)

    // The picture's own shape, so it is fitted to this screen instead of cropped to fill it.
    private val videoAspect = mutableStateOf<Float?>(null)

    // The exit button, touch controls and stream stats, as chosen on the Remote play screen.
    private var exitMode = RevealMode.TAP
    private var touchMode = RevealMode.TAP
    private var statsOn = false
    private val exitShown = mutableStateOf(false)
    private val touchShown = mutableStateOf(false)
    private val statsLine = mutableStateOf("")

    // Same Wi-Fi with QoS marking asked for: the stats say so, so a screenshot shows which mode ran.
    private var qosMarked = false
    private val hostLine = mutableStateOf("")

    private val input = ViewerInput { pad -> session?.sendPad(pad) }

    // The on-screen controls' presses, merged into player 2's pad.
    private val touchPad = TouchPadState { pad -> input.setTouch(pad) }
    private val padSink: (Int, Int, Boolean) -> Unit = { keycode, value, pressed -> touchPad.onKey(keycode, value, pressed) }

    // Fingers on the screen now. Tap to reveal never hides the controls under a held finger.
    private var fingersDown = 0

    // The controller last used here, where the host's rumble for player 2 goes; -1 before any.
    private var lastPadDevice = -1

    private val hideExit = Runnable { exitShown.value = false }
    private val hideTouch = object : Runnable {
        override fun run() {
            if (touchMode != RevealMode.TAP) return
            if (fingersDown > 0) {
                main.postDelayed(this, TOUCH_HIDE_MS)
            } else {
                hideTouchPad()
            }
        }
    }

    // Only while the stats are switched on: they refresh every second, and nothing is sampled
    // otherwise.
    private val statsTick = object : Runnable {
        override fun run() {
            if (!statsOn || isDestroyed) return
            session?.let { current ->
                // The decoder's own worst of the last second: its slowest frame, and the longest the
                // screen waited for the next one. Only when it decodes onto the screen itself.
                val peaks = if (output != null && renderer == null) ArmsxSurfaceDecoder.takePeaks() else null
                val decoder = peaks?.let { " · decode max ${it[0]} ms · gap max ${it[1]} ms" }.orEmpty()
                // The screen's refresh rate now: 60 Hz unless the screen could not switch to it.
                // And this device's Wi-Fi link, the usual suspect behind stalls and lost packets.
                val screen = window.decorView.display?.refreshRate?.let { " · screen ${it.roundToInt()} Hz" }.orEmpty() +
                    WifiLink.read(this@RemotePlayViewerActivity)?.let { " · ${it.describe()}" }.orEmpty() +
                    (if (qosMarked) " · QoS" else "")
                current.sampleStats { line ->
                    val audio = current.audioKind?.let { " · audio $it" }.orEmpty()
                    main.post { statsLine.value = line + decoder + screen + audio }
                }
                current.hostStats?.let { hostLine.value = describeHost(it) }
            }
            main.postDelayed(this, 1_000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Recreated after the app's process was ended: that session is gone, and the app's settings
        // this screen reads are only loaded by the main screen.
        if (savedInstanceState != null) {
            finish()
            return
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        preferSixtyHz()
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        val offer = intent.getStringExtra(EXTRA_OFFER)?.let { runCatching { JSONObject(it) }.getOrNull() }
        val lobby = intent.getStringExtra(EXTRA_LOBBY)?.let(RemotePlayLobby::normalize)
        val host = intent.getStringExtra(EXTRA_HOST)
        val mode = when {
            lobby != null -> ViewerMode.Lobby(lobby)
            offer != null -> ViewerMode.Invite(offer)
            host != null -> ViewerMode.Wifi(host, intent.getIntExtra(EXTRA_PORT, RemotePlayProtocol.PORT))
            else -> {
                finish()
                return
            }
        }
        inviteMode.value = mode is ViewerMode.Invite
        qosMarked = mode is ViewerMode.Wifi && RemotePlayPrefs.wifiQos(this)

        exitMode = RemotePlayPrefs.exitButton(this)
        touchMode = RemotePlayPrefs.touchControls(this)
        statsOn = RemotePlayPrefs.streamStats(this)
        exitShown.value = exitMode == RevealMode.ON
        touchShown.value = touchMode == RevealMode.ON
        // The touch controls press player 2's buttons for as long as this screen is open.
        setRemotePlayPadSink(padSink)

        // The Back key and the back gesture both ask before leaving; asked again, they cancel.
        onBackPressedDispatcher.addCallback(this) { askToLeave() }

        if (ArmsxVideoOutput.available()) {
            val target = ArmsxVideoOutput(object : ArmsxVideoOutput.Listener {
                override fun onFirstFrame() {
                    main.post { firstFrame() }
                }

                override fun onVideoSize(width: Int, height: Int) {
                    main.post { videoAspect.value = width.toFloat() / height }
                }

                override fun onFallback() {
                    main.post { useRenderer() }
                }
            })
            output = target
            videoView.value = SurfaceView(this).also { view ->
                view.holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) {
                        screenReady(holder)
                        target.setSurface(holder.surface)
                    }

                    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                        screenReady(holder)
                        target.setSurface(holder.surface)
                    }

                    override fun surfaceDestroyed(holder: SurfaceHolder) = target.setSurface(null)
                })
            }
        } else {
            useRenderer()
        }

        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                Box(
                    Modifier
                        .fillMaxSize()
                        .background(Color.Black)
                        .pointerInput(Unit) {
                            // Watches every touch after the controls and the exit area have had it,
                            // and takes none. A tap nobody claimed brings the touch controls up.
                            awaitPointerEventScope {
                                while (true) {
                                    val event = awaitPointerEvent()
                                    fingersDown = event.changes.count { it.pressed }
                                    val downs = event.changes.filter { it.changedToDownIgnoreConsumed() }
                                    if (downs.isNotEmpty()) onScreenTouched(unclaimed = downs.any { !it.isConsumed })
                                }
                            }
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    val aspect = videoAspect.value
                    videoView.value?.let { view ->
                        // A view of its own when the renderer takes over from the plain one.
                        key(view) {
                            AndroidView(
                                factory = { view },
                                modifier = if (aspect != null) Modifier.aspectRatio(aspect) else Modifier.fillMaxSize(),
                            )
                        }
                    }

                    if (!playing.value) {
                        if (inviteMode.value && !connected.value) {
                            ReplyPanel()
                        } else if (status.value.isNotEmpty()) {
                            Text(
                                status.value,
                                color = Color.White,
                                textAlign = TextAlign.Center,
                                modifier = Modifier.padding(24.dp),
                            )
                        }
                    }

                    if (playing.value && touchShown.value && !leaveAsked.value) {
                        RemotePlayTouchControls()
                    }

                    // Tap to reveal: the top middle of the screen is the exit button's, and only a
                    // tap there brings it up. Above the touch controls, so it is never a button.
                    if (exitMode == RevealMode.TAP && !leaveAsked.value) {
                        ExitArea()
                    }

                    if (exitShown.value && !leaveAsked.value) {
                        ExitButton()
                    }

                    if (statsOn && playing.value && statsLine.value.isNotEmpty() && !leaveAsked.value) {
                        Text(
                            if (hostLine.value.isEmpty()) statsLine.value else statsLine.value + "\n" + hostLine.value,
                            color = Color.White.copy(alpha = 0.85f),
                            style = MaterialTheme.typography.labelSmall,
                            modifier = Modifier
                                .align(Alignment.BottomStart)
                                .padding(8.dp)
                                .background(Color.Black.copy(alpha = 0.55f), RoundedCornerShape(6.dp))
                                .padding(horizontal = 8.dp, vertical = 4.dp),
                        )
                    }

                    if (playing.value && hintVisible.value && !leaveAsked.value && !touchShown.value) {
                        val hint = buildList {
                            add(str("remoteplay.viewer.hint"))
                            if (exitMode == RevealMode.TAP) add(str("remoteplay.viewer.hintExit"))
                            if (touchMode == RevealMode.TAP) {
                                add(str(if (exitMode == RevealMode.TAP) "remoteplay.viewer.hintTouchElsewhere" else "remoteplay.viewer.hintTouch"))
                            }
                        }.joinToString(" ")
                        Text(
                            hint,
                            color = Color.White,
                            textAlign = TextAlign.Center,
                            modifier = Modifier
                                .align(Alignment.BottomCenter)
                                .padding(16.dp)
                                .background(Color.Black.copy(alpha = 0.6f), RoundedCornerShape(10.dp))
                                .padding(horizontal = 14.dp, vertical = 8.dp),
                        )
                    }

                    val notice = when {
                        interrupted.value -> str("remoteplay.viewer.interrupted")
                        hostPaused.value -> str("remoteplay.viewer.hostPaused")
                        else -> null
                    }
                    if (playing.value && notice != null && !leaveAsked.value) {
                        Text(
                            notice,
                            color = Color.White,
                            textAlign = TextAlign.Center,
                            modifier = Modifier
                                .align(Alignment.Center)
                                .padding(24.dp)
                                .background(Color.Black.copy(alpha = 0.7f), RoundedCornerShape(12.dp))
                                .padding(horizontal = 18.dp, vertical = 10.dp),
                        )
                    }

                    if (leaveAsked.value) {
                        LeavePrompt()
                    }
                }
            }
        }

        // The pad and the touch controls reach this screen as soon as they move, not saved up for
        // the next refresh as Android does by default: up to a frame sooner (as Moonlight does).
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.decorView.post {
                window.decorView.requestUnbufferedDispatch(
                    InputDevice.SOURCE_CLASS_JOYSTICK or InputDevice.SOURCE_CLASS_BUTTON or InputDevice.SOURCE_CLASS_POINTER,
                )
            }
        }

        status.value = when (mode) {
            is ViewerMode.Wifi -> I18n.get("remoteplay.viewer.connecting").format(intent.getStringExtra(EXTRA_NAME).orEmpty().ifBlank { mode.host })
            is ViewerMode.Lobby -> I18n.get("remoteplay.viewer.joining").format(RemotePlayLobby.display(mode.code))
            is ViewerMode.Invite -> ""
        }
        session = ViewerSession(
            context = this,
            mode = mode,
            output = output,
            rumbleDevice = { lastPadDevice },
            onInterrupted = { interrupted.value = it },
            onHostPaused = { hostPaused.value = it },
            onReply = { code -> reply.value = code },
            onConnected = {
                connected.value = true
                status.value = I18n.get("remoteplay.viewer.waitingVideo")
            },
            onWaitingForHost = {
                if (!playing.value) status.value = I18n.get("remoteplay.viewer.waitingAccept")
            },
            onEnded = { message ->
                if (message != null) Toast.makeText(this, message, Toast.LENGTH_LONG).show()
                finish()
            },
        ).also { created ->
            renderer?.let(created::showOn)
            created.start()
        }
    }

    private fun firstFrame() {
        if (playing.value || isDestroyed) return
        playing.value = true
        hintVisible.value = true
        main.postDelayed({ hintVisible.value = false }, 6_000)
        touchActivity()
        if (statsOn) main.post(statsTick)
    }

    /**
     * WebRTC's renderer draws the frames: from the start when decoding onto the screen has failed
     * on this device before, or from now on when it just did.
     */
    private fun useRenderer() {
        if (renderer != null || isDestroyed) return
        val view = SurfaceViewRenderer(this).apply {
            init(RemotePlayRtc.eglBase.eglBaseContext, object : RendererCommon.RendererEvents {
                override fun onFirstFrameRendered() {
                    main.post { firstFrame() }
                }

                override fun onFrameResolutionChanged(width: Int, height: Int, rotation: Int) {
                    if (width <= 0 || height <= 0) return
                    val aspect = if (rotation % 180 == 0) width.toFloat() / height else height.toFloat() / width
                    main.post { videoAspect.value = aspect }
                }
            })
            setScalingType(RendererCommon.ScalingType.SCALE_ASPECT_FIT)
            setEnableHardwareScaler(true)
        }
        view.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) = screenReady(holder)

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) =
                screenReady(holder)

            override fun surfaceDestroyed(holder: SurfaceHolder) = Unit
        })
        renderer = view
        videoView.value = view
        session?.showOn(view)
    }

    /**
     * The stream runs at 60 fps, so while it plays the screen does too, as Moonlight does: each
     * frame then stays up for exactly one refresh. On a 90 or 120 Hz screen, frames shown as they
     * arrive stay up for one refresh or two (or three), which reads as stutter, and evening that
     * out by holding frames back cost both delay and frames (test 12 and 13: 45 to 48 fps on a 120
     * Hz screen). Takes the screen's own 60 Hz mode at its current resolution; a screen without
     * one stays as it is. Leaving this screen gives the rest of the app its usual rate back.
     */
    private fun preferSixtyHz() {
        val screen = (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) this.display else null)
            ?: @Suppress("DEPRECATION") windowManager.defaultDisplay
            ?: return
        val current = screen.mode
        val sixty = screen.supportedModes
            .filter { it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight }
            .filter { abs(it.refreshRate - 60f) < 1f }
            .minByOrNull { abs(it.refreshRate - 60f) }
            ?: return
        if (sixty.modeId != current.modeId) {
            window.attributes = window.attributes.apply { preferredDisplayModeId = sixty.modeId }
        }
        Log.i(TAG, "remote play: screen ${current.refreshRate.roundToInt()} Hz, asked for mode ${sixty.modeId} (${sixty.refreshRate.roundToInt()} Hz)")
    }

    /** The stream's surface is there: tells the system its content runs at 60 fps. */
    private fun screenReady(holder: SurfaceHolder) {
        val surface = holder.surface
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && surface.isValid) {
            runCatching {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    surface.setFrameRate(60f, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, Surface.CHANGE_FRAME_RATE_ALWAYS)
                } else {
                    surface.setFrameRate(60f, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
                }
            }
        }
    }

    /** [unclaimed]: the tap landed on nothing, neither a control nor the exit area. */
    private fun onScreenTouched(unclaimed: Boolean) {
        if (unclaimed && touchMode == RevealMode.TAP && playing.value) touchShown.value = true
        touchActivity()
    }

    private fun revealExit() {
        exitShown.value = true
        main.removeCallbacks(hideExit)
        main.postDelayed(hideExit, EXIT_SHOWN_MS)
    }

    // Tap to reveal: the controls go away again after a while untouched.
    private fun touchActivity() {
        main.removeCallbacks(hideTouch)
        if (touchMode == RevealMode.TAP) main.postDelayed(hideTouch, TOUCH_HIDE_MS)
    }

    private fun hideTouchPad() {
        touchShown.value = false
        touchPad.clear()
    }

    private fun askToLeave() {
        if (leaveAsked.value) {
            leaveAsked.value = false
            return
        }
        input.releaseAll()
        leaveAsked.value = true
    }

    private fun shareReply() {
        reply.value?.let { shareText(this, I18n.get("remoteplay.reply.share").format(it)) }
    }

    private fun copyReply() {
        reply.value?.let { copyText(this, it) }
    }

    /** The top middle of the screen, invisible: a tap here shows the exit button. */
    @Composable
    private fun BoxScope.ExitArea() {
        Box(
            Modifier
                .align(Alignment.TopCenter)
                .size(width = 260.dp, height = 96.dp)
                .pointerInput(Unit) {
                    awaitPointerEventScope {
                        while (true) {
                            val event = awaitPointerEvent()
                            val downs = event.changes.filter { it.changedToDownIgnoreConsumed() }
                            if (downs.isEmpty()) continue
                            downs.forEach { it.consume() }
                            revealExit()
                        }
                    }
                },
        )
    }

    @Composable
    private fun BoxScope.ExitButton() {
        Text(
            "✕  " + str("remoteplay.viewer.leave"),
            color = Color.White,
            modifier = Modifier
                .align(Alignment.TopCenter)
                .padding(top = 10.dp)
                .clip(RoundedCornerShape(50))
                .background(Color.Black.copy(alpha = 0.6f))
                .clickable { askToLeave() }
                .padding(horizontal = 18.dp, vertical = 9.dp),
        )
    }

    @Composable
    private fun BoxScope.ReplyPanel() {
        Column(
            Modifier
                .align(Alignment.Center)
                .widthIn(max = 520.dp)
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            val code = reply.value
            if (code == null) {
                Text(str("remoteplay.viewer.preparing"), color = Color.White)
            } else {
                Text(str("remoteplay.viewer.sendReply"), color = Color.White, textAlign = TextAlign.Center)
                Text(
                    code.take(32) + "...",
                    color = Color.White.copy(alpha = 0.6f),
                    fontFamily = FontFamily.Monospace,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Button(onClick = { shareReply() }) { Text(str("remoteplay.code.share") + " (A)") }
                    OutlinedButton(onClick = { copyReply() }) { Text(str("remoteplay.code.copy") + " (Y)") }
                }
                Text(
                    str("remoteplay.viewer.waitingHost"),
                    color = Color.White.copy(alpha = 0.7f),
                    textAlign = TextAlign.Center,
                )
            }
        }
    }

    @Composable
    private fun BoxScope.LeavePrompt() {
        Column(
            Modifier
                .align(Alignment.Center)
                .background(Color.Black.copy(alpha = 0.85f), RoundedCornerShape(16.dp))
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(str("remoteplay.viewer.leaveQuestion"), color = Color.White)
            Text(str("remoteplay.viewer.leaveButtons"), color = Color.White.copy(alpha = 0.7f))
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedButton(onClick = { leaveAsked.value = false }) { Text(str("remoteplay.viewer.stay")) }
                Button(onClick = { finish() }) { Text(str("remoteplay.viewer.leave")) }
            }
        }
    }

    // Rumble only while this screen is in front: in the background nobody is holding the pad.
    override fun onStart() {
        super.onStart()
        session?.setForeground(true)
    }

    override fun onStop() {
        session?.setForeground(false)
        super.onStop()
    }

    // Focus gone (the notification shade, a call, Home): whatever is held now would stay held on the
    // host, since its release goes to another window. Let go of everything.
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus) {
            input.releaseAll()
            touchPad.clear()
        }
    }

    override fun onDestroy() {
        // The session first: while it ends, the decoder can still post to this screen, and those
        // posts go with the rest below.
        session?.end(null)
        session = null
        main.removeCallbacksAndMessages(null)
        clearRemotePlayPadSink(padSink)
        renderer?.release()
        super.onDestroy()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val fromPad = event.isFromSource(InputDevice.SOURCE_GAMEPAD) ||
            event.isFromSource(InputDevice.SOURCE_JOYSTICK) ||
            event.isFromSource(InputDevice.SOURCE_DPAD)
        if (fromPad && event.deviceId >= 0) lastPadDevice = event.deviceId

        if (leaveAsked.value) {
            if (event.action == KeyEvent.ACTION_UP) {
                when (event.keyCode) {
                    KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> finish()
                    KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_BUTTON_MODE -> leaveAsked.value = false
                }
            }
            return true
        }

        // Home/PS on the pad asks to leave; it never reaches the host, where player 2's PS button
        // would open the host's own home menu. Back goes to onBackPressedDispatcher, which asks too.
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_MODE) {
            if (event.action == KeyEvent.ACTION_UP) askToLeave()
            return true
        }

        // A real controller in use: a revealed touch pad is in the way.
        if (fromPad && event.action == KeyEvent.ACTION_DOWN && touchMode == RevealMode.TAP && touchShown.value) {
            hideTouchPad()
        }

        // While the reply waits to be sent, the pad works the panel: A shares, Y copies.
        if (inviteMode.value && !connected.value && reply.value != null && fromPad) {
            if (event.action == KeyEvent.ACTION_UP) {
                when (event.keyCode) {
                    KeyEvent.KEYCODE_BUTTON_A -> shareReply()
                    KeyEvent.KEYCODE_BUTTON_Y -> copyReply()
                }
            }
            return true
        }

        if (fromPad && input.onKey(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.isFromSource(InputDevice.SOURCE_JOYSTICK) && event.deviceId >= 0) lastPadDevice = event.deviceId
        if (!leaveAsked.value && input.onMotion(event)) return true
        return super.dispatchGenericMotionEvent(event)
    }
}

/**
 * This device's controller as player 2's pad. Face buttons are positional: the bottom button
 * (Android's A) is Cross, as on a DualShock.
 */
internal class ViewerInput(private val send: (RemotePlayProtocol.PadState) -> Unit) {
    private var keyButtons = 0
    private var hatButtons = 0
    private var lx = 128
    private var ly = 128
    private var rx = 128
    private var ry = 128
    private var l2Axis = 0
    private var r2Axis = 0
    private var l2Key = false
    private var r2Key = false
    private var last = RemotePlayProtocol.PadState()

    // The on-screen pad's own state, neutral while it is hidden. Merged with the controller's.
    private var touch = RemotePlayProtocol.PadState()

    fun setTouch(state: RemotePlayProtocol.PadState) {
        touch = state
        publish()
    }

    fun onKey(event: KeyEvent): Boolean {
        val down = event.action == KeyEvent.ACTION_DOWN
        if (event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP) return false
        when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_L2 -> l2Key = down
            KeyEvent.KEYCODE_BUTTON_R2 -> r2Key = down
            else -> {
                val bit = when (event.keyCode) {
                    KeyEvent.KEYCODE_BUTTON_A -> RemotePlayProtocol.PAD_CROSS
                    KeyEvent.KEYCODE_BUTTON_B -> RemotePlayProtocol.PAD_CIRCLE
                    KeyEvent.KEYCODE_BUTTON_X -> RemotePlayProtocol.PAD_SQUARE
                    KeyEvent.KEYCODE_BUTTON_Y -> RemotePlayProtocol.PAD_TRIANGLE
                    KeyEvent.KEYCODE_BUTTON_L1 -> RemotePlayProtocol.PAD_L1
                    KeyEvent.KEYCODE_BUTTON_R1 -> RemotePlayProtocol.PAD_R1
                    KeyEvent.KEYCODE_BUTTON_THUMBL -> RemotePlayProtocol.PAD_L3
                    KeyEvent.KEYCODE_BUTTON_THUMBR -> RemotePlayProtocol.PAD_R3
                    KeyEvent.KEYCODE_BUTTON_START -> RemotePlayProtocol.PAD_START
                    KeyEvent.KEYCODE_BUTTON_SELECT -> RemotePlayProtocol.PAD_SELECT
                    KeyEvent.KEYCODE_DPAD_UP -> RemotePlayProtocol.PAD_UP
                    KeyEvent.KEYCODE_DPAD_DOWN -> RemotePlayProtocol.PAD_DOWN
                    KeyEvent.KEYCODE_DPAD_LEFT -> RemotePlayProtocol.PAD_LEFT
                    KeyEvent.KEYCODE_DPAD_RIGHT -> RemotePlayProtocol.PAD_RIGHT
                    else -> return false
                }
                keyButtons = if (down) keyButtons or bit else keyButtons and bit.inv()
            }
        }
        publish()
        return true
    }

    fun onMotion(event: MotionEvent): Boolean {
        if (!event.isFromSource(InputDevice.SOURCE_JOYSTICK) && !event.isFromSource(InputDevice.SOURCE_GAMEPAD)) return false
        // The system cancels a held stick when the window loses it: let go, as for a release.
        if (event.action == MotionEvent.ACTION_CANCEL) {
            lx = 128; ly = 128; rx = 128; ry = 128
            l2Axis = 0; r2Axis = 0
            hatButtons = 0
            publish()
            return true
        }
        if (event.action != MotionEvent.ACTION_MOVE) return false

        val device = event.device
        fun axis(id: Int) = event.getAxisValue(id)
        fun has(id: Int) = device?.getMotionRange(id, event.source) != null

        val (x, y) = shape(axis(MotionEvent.AXIS_X), axis(MotionEvent.AXIS_Y))
        // Most pads put the right stick on Z/RZ; some use RX/RY.
        val rightOnZ = has(MotionEvent.AXIS_Z) && has(MotionEvent.AXIS_RZ)
        val (zx, zy) = shape(
            axis(if (rightOnZ) MotionEvent.AXIS_Z else MotionEvent.AXIS_RX),
            axis(if (rightOnZ) MotionEvent.AXIS_RZ else MotionEvent.AXIS_RY),
        )
        lx = RemotePlayProtocol.axisByte(x)
        ly = RemotePlayProtocol.axisByte(y)
        rx = RemotePlayProtocol.axisByte(zx)
        ry = RemotePlayProtocol.axisByte(zy)

        l2Axis = (max(axis(MotionEvent.AXIS_LTRIGGER), axis(MotionEvent.AXIS_BRAKE)) * 255f).roundToInt().coerceIn(0, 255)
        r2Axis = (max(axis(MotionEvent.AXIS_RTRIGGER), axis(MotionEvent.AXIS_GAS)) * 255f).roundToInt().coerceIn(0, 255)

        val hatX = axis(MotionEvent.AXIS_HAT_X)
        val hatY = axis(MotionEvent.AXIS_HAT_Y)
        hatButtons = (if (hatX < -0.5f) RemotePlayProtocol.PAD_LEFT else 0) or
            (if (hatX > 0.5f) RemotePlayProtocol.PAD_RIGHT else 0) or
            (if (hatY < -0.5f) RemotePlayProtocol.PAD_UP else 0) or
            (if (hatY > 0.5f) RemotePlayProtocol.PAD_DOWN else 0)

        publish()
        return true
    }

    /** Drops everything held, so leaving or a prompt never strands player 2 mid-press. */
    fun releaseAll() {
        keyButtons = 0
        hatButtons = 0
        lx = 128; ly = 128; rx = 128; ry = 128
        l2Axis = 0; r2Axis = 0
        l2Key = false; r2Key = false
        publish()
    }

    /**
     * Radial deadzone, then expanded to the square like the host's own sticks (Square gate, on by
     * default): a full diagonal reaches the end of both axes, as a DualShock's does.
     */
    private fun shape(x: Float, y: Float): Pair<Float, Float> {
        val magnitude = hypot(x, y)
        if (magnitude < DEADZONE) return 0f to 0f
        val out = ((magnitude - DEADZONE) / (1f - DEADZONE)).coerceAtMost(1f)
        val scale = out / max(abs(x), abs(y))
        return (x * scale).coerceIn(-1f, 1f) to (y * scale).coerceIn(-1f, 1f)
    }

    private fun publish() {
        // A touch stick that is being pushed wins over a resting controller stick.
        val touchLeft = touch.lx != 128 || touch.ly != 128
        val touchRight = touch.rx != 128 || touch.ry != 128
        val pad = RemotePlayProtocol.PadState(
            buttons = keyButtons or hatButtons or touch.buttons,
            lx = if (touchLeft) touch.lx else lx,
            ly = if (touchLeft) touch.ly else ly,
            rx = if (touchRight) touch.rx else rx,
            ry = if (touchRight) touch.ry else ry,
            l2 = max(max(l2Axis, if (l2Key) 255 else 0), touch.l2),
            r2 = max(max(r2Axis, if (r2Key) 255 else 0), touch.r2),
        )
        if (pad != last) {
            last = pad
            send(pad)
        }
    }

    private companion object {
        const val DEADZONE = 0.1f
    }
}

/** The viewer's connection: signaling, the peer, audio playback and the pad. Main thread. */
internal class ViewerSession(
    private val context: Context,
    private val mode: ViewerMode,
    private val output: ArmsxVideoOutput?,
    private val rumbleDevice: () -> Int,
    private val onInterrupted: (Boolean) -> Unit,
    private val onHostPaused: (Boolean) -> Unit,
    private val onReply: (String) -> Unit,
    private val onConnected: () -> Unit,
    private val onWaitingForHost: () -> Unit,
    private val onEnded: (String?) -> Unit,
) {
    private val main = Handler(Looper.getMainLooper())
    private val audio = AudioOut()
    private val opusIn = OpusIn(audio)
    private val stats = RemotePlayStats(sending = false)
    private val wifi = RemotePlayWifi(context)

    @Volatile
    private var link: SignalLink? = null
    private var rtc: RemotePlayRtc.Factory? = null
    private var peer: PeerConnection? = null
    private var gather: GatherWait? = null

    @Volatile
    private var inputChannel: DataChannel? = null
    private var audioChannel: DataChannel? = null
    private var pad = RemotePlayProtocol.PadState()
    private var seq = 0
    private var connectedOnce = false
    private var ended = false

    // The host's video, and where WebRTC's renderer draws it when the decoder does not draw it onto
    // the screen itself.
    private var videoTrack: VideoTrack? = null
    private var sink: VideoSink? = null

    /** How the host's audio arrives, for the stats: "Opus" or "uncompressed"; null before any. */
    @Volatile
    var audioKind: String? = null
        private set

    // Player 2's rumble from the host. The host repeats it every 200 ms, so motors left running
    // with nothing heard for a while belong to a connection that stalled, and stop.
    private var foreground = true
    private var hostPaused = false
    private var rumbleSeq = 0
    private var rumbleHeard = false
    private var rumbleOn = 0
    private val rumbleWatchdog = Runnable { stopRumble() }

    /** The host's side of the stream, as it last reported it (once a second). */
    @Volatile
    var hostStats: RemotePlayProtocol.HostStats? = null
        private set

    // The pad goes out on every change and also every 10 ms, which fixes a lost packet within
    // one beat, tells the host the viewer is still there, and keeps this device's Wi-Fi radio
    // from dozing between frames: a dozing radio has the router hold the stream's packets and
    // hand them over in a clump when it wakes (the round trip flipped between 8 and 83 ms in test
    // 19 on a strong link, with 70 to 100 ms gaps between frames). About 80 kbit/s; the host
    // ignores a pad state that has not changed.
    private val heartbeat = object : Runnable {
        override fun run() {
            if (ended) return
            sendNow()
            main.postDelayed(this, 10)
        }
    }

    private val prioritize = Runnable { if (!ended) RemotePlayRtc.prioritizeStreamThreads() }

    private val connectTimeout = Runnable {
        if (connectedOnce) return@Runnable
        end(
            I18n.get(
                when (mode) {
                    is ViewerMode.Wifi -> "remoteplay.error.connect"
                    is ViewerMode.Invite -> "remoteplay.timeout.viewer"
                    is ViewerMode.Lobby -> "remoteplay.timeout.host"
                },
            ),
        )
    }

    fun start() {
        when (mode) {
            is ViewerMode.Wifi -> connectWifi(mode.host, mode.port)
            is ViewerMode.Lobby -> joinLobby(mode.code)
            is ViewerMode.Invite -> {
                if (mode.offer.optInt("v") != RemotePlayProtocol.VERSION) {
                    end(I18n.get("remoteplay.bye.version"))
                    return
                }
                link = CodeLink { code -> main.post { if (!ended) onReply(code) } }
                answer(mode.offer.optString("sdp"))
                main.postDelayed(connectTimeout, INVITE_CONNECT_TIMEOUT_MS)
            }
        }
    }

    private fun joinLobby(code: String) {
        thread(name = "rp-lobby", isDaemon = true) {
            val result = RemotePlayLobby.offer(code)
            main.post {
                if (ended) return@post
                val offer = (result as? RemotePlayLobby.Result.Ok)?.value?.let { runCatching { JSONObject(it) }.getOrNull() }
                when {
                    offer != null && offer.optString("t") == "offer" -> {
                        if (offer.optInt("v") != RemotePlayProtocol.VERSION) {
                            end(I18n.get("remoteplay.bye.version"))
                            return@post
                        }
                        link = LobbyViewerLink(code) { failure -> main.post { end(lobbyMessage(failure)) } }
                        answer(offer.optString("sdp"))
                        main.postDelayed(connectTimeout, LOBBY_CONNECT_TIMEOUT_MS)
                    }
                    else -> end(lobbyMessage(result))
                }
            }
        }
    }

    private fun lobbyMessage(result: RemotePlayLobby.Result): String = when (result) {
        RemotePlayLobby.Result.Busy -> I18n.get("remoteplay.bye.busy")
        RemotePlayLobby.Result.NotFound -> I18n.get("remoteplay.join.notFound")
        else -> I18n.get("remoteplay.error.lobby")
    }

    private fun connectWifi(host: String, port: Int) {
        thread(name = "rp-connect", isDaemon = true) {
            val connection = try {
                SignalingConnection.connect(
                    host, port,
                    onMessage = { message -> main.post { onSignal(message) } },
                    // Once the stream is up the link has done its job.
                    onClosed = { main.post { if (!connectedOnce) end(I18n.get("remoteplay.viewer.lost")) } },
                )
            } catch (e: Exception) {
                Log.w(TAG, "connect to $host:$port failed", e)
                main.post { end(I18n.get("remoteplay.error.connect")) }
                return@thread
            }
            main.post {
                if (ended) {
                    connection.close()
                    return@post
                }
                link = TcpLink(connection)
                connection.start()
                connection.send(
                    JSONObject()
                        .put("t", "hello")
                        .put("v", RemotePlayProtocol.VERSION)
                        .put("name", playerName())
                        // Asks the host to mark the video for the router's video queue (see
                        // RemotePlayRtc.configuration), when the player turned that on.
                        .put("qos", RemotePlayPrefs.wifiQos(context)),
                )
                main.postDelayed(connectTimeout, WIFI_CONNECT_TIMEOUT_MS)
            }
        }
    }

    /** One line of stream statistics, for the overlay. [onLine] runs on WebRTC's thread. */
    fun sampleStats(onLine: (String) -> Unit) {
        peer?.let { stats.sample(it, onLine) }
    }

    /** From now on WebRTC's renderer draws the host's video, on [target]. */
    fun showOn(target: VideoSink) {
        if (ended) return
        sink = target
        videoTrack?.addSink(target)
    }

    /** Whether the screen is in front; rumble only plays while it is. */
    fun setForeground(on: Boolean) {
        foreground = on
        if (!on) stopRumble()
    }

    private fun onRumble(rumble: RemotePlayProtocol.Rumble) {
        if (ended || !foreground) return
        // The channel is unordered: an older packet must not undo a newer one.
        if (rumbleHeard && rumble.seq - rumbleSeq <= 0) return
        rumbleHeard = true
        rumbleSeq = rumble.seq
        if (rumble.paused != hostPaused) {
            hostPaused = rumble.paused
            onHostPaused(rumble.paused)
        }

        val packed = (rumble.large shl 8) or rumble.small
        if (packed != rumbleOn) {
            rumbleOn = packed
            RemotePlayCore.playRemoteRumble(rumbleDevice(), rumble.large, rumble.small)
        }
        main.removeCallbacks(rumbleWatchdog)
        if (packed != 0) main.postDelayed(rumbleWatchdog, 600)
    }

    private fun stopRumble() {
        main.removeCallbacks(rumbleWatchdog)
        if (rumbleOn == 0) return
        rumbleOn = 0
        RemotePlayCore.playRemoteRumble(-1, 0, 0)
    }

    // When the pad last went out and with which buttons, for spacing out stick movement
    // (ANALOG_SEND_MS); the movement in between goes out once that time is up.
    private var padSentAt = 0L
    private var padSentButtons = 0
    private var padLater = false
    private val sendLater = Runnable {
        padLater = false
        sendNow()
    }

    fun sendPad(state: RemotePlayProtocol.PadState) {
        pad = state
        val wait = padSentAt + ANALOG_SEND_MS - SystemClock.uptimeMillis()
        if (state.buttons != padSentButtons || wait <= 0) {
            sendNow()
        } else if (!padLater) {
            padLater = true
            main.postDelayed(sendLater, wait)
        }
    }

    private fun sendNow() {
        val channel = inputChannel ?: return
        if (channel.state() != DataChannel.State.OPEN) return
        if (padLater) {
            main.removeCallbacks(sendLater)
            padLater = false
        }
        padSentAt = SystemClock.uptimeMillis()
        padSentButtons = pad.buttons
        channel.send(DataChannel.Buffer(RemotePlayProtocol.encodePad(seq++, pad), true))
    }

    private fun onSignal(message: JSONObject) {
        if (ended) return
        when (message.optString("t")) {
            "offer" -> answer(message.optString("sdp"))
            "bye" -> end(byeMessage(message.optString("reason")))
        }
    }

    /** Answers the host's offer; the answer goes out once complete, candidates included. */
    private fun answer(offer: String) {
        if (peer != null || ended) return
        val created = RemotePlayRtc.createFactory(context, output).also { rtc = it }
        val factory = created.factory
        Log.i(
            TAG,
            "video decoder: ${created.h264Decoder ?: "no hardware H.264, stock factories"}" +
                if (output != null) ", straight onto the screen" else ", drawn by the renderer",
        )
        // Tells the host this device can play compressed audio (it can on any Android from 5).
        val audioFormat = if (OpusDecoder.available()) "opus" else "pcm"

        val wait = GatherWait(main, 4_000) {
            val description = peer?.localDescription ?: return@GatherWait
            link?.send(
                RemotePlayRtc.sdpMessage("answer", description)
                    .put("v", RemotePlayProtocol.VERSION)
                    .put("name", playerName())
                    .put("audio", audioFormat),
            )
        }
        gather = wait

        val pc = factory.createPeerConnection(RemotePlayRtc.configuration(), object : PeerObserverAdapter() {
            override fun onIceGatheringChange(state: PeerConnection.IceGatheringState?) {
                if (state == PeerConnection.IceGatheringState.COMPLETE) wait.gatheringComplete()
            }

            override fun onTrack(transceiver: RtpTransceiver) {
                val track = transceiver.receiver.track() as? VideoTrack ?: return
                main.post {
                    if (ended) return@post
                    videoTrack = track
                    sink?.let(track::addSink)
                }
            }

            override fun onDataChannel(channel: DataChannel) {
                when (channel.label()) {
                    RemotePlayProtocol.CHANNEL_INPUT -> inputChannel = channel
                    RemotePlayProtocol.CHANNEL_AUDIO -> {
                        channel.registerObserver(MessageObserver { buffer ->
                            val data = buffer.data
                            if (RemotePlayProtocol.isBye(data)) {
                                val reason = RemotePlayProtocol.byeReason(data)
                                main.post { end(byeMessage(reason)) }
                                return@MessageObserver
                            }
                            if (RemotePlayProtocol.isWaiting(data)) {
                                main.post { if (!ended) onWaitingForHost() }
                                return@MessageObserver
                            }
                            RemotePlayProtocol.decodeHostStats(data)?.let {
                                hostStats = it
                                return@MessageObserver
                            }
                            RemotePlayProtocol.decodeRumble(data)?.let { rumble ->
                                main.post { onRumble(rumble) }
                                return@MessageObserver
                            }
                            RemotePlayProtocol.decodeOpus(data)?.let {
                                audioKind = "Opus"
                                opusIn.offer(it)
                                return@MessageObserver
                            }
                            RemotePlayProtocol.decodeAudio(data)?.let {
                                audioKind = "uncompressed"
                                audio.enqueue(it)
                            }
                        })
                        main.post { audioChannel = channel }
                    }
                }
            }

            override fun onConnectionChange(newState: PeerConnection.PeerConnectionState) {
                main.post { onConnectionState(newState) }
            }
        })
        if (pc == null) {
            end(I18n.get("remoteplay.viewer.lost"))
            return
        }
        peer = pc

        val failed: (String) -> Unit = { error ->
            Log.w(TAG, "viewer negotiation failed: $error")
            main.post { end(I18n.get("remoteplay.viewer.lost")) }
        }
        pc.setRemoteDescription(object : SdpAdapter(failed) {
            override fun onSetSuccess() {
                pc.createAnswer(object : SdpAdapter(failed) {
                    override fun onCreateSuccess(description: SessionDescription) {
                        pc.setLocalDescription(object : SdpAdapter(failed) {
                            override fun onSetSuccess() = wait.localDescriptionSet()
                        }, description)
                    }
                }, MediaConstraints())
            }
        }, SessionDescription(SessionDescription.Type.OFFER, offer))
    }

    private fun onConnectionState(state: PeerConnection.PeerConnectionState) {
        if (ended) return
        when (state) {
            PeerConnection.PeerConnectionState.CONNECTED -> {
                onInterrupted(false)
                if (!connectedOnce) {
                    connectedOnce = true
                    main.removeCallbacks(connectTimeout)
                    wifi.hold()
                    RemotePlayRtc.prioritizeStreamThreads()
                    main.postDelayed(prioritize, 2_000)
                    onConnected()
                    main.removeCallbacks(heartbeat)
                    main.post(heartbeat)
                }
            }
            // Before the first connection a failure can still recover: online, the host has not
            // necessarily pasted the reply yet. The connect timeout decides. After it, failed means gone.
            PeerConnection.PeerConnectionState.FAILED -> if (connectedOnce) end(I18n.get("remoteplay.viewer.lost"))
            PeerConnection.PeerConnectionState.CLOSED -> end(I18n.get("remoteplay.viewer.lost"))
            // Connected before and nothing heard for a few seconds: WebRTC keeps trying for a
            // while before it gives up (FAILED above), and the screen says so meanwhile.
            PeerConnection.PeerConnectionState.DISCONNECTED -> if (connectedOnce) onInterrupted(true)
            else -> Unit
        }
    }

    private fun byeMessage(reason: String): String = when (reason) {
        "busy" -> I18n.get("remoteplay.bye.busy")
        "stopped" -> I18n.get("remoteplay.bye.stopped")
        "version" -> I18n.get("remoteplay.bye.version")
        "denied" -> I18n.get("remoteplay.bye.denied")
        "noanswer" -> I18n.get("remoteplay.bye.noAnswer")
        else -> I18n.get("remoteplay.bye.ended")
    }

    /** Idempotent. [message] is shown to the player; null when they left on purpose. */
    fun end(message: String?) {
        if (ended) return
        ended = true
        main.removeCallbacks(heartbeat)
        main.removeCallbacks(sendLater)
        main.removeCallbacks(prioritize)
        main.removeCallbacks(connectTimeout)
        gather?.cancel()

        // A few times, since the channel drops what it cannot deliver.
        inputChannel?.takeIf { it.state() == DataChannel.State.OPEN }?.let { channel ->
            repeat(3) { channel.send(DataChannel.Buffer(RemotePlayProtocol.encodeBye(), true)) }
        }
        link?.close(RemotePlayRtc.bye("left"))
        stopRumble()
        opusIn.stop()
        audio.stop()
        wifi.release()

        // The renderer, when it draws: a sink left on the track keeps it, and this screen, alive.
        sink?.let { renderer -> runCatching { videoTrack?.removeSink(renderer) } }
        runCatching { peer?.close() }
        runCatching { audioChannel?.unregisterObserver() }
        runCatching { audioChannel?.dispose() }
        runCatching { inputChannel?.dispose() }
        runCatching { peer?.dispose() }
        rtc?.dispose()
        peer = null
        videoTrack = null
        inputChannel = null
        audioChannel = null
        onEnded(message)
    }
}

/**
 * Plays the host's audio packets. Holds about 40 ms before starting so small network wobbles do
 * not underrun, and drops the oldest audio beyond about 120 ms so the sound never drifts behind
 * the picture.
 */
internal class AudioOut {
    private val queue = LinkedBlockingDeque<ByteArray>()
    private val queuedBytes = AtomicInteger(0)

    @Volatile
    private var running = false
    private var worker: Thread? = null

    // Set by stop(): a packet still on its way in then must not start playback again.
    private var closed = false

    @Synchronized
    fun enqueue(pcm: ByteArray) {
        if (closed) return
        if (!running) {
            running = true
            worker = thread(name = "rp-audio-out", isDaemon = true) { play() }
        }
        queue.offerLast(pcm)
        queuedBytes.addAndGet(pcm.size)
        while (queuedBytes.get() > MAX_QUEUED_BYTES) {
            val dropped = queue.pollFirst() ?: break
            queuedBytes.addAndGet(-dropped.size)
        }
    }

    fun stop() {
        synchronized(this) { closed = true }
        running = false
        worker?.join(300)
        worker = null
        queue.clear()
        queuedBytes.set(0)
    }

    private fun play() {
        val minimum = AudioTrack.getMinBufferSize(
            RemotePlayProtocol.SAMPLE_RATE,
            AudioFormat.CHANNEL_OUT_STEREO,
            AudioFormat.ENCODING_PCM_16BIT,
        )
        val track = runCatching {
            AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                        .build(),
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setSampleRate(RemotePlayProtocol.SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .build(),
                )
                .setBufferSizeInBytes(max(minimum, START_BYTES))
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .build()
        }.getOrElse {
            Log.w(TAG, "no audio output", it)
            return
        }

        try {
            var primed = false
            track.play()
            while (running) {
                if (!primed) {
                    if (queuedBytes.get() < START_BYTES) {
                        Thread.sleep(5)
                        continue
                    }
                    primed = true
                }
                val chunk = queue.pollFirst(100, TimeUnit.MILLISECONDS)
                if (chunk == null) {
                    // Ran dry: wait for a fresh cushion instead of stuttering chunk by chunk.
                    primed = false
                    continue
                }
                queuedBytes.addAndGet(-chunk.size)
                track.write(chunk, 0, chunk.size)
            }
        } catch (_: InterruptedException) {
        } finally {
            runCatching { track.stop() }
            track.release()
        }
    }

    private companion object {
        const val BYTES_PER_MS = RemotePlayProtocol.SAMPLE_RATE * 4 / 1000
        const val START_BYTES = BYTES_PER_MS * 40
        const val MAX_QUEUED_BYTES = BYTES_PER_MS * 120
    }
}

// How the host's screen names this player: the Discord name when signed in there, the device's model
// otherwise.
private fun playerName(): String {
    val discord = com.armsx2.DiscordPresence.self.value?.name
    val signedIn = com.armsx2.DiscordPresence.status.value == com.armsx2.DiscordPresence.CONNECTED
    return if (signedIn && !discord.isNullOrBlank()) discord else Build.MODEL
}
