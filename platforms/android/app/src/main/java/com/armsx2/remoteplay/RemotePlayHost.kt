// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.KeyEvent
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateOf
import com.armsx2.i18n.I18n
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONObject
import org.webrtc.ArmsxVideoEncoderFactory
import org.webrtc.CapturerObserver
import org.webrtc.DataChannel
import org.webrtc.MediaConstraints
import org.webrtc.PeerConnection
import org.webrtc.RtpParameters
import org.webrtc.RtpTransceiver
import org.webrtc.SessionDescription
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import java.io.IOException
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread

private const val TAG = "RemotePlay"

/** Player 2 is pad port 1. Pushing data to it is what plugs the controller in (see overlayPadData). */
private const val PLAYER_2_PORT = 1

/** After the friend's answer arrives, how long the connection gets to come up. */
private const val CONNECT_TIMEOUT_MS = 60_000L

/** On the same Wi-Fi, how long a viewer that connected gets to answer the offer. */
private const val WIFI_ANSWER_TIMEOUT_MS = 30_000L

/** How long the host gets to let a friend in before the friend is told there was no answer. */
private const val ASK_TIMEOUT_MS = 60_000L

/** While the host decides, how often the friend's screen is told it is waiting for them. */
private const val ASK_TELL_MS = 500L

/**
 * The hosting side. Takes one viewer at a time, streams the running game to it and feeds the
 * viewer's pad in as player 2. A viewer arrives one of two ways: on the same Wi-Fi it finds this
 * device by itself ([startWifi]); online, it joins with an invite ([createInvite]): a six-character
 * code through the lobby service, or, when that cannot be reached, a pasted code with a pasted
 * reply ([acceptReply]).
 *
 * Process-wide rather than tied to a screen, so closing the in-game menu, or leaving for a chat
 * app to send an invite, does not end anything. Hosting ends when it is stopped or the game
 * closes. Main thread unless a comment says otherwise.
 */
object RemotePlayHost {
    sealed interface Status {
        data object Off : Status
        /** Hosting on this Wi-Fi. [address] is what a viewer can type if this device is not listed. */
        data class Waiting(val address: String?) : Status
        /**
         * An online invite: [lobbyCode] is the short code to give the friend; [code] is the long
         * pasted one used when the lobby service cannot be reached. Both null while preparing.
         */
        data class Inviting(val code: String? = null, val lobbyCode: String? = null) : Status
        data class Connecting(val viewer: String) : Status
        /**
         * [viewer] is connected and waits for the host to let them in ([accept]) or not
         * ([decline]). Nothing streams and their pad does nothing until then.
         */
        data class Asking(val viewer: String) : Status
        data class Playing(val viewer: String) : Status
        data class Failed(val message: String) : Status
    }

    private val _status = mutableStateOf<Status>(Status.Off)
    val status: State<Status> get() = _status

    /**
     * A friend joining or leaving as player 2, for the banner over the game (RemotePlayNoticeBanner):
     * the host is playing, with the in-game menu's hosting status out of sight. A new object each
     * time, so the same friend joining again still shows.
     */
    class Notice(val name: String, val joined: Boolean)

    private val _notice = mutableStateOf<Notice?>(null)
    val notice: State<Notice?> get() = _notice

    /** The banner is done with [shown]; a newer notice stays. */
    fun clearNotice(shown: Notice) {
        if (_notice.value === shown) _notice.value = null
    }

    private val main = Handler(Looper.getMainLooper())
    private var appContext: Context? = null
    private var server: SignalingServer? = null
    private var advertiser: HostAdvertiser? = null
    private var session: HostSession? = null

    // Same Wi-Fi: the address the current viewer connected from, and the addresses the host turned
    // away while hosting, which are turned away again at once instead of asking each time.
    private var sessionAddress: String? = null
    private val declined = HashSet<String>()

    // The in-game menu was opened to ask the host, so it closes again once they have answered.
    private var openedToAsk = false

    // Hosting belongs to the running game: a closed game ends it, whichever path closed it.
    private val gameWatch = object : Runnable {
        override fun run() {
            if (RemotePlayCore.runningGame() == null) {
                stop()
                return
            }
            main.postDelayed(this, 1_000)
        }
    }

    /** Host on this Wi-Fi: listen, and announce this device so viewers list it. */
    fun startWifi(context: Context) {
        if (server != null) return
        val game = runningGame() ?: return
        val app = context.applicationContext
        appContext = app

        val listener = SignalingServer { socket -> main.post { onWifiViewer(socket) } }
        if (!listener.start()) {
            _status.value = Status.Failed(I18n.get("remoteplay.host.noNetwork"))
            return
        }
        server = listener
        advertiser = HostAdvertiser(app).also { it.register(Build.MODEL, listener.port, game) }
        if (session == null) _status.value = Status.Waiting(address(app, listener.port))
        watchGame()
    }

    /** Online: prepare an offer and put it up under a short code for the friend to type. */
    fun createInvite(context: Context) {
        if (session != null) return
        val game = runningGame() ?: return
        val app = context.applicationContext
        appContext = app

        lateinit var created: HostSession
        val link = LobbyHostLink(
            onCode = { code -> main.post { if (session === created) _status.value = Status.Inviting(lobbyCode = code) } },
            onFallback = { pasted -> main.post { if (session === created) _status.value = Status.Inviting(code = pasted) } },
            onAnswer = { answer -> main.post { if (session === created) acceptAnswer(created, answer) } },
            onExpired = { main.post { if (session === created) sessionEnded(created, I18n.get("remoteplay.invite.expired")) } },
        )
        created = HostSession(
            app, link, game, online = true,
            onFinished = { ended, message -> main.post { if (session === ended) sessionEnded(ended, message) } },
        )
        session = created
        _status.value = Status.Inviting()
        watchGame()
        created.offer()
    }

    /**
     * The friend's reply to an invite, as pasted (the whole chat message is fine). False when it
     * holds no reply code; the invite stays open so the player can try again.
     */
    fun acceptReply(text: String): Boolean {
        val pending = session ?: return false
        val message = RemotePlayProtocol.decodeCode(text) ?: return false
        if (message.optString("t") != "answer") return false
        acceptAnswer(pending, message)
        return true
    }

    private fun acceptAnswer(pending: HostSession, message: JSONObject) {
        if (message.optInt("v") != RemotePlayProtocol.VERSION) {
            sessionEnded(pending, I18n.get("remoteplay.bye.version"))
            return
        }
        pending.onSignal(message)
    }

    /** Hosting on this Wi-Fi was asked for, and the local network permission refused (Android 17). */
    fun refuseLocalNetwork() {
        if (server != null || session != null) return
        _status.value = Status.Failed(I18n.get("remoteplay.permission.host"))
    }

    fun stop() {
        main.removeCallbacks(gameWatch)
        session?.end("stopped")
        session = null
        sessionAddress = null
        openedToAsk = false
        stopWifi()
        _status.value = Status.Off
    }

    private fun stopWifi() {
        advertiser?.unregister()
        advertiser = null
        server?.stop()
        server = null
        declined.clear()
    }

    /** Lets in the friend the host is being asked about ([Status.Asking]). */
    fun accept() {
        if (_status.value !is Status.Asking) return
        session?.approve()
        closeAskMenu()
    }

    /**
     * Turns away the friend the host is being asked about. On the same Wi-Fi their device is then
     * turned away at once for as long as this hosting lasts, so it cannot keep asking.
     */
    fun decline() {
        if (_status.value !is Status.Asking) return
        sessionAddress?.let(declined::add)
        session?.decline()
        closeAskMenu()
    }

    // A viewer is connected and waits to be let in. The host answers in the in-game menu, which is
    // opened for it (pausing the game) unless it is open already.
    internal fun ask(viewer: String) {
        _status.value = Status.Asking(viewer)
        if (!com.armsx2.ui.WindowImpl.overlayVisible.value) {
            com.armsx2.ui.InGameOverlay.open()
            openedToAsk = true
        }
    }

    // Back to the game once the host has answered, if the menu was opened only to ask them.
    private fun closeAskMenu() {
        if (!openedToAsk) return
        openedToAsk = false
        if (com.armsx2.ui.WindowImpl.overlayVisible.value) com.armsx2.ui.InGameOverlay.toggle()
    }

    private fun runningGame(): String? {
        val game = RemotePlayCore.runningGame()
        if (game == null) _status.value = Status.Failed(I18n.get("remoteplay.host.noGame"))
        return game
    }

    private fun watchGame() {
        main.removeCallbacks(gameWatch)
        main.postDelayed(gameWatch, 1_000)
    }

    private fun address(context: Context, port: Int): String? {
        val ip = localIpv4(context) ?: return null
        return if (port == RemotePlayProtocol.PORT) ip else "$ip:$port"
    }

    private fun onWifiViewer(socket: Socket) {
        val app = appContext
        val game = RemotePlayCore.runningGame()
        val address = socket.inetAddress?.hostAddress
        // A peer that connected and reset at once has no streams left to talk on. The IOException
        // that says so, uncaught here on the main thread, took the app down with the host's game in
        // it, so every SignalingConnection made here is guarded.
        val turnedAway = address != null && address in declined
        if (session != null || server == null || app == null || game == null || turnedAway) {
            // One player 2 at a time; and a device the host turned away stays turned away.
            val reason = if (turnedAway) "denied" else "busy"
            runCatching { SignalingConnection(socket, {}, {}).also { it.start() }.sendAndClose(RemotePlayRtc.bye(reason)) }
                .onFailure { runCatching { socket.close() } }
            return
        }

        lateinit var created: HostSession
        val connection = try {
            SignalingConnection(
                socket,
                onMessage = { message -> main.post { if (session === created) created.onSignal(message) } },
                onClosed = { main.post { if (session === created) created.linkClosed() } },
            )
        } catch (_: IOException) {
            runCatching { socket.close() }
            return
        }
        created = HostSession(
            app, TcpLink(connection), game, online = false,
            onFinished = { ended, message -> main.post { if (session === ended) sessionEnded(ended, message) } },
        )
        session = created
        sessionAddress = address
        created.expectAnswerWithin(WIFI_ANSWER_TIMEOUT_MS)
        connection.start()
    }

    /** [message] is shown to the player when the session ended on a problem, not by choice. */
    private fun sessionEnded(ended: HostSession, message: String?) {
        // Player 2 is gone, whether they left or their connection did. Stopping hosting does not
        // come through here: the host knows they did that.
        if (ended.hadPlayer) _notice.value = Notice(ended.playerName, joined = false)
        ended.end("ended")
        session = null
        sessionAddress = null
        // A menu opened to ask about this viewer stays: the host may be using it by now.
        openedToAsk = false
        val listener = server
        _status.value = when {
            message != null -> Status.Failed(message)
            listener != null -> Status.Waiting(appContext?.let { address(it, listener.port) })
            else -> Status.Off
        }
        if (message != null) {
            // A failure ends hosting entirely, so the message is not replaced by a list nobody asked for.
            main.removeCallbacks(gameWatch)
            stopWifi()
        } else if (listener == null) {
            main.removeCallbacks(gameWatch)
        }
    }

    internal fun report(status: Status) {
        main.post { _status.value = status }
    }

    internal fun announce(notice: Notice) {
        main.post { _notice.value = notice }
    }
}

/**
 * One viewer, from the offer to the bye. [online]: the viewer came with an invite rather than over
 * this Wi-Fi, which sets the video bitrate and compresses the audio. Main thread unless a comment
 * says otherwise.
 */
private class HostSession(
    private val context: Context,
    private val link: SignalLink,
    private val game: String,
    private val online: Boolean,
    private val onFinished: (HostSession, String?) -> Unit,
) {
    private val main = Handler(Looper.getMainLooper())
    private val bitrate = if (online) RemotePlayProtocol.ONLINE_BITRATE else RemotePlayProtocol.WIFI_BITRATE

    // Online, the audio goes out as Opus when the viewer plays it. On the same Wi-Fi it stays plain
    // samples: there is bandwidth to spare, and the sound arrives sooner (Opus gathers 20 ms before
    // it encodes, and looks 6.5 ms ahead).
    @Volatile
    private var opusAudio = false
    private var viewer = "?"

    // Sent to the friend with the offer, so both devices' Discord status carry the same party and
    // Discord shows them as one remote play session.
    private val partyId = "rp-" + java.util.UUID.randomUUID()

    /** Who is playing, as their device introduced itself. */
    val playerName: String get() = viewer

    /** The friend got as far as playing: the stream started. Main thread. */
    var hadPlayer = false
        private set
    private var rtc: RemotePlayRtc.Factory? = null
    private var peer: PeerConnection? = null
    private var gather: GatherWait? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null
    private var inputChannel: DataChannel? = null

    @Volatile
    private var audioChannel: DataChannel? = null

    @Volatile
    private var streaming = false
    private var videoThread: Thread? = null
    private var audioThread: Thread? = null
    // Built and used on the video thread; released by end()'s thread once that one has ended.
    @Volatile
    private var frames: RemotePlayFrames? = null
    private var offered = false
    private var answered = false
    private var connectedOnce = false
    @Volatile
    private var ended = false

    // Same Wi-Fi: the viewer turned on QoS marking ("qos" in its hello).
    private var qos = false

    // Player 2's pad packets are applied here, not on the main thread, which the game's own
    // screens also use: a press never waits behind them. Audio priority, since each packet is a
    // few microseconds of work and its delay is the delay of the press.
    private val inputThread = HandlerThread("rp-input", Process.THREAD_PRIORITY_AUDIO).apply { start() }
    private val input = Handler(inputThread.looper)

    private val wifi = RemotePlayWifi(context)

    // Online: the host's router asked to let the friend in (UPnP), for this session only.
    private val upnp = if (online) RemotePlayUpnp() else null

    // Input thread.
    private var appliedPad = RemotePlayProtocol.PadState()
    private var lastPadSeq = 0
    private var hasPadSeq = false
    private var lastPadAt = 0L

    // Input thread. Releases player 2's buttons when its packets stop arriving, so a dropped
    // connection does not leave a direction held until it recovers.
    private val padWatch = object : Runnable {
        override fun run() {
            if (ended) return
            if (SystemClock.uptimeMillis() - lastPadAt > 1_000) applyPad(RemotePlayProtocol.PadState())
            input.postDelayed(this, 250)
        }
    }

    private val connectTimeout = Runnable {
        if (!connectedOnce) finish(I18n.get("remoteplay.timeout.host"))
    }

    // Same Wi-Fi: a viewer that connected and then never answered (gone from the network, or
    // anything else that opened the port) must not hold hosting as "busy" for everyone else. The
    // session ends quietly and hosting goes back to waiting.
    private val answerTimeout = Runnable {
        if (!answered && !ended) finish(null)
    }

    /** Ends the session quietly unless the viewer answers within [ms]. */
    fun expectAnswerWithin(ms: Long) {
        main.removeCallbacks(answerTimeout)
        main.postDelayed(answerTimeout, ms)
    }

    // Nobody streams to a viewer, or takes their pad, until the host lets them in: connecting is
    // not joining. [asking] while the host is being asked; [approved] once they said yes.
    private var asking = false
    private var approved = false

    // Why the viewer is told the session ended, when it is not the reason end() is given: the host
    // turned them away ("denied") or never answered ("noanswer").
    private var byeReason: String? = null

    private val askTimeout = Runnable {
        if (asking && !ended) {
            byeReason = "noanswer"
            finish(null)
        }
    }

    // The viewer's screen says it is waiting for the host while it is. The channel drops what it
    // cannot deliver, so it is told again every half second instead of once.
    private val askTell = object : Runnable {
        override fun run() {
            if (!asking || ended) return
            audioChannel?.takeIf { it.state() == DataChannel.State.OPEN }
                ?.send(DataChannel.Buffer(RemotePlayProtocol.encodeWaiting(), true))
            main.postDelayed(this, ASK_TELL_MS)
        }
    }

    private fun ask() {
        if (asking || approved || ended) return
        asking = true
        RemotePlayHost.ask(viewer)
        main.post(askTell)
        main.postDelayed(askTimeout, ASK_TIMEOUT_MS)
    }

    /** The host let the viewer in: the stream starts. */
    fun approve() {
        if (!asking || ended) return
        asking = false
        approved = true
        main.removeCallbacks(askTell)
        main.removeCallbacks(askTimeout)
        startStreaming()
    }

    /** The host turned the viewer away: the session ends and the viewer is told so. */
    fun decline() {
        if (ended) return
        asking = false
        byeReason = "denied"
        finish(null)
    }

    // What the stream is doing, in logcat (tag RemotePlay) every few seconds while it runs.
    private val stats = RemotePlayStats(sending = true)
    private val statsTick = object : Runnable {
        override fun run() {
            if (ended || !streaming) return
            val host = hostStats?.let(::describeHost)
            peer?.let { pc -> stats.sample(pc) { line -> Log.i(TAG, "host stream: $line" + (host?.let { " · $it" } ?: "")) } }
            main.postDelayed(this, 5_000)
        }
    }

    // The host's side of the stream, sent to the viewer once a second for its stats: what the game
    // drew, what was captured and encoded, and what was dropped on the way. A few bytes a second.
    private var hostStats: RemotePlayProtocol.HostStats? = null
    private var lastCore: IntArray? = null
    private var lastEncoder: IntArray? = null

    // Frames handed to WebRTC (rp-video thread). Those that never reach the encoder were skipped
    // inside WebRTC, which the stats show.
    @Volatile
    private var framesHanded = 0
    private var lastHanded = -1

    // The longest wait between two frames handed to WebRTC since the stats last took it (ms): a
    // stall that starts here is the game's or the capture's, not the network's.
    private val peakHandGapMs = java.util.concurrent.atomic.AtomicInteger()
    private val rtcSampler = HostRtcSampler()
    private val hostStatsTick = object : Runnable {
        override fun run() {
            if (ended || !streaming) return
            // WebRTC answers in the background; its numbers go with the next second's stats.
            peer?.let(rtcSampler::sample)
            sendHostStats()
            main.postDelayed(this, 1_000)
        }
    }

    // Player 2's rumble, as the game drives it, for the viewer to feel, and whether the game is
    // paused, for the viewer to say so instead of showing a frozen picture: checked every 30 ms, as
    // the local motors are, and sent on every change and every 200 ms besides.
    private var rumbleSent = -1
    private var rumbleSentAt = 0L
    private var rumbleSeq = 0
    private val rumbleTick = object : Runnable {
        override fun run() {
            if (ended || !streaming) return
            val paused = RemotePlayCore.isPaused()
            val packed = RemotePlayCore.remoteRumble() or (if (paused) 1 shl 16 else 0)
            val now = SystemClock.uptimeMillis()
            val channel = audioChannel
            if ((packed != rumbleSent || now - rumbleSentAt >= 200) && channel?.state() == DataChannel.State.OPEN) {
                val packet = RemotePlayProtocol.encodeRumble(rumbleSeq++, (packed shr 8) and 0xff, packed and 0xff, paused)
                channel.send(DataChannel.Buffer(packet, true))
                rumbleSent = packed
                rumbleSentAt = now
            }
            main.postDelayed(this, 30)
        }
    }

    fun onSignal(message: JSONObject) {
        if (ended) return
        when (message.optString("t")) {
            "hello" -> {
                if (message.optInt("v") != RemotePlayProtocol.VERSION) {
                    link.close(RemotePlayRtc.bye("version"))
                    finish(null)
                    return
                }
                viewer = message.optString("name").ifBlank { "?" }.take(40)
                qos = message.optBoolean("qos")
                RemotePlayHost.report(RemotePlayHost.Status.Connecting(viewer))
                offer()
            }
            "answer" -> {
                val pc = peer ?: return
                if (answered) return
                answered = true
                viewer = message.optString("name").ifBlank { viewer }.take(40)
                opusAudio = online && message.optString("audio") == "opus"
                RemotePlayHost.report(RemotePlayHost.Status.Connecting(viewer))
                pc.setRemoteDescription(
                    SdpAdapter { error -> fail("answer: $error") },
                    SessionDescription(SessionDescription.Type.ANSWER, message.optString("sdp")),
                )
                main.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS)
            }
            "bye" -> finish(null)
        }
    }

    /** The Wi-Fi link dropped. Before the stream is up that is the end; after, the stream carries on. */
    fun linkClosed() {
        if (!connectedOnce) finish(null)
    }

    /** Builds the peer and sends the offer once it is complete, candidates included. */
    fun offer() {
        if (offered || ended) return
        offered = true

        // Looks for the router while WebRTC gathers, so the offer seldom waits for it.
        upnp?.start()
        val created = RemotePlayRtc.createFactory(context).also { rtc = it }
        val factory = created.factory
        Log.i(TAG, "video encoder: ${created.h264Encoder ?: "no hardware H.264, stock factories"}")
        val wait = GatherWait(main, 4_000) {
            val description = peer?.localDescription ?: return@GatherWait
            val send = { sdp: String ->
                link.send(
                    RemotePlayRtc.sdpMessage("offer", sdp)
                        .put("v", RemotePlayProtocol.VERSION)
                        .put("game", game.take(80))
                        .put("serial", RemotePlayCore.runningSerial())
                        .put("party", partyId)
                        .put("host", Build.MODEL),
                )
            }
            val router = upnp
            if (router == null) {
                send(description.description)
            } else {
                thread(name = "rp-upnp-offer", isDaemon = true) {
                    val sdp = router.addCandidates(description.description, 3_000)
                    main.post { if (!ended) send(sdp) }
                }
            }
        }
        gather = wait

        val pc = factory.createPeerConnection(RemotePlayRtc.configuration(dscp = qos), object : PeerObserverAdapter() {
            override fun onIceGatheringChange(state: PeerConnection.IceGatheringState?) {
                if (state == PeerConnection.IceGatheringState.COMPLETE) wait.gatheringComplete()
            }

            override fun onConnectionChange(newState: PeerConnection.PeerConnectionState) {
                main.post { onConnectionState(newState) }
            }
        })
        if (pc == null) {
            fail("no peer connection")
            return
        }
        peer = pc
        // Start near where a game stream settles instead of ramping up from a few hundred kbps,
        // which left the first seconds blurry and stuttering.
        pc.setBitrate(bitrate.min, bitrate.start, bitrate.max)

        val source = factory.createVideoSource(false).also { videoSource = it }
        val track = factory.createVideoTrack("game", source).also { videoTrack = it }
        val transceiver = pc.addTransceiver(
            track,
            RtpTransceiver.RtpTransceiverInit(RtpTransceiver.RtpTransceiverDirection.SEND_ONLY, listOf("armsx2")),
        )
        RemotePlayRtc.restrictCodecs(factory, transceiver)

        // Unordered and never retransmitted: a late pad state is worse than a missing one, since
        // the next packet carries the whole pad anyway, and late audio is just a delay.
        fun lossy() = DataChannel.Init().apply {
            ordered = false
            maxRetransmits = 0
        }
        inputChannel = pc.createDataChannel(RemotePlayProtocol.CHANNEL_INPUT, lossy())?.also { channel ->
            channel.registerObserver(MessageObserver { buffer -> onInputPacket(buffer.data) })
        }
        audioChannel = pc.createDataChannel(RemotePlayProtocol.CHANNEL_AUDIO, lossy())

        pc.createOffer(object : SdpAdapter({ error -> fail("offer: $error") }) {
            override fun onCreateSuccess(description: SessionDescription) {
                pc.setLocalDescription(object : SdpAdapter({ error -> fail("local: $error") }) {
                    override fun onSetSuccess() = wait.localDescriptionSet()
                }, description)
            }
        }, MediaConstraints())
    }

    private fun onConnectionState(state: PeerConnection.PeerConnectionState) {
        if (ended) return
        when (state) {
            PeerConnection.PeerConnectionState.CONNECTED -> {
                connectedOnce = true
                main.removeCallbacks(connectTimeout)
                // The first time, the host is asked; after a reconnect, streaming carries on.
                if (approved) startStreaming() else ask()
            }
            // Before the first connection, a failure can still recover once the friend's side
            // starts answering; the connect timeout decides. After it, failed means gone.
            PeerConnection.PeerConnectionState.FAILED -> if (connectedOnce) finish(null)
            PeerConnection.PeerConnectionState.CLOSED -> finish(null)
            else -> Unit
        }
    }

    private fun startStreaming() {
        if (streaming || ended) return
        val pc = peer ?: return
        val source = videoSource ?: return

        if (!NativeApp.remotePlayStart(RemotePlayProtocol.MAX_WIDTH, RemotePlayProtocol.MAX_HEIGHT)) {
            fail("capture did not start")
            return
        }
        streaming = true
        // Port 2 gets a pad now, if it had none: the friend is player 2 from here.
        RemotePlayCore.hostPlayer2(true)

        // The frame rate and the resolution never change; a tight network only softens the picture.
        // Left to adapt, WebRTC lowered the resolution whenever the picture got busy (a scene
        // change, moving again after standing still) and raised it once things calmed down, and
        // every switch restarts the encoder here and the decoder on the viewer: a hitch each time.
        pc.senders.firstOrNull { it.track()?.kind() == "video" }?.let { sender ->
            val parameters = sender.parameters
            parameters.degradationPreference = RtpParameters.DegradationPreference.DISABLED
            parameters.encodings.forEach {
                it.maxBitrateBps = bitrate.max
                it.maxFramerate = 60
            }
            sender.setParameters(parameters)
        }

        RemotePlayHost.report(RemotePlayHost.Status.Playing(viewer))
        hadPlayer = true
        com.armsx2.DiscordPresence.joinRemotePlay(com.armsx2.DiscordPresence.RemotePlayParty(partyId))
        RemotePlayHost.announce(RemotePlayHost.Notice(viewer, joined = true))
        wifi.hold()
        val observer = source.capturerObserver
        observer.onCapturerStarted(true)
        // The pipeline is built on the video thread: it sets up a GL context and waits for it, for
        // up to a second, which on the main thread held the host's game.
        val textureInput = rtc?.textureInput == true
        videoThread = thread(name = "rp-video") {
            val pipeline = RemotePlayFrames(textureInput)
            frames = pipeline
            videoLoop(observer, pipeline)
        }
        audioThread = thread(name = "rp-audio") { audioLoop() }
        input.post { lastPadAt = SystemClock.uptimeMillis() }
        input.postDelayed(padWatch, 250)
        main.postDelayed(statsTick, 5_000)
        main.postDelayed(hostStatsTick, 1_000)
        main.post(rumbleTick)
        RemotePlayRtc.prioritizeStreamThreads()
        main.postDelayed(prioritize, 1_000)
        main.postDelayed(prioritize, 3_000)
    }

    private val prioritize = Runnable { if (!ended) RemotePlayRtc.prioritizeStreamThreads() }

    private fun sendHostStats() {
        val core = IntArray(4)
        if (!NativeApp.remotePlayCounters(core)) return
        val encoder = ArmsxVideoEncoderFactory.encoderCounters()
        val peaks = ArmsxVideoEncoderFactory.takeEncoderPeaks()
        val handed = framesHanded
        val previousCore = lastCore
        val previousEncoder = lastEncoder
        val previousHanded = lastHanded
        lastCore = core
        lastEncoder = encoder
        lastHanded = handed
        if (previousCore == null || previousEncoder == null || previousHanded < 0) return

        // Counters are running totals; the stats are the last second's share of them.
        fun coreDelta(index: Int) = core[index] - previousCore[index]
        fun encoderDelta(index: Int) = encoder[index] - previousEncoder[index]
        val rtc = rtcSampler.latest
        val link = WifiLink.read(context)
        val current = RemotePlayProtocol.HostStats(
            gameFps = coreDelta(0),
            capturedFps = coreDelta(2),
            encodedFps = encoderDelta(1),
            droppedGpu = coreDelta(1),
            droppedLate = coreDelta(3),
            droppedEncoder = encoderDelta(2),
            droppedWebrtc = (handed - previousHanded - encoderDelta(0) - encoderDelta(2)).coerceAtLeast(0),
            peakEncodeMs = peaks[0],
            peakFrameKb = (peaks[1] + 1023) / 1024,
            encoder = ArmsxVideoEncoderFactory.encoderName(),
            sentFps = rtc?.sentFps ?: 0,
            bweKbps = rtc?.bweKbps ?: 0,
            targetKbps = rtc?.targetKbps ?: 0,
            nacks = rtc?.nacks ?: 0,
            plis = rtc?.plis ?: 0,
            wifiMhz = link?.frequencyMhz ?: 0,
            wifiTxMbps = link?.txMbps ?: 0,
            wifiRxMbps = link?.rxMbps ?: 0,
            wifiRssi = link?.rssiDbm ?: 0,
            captureGapMs = peakHandGapMs.getAndSet(0),
            pacerMs = rtc?.pacerMs ?: 0,
        )
        hostStats = current
        audioChannel?.takeIf { it.state() == DataChannel.State.OPEN }
            ?.send(DataChannel.Buffer(RemotePlayProtocol.encodeHostStats(current), true))
    }

    // rp-video thread. Pulls frames from the core and hands them to the encoder: as GPU textures the
    // encoder draws straight into its input where the device allows, otherwise as NV12 the GPU drew
    // and this thread copies out once (see RemotePlayFrames).
    private fun videoLoop(observer: CapturerObserver, pipeline: RemotePlayFrames) {
        var lastHandedAt = 0L
        while (streaming) {
            when (val next = pipeline.poll(50)) {
                is RemotePlayFrames.Poll.Frame -> {
                    observer.onFrameCaptured(next.frame)
                    framesHanded++
                    next.frame.release()
                    val now = SystemClock.elapsedRealtime()
                    if (lastHandedAt != 0L) peakHandGapMs.accumulateAndGet((now - lastHandedAt).toInt()) { a, b -> maxOf(a, b) }
                    lastHandedAt = now
                }
                RemotePlayFrames.Poll.Stopped -> break
                RemotePlayFrames.Poll.Timeout -> Unit
            }
        }
        observer.onCapturerStopped()
    }

    // rp-audio thread. Pulls 10 ms chunks from the core and sends them: as plain samples, a chunk to
    // a packet, or as Opus, 20 ms to a packet with the packet before riding along (see opusAudio).
    private fun audioLoop() {
        val pcm = ByteBuffer.allocateDirect(RemotePlayProtocol.AUDIO_CHUNK_FRAMES * 4).order(ByteOrder.LITTLE_ENDIAN)
        var encoder = if (opusAudio) OpusEncoder.create() else null
        Log.i(TAG, "audio: " + (encoder?.let { "Opus ${RemotePlayProtocol.OPUS_BITRATE / 1000} kbps (${it.name})" } ?: "uncompressed"))
        var seq = 0
        var previous: ByteArray? = null
        try {
            while (streaming) {
                val frames = NativeApp.remotePlayPollAudio(pcm, RemotePlayProtocol.AUDIO_CHUNK_FRAMES, 50)
                if (frames < 0) break
                if (frames == 0) continue
                val channel = audioChannel ?: break
                // Behind on the network: skip rather than build up delay.
                val behind = channel.state() != DataChannel.State.OPEN || channel.bufferedAmount() > 64 * 1024
                val opus = encoder
                if (opus == null) {
                    if (!behind) channel.send(DataChannel.Buffer(RemotePlayProtocol.encodeAudio(seq++, pcm, frames), true))
                    continue
                }
                try {
                    // A skipped packet still takes its number, so the viewer sees the gap and plays
                    // the copy in the next one.
                    opus.encode(pcm, frames) { packet ->
                        if (!behind) channel.send(DataChannel.Buffer(RemotePlayProtocol.encodeOpus(seq, packet, previous), true))
                        seq++
                        previous = packet
                    }
                } catch (e: RuntimeException) {
                    Log.w(TAG, "Opus encoder failed, sending uncompressed audio", e)
                    opus.release()
                    encoder = null
                }
            }
        } finally {
            encoder?.release()
        }
    }

    // WebRTC's thread.
    private fun onInputPacket(data: ByteBuffer) {
        if (RemotePlayProtocol.isBye(data)) {
            main.post { finish(null) }
            return
        }
        val (seq, pad) = RemotePlayProtocol.decodePad(data) ?: return
        input.post {
            if (ended || !streaming) return@post
            // The channel is unordered: a late packet must not undo a newer one.
            if (hasPadSeq && seq - lastPadSeq <= 0) return@post
            hasPadSeq = true
            lastPadSeq = seq
            lastPadAt = SystemClock.uptimeMillis()
            applyPad(pad)
        }
    }

    private fun applyPad(pad: RemotePlayProtocol.PadState) {
        val old = appliedPad
        if (pad == old) return
        val changed = pad.buttons xor old.buttons
        for (bit in RemotePlayProtocol.PAD_KEYCODES.indices) {
            val mask = 1 shl bit
            if (changed and mask != 0) {
                RemotePlayCore.setPadButton(PLAYER_2_PORT, RemotePlayProtocol.PAD_KEYCODES[bit], 0, pad.buttons and mask != 0)
            }
        }
        if (pad.l2 != old.l2) RemotePlayCore.setPadButton(PLAYER_2_PORT, KeyEvent.KEYCODE_BUTTON_L2, pad.l2 * 32767 / 255, pad.l2 > 0)
        if (pad.r2 != old.r2) RemotePlayCore.setPadButton(PLAYER_2_PORT, KeyEvent.KEYCODE_BUTTON_R2, pad.r2 * 32767 / 255, pad.r2 > 0)
        if (pad.lx != old.lx || pad.ly != old.ly) RemotePlayCore.setStick(PLAYER_2_PORT, true, pad.lx, pad.ly)
        if (pad.rx != old.rx || pad.ry != old.ry) RemotePlayCore.setStick(PLAYER_2_PORT, false, pad.rx, pad.ry)
        appliedPad = pad
    }

    private fun fail(reason: String) {
        Log.w(TAG, "host session failed: $reason")
        main.post { finish(I18n.get("remoteplay.host.failed")) }
    }

    /** Asks the host to end this session; [message] is shown when it ended on a problem. */
    private fun finish(message: String?) {
        if (ended) return
        onFinished(this, message)
    }

    /** Idempotent. */
    fun end(reason: String) {
        if (ended) return
        ended = true
        com.armsx2.DiscordPresence.leaveRemotePlay(partyId)
        input.removeCallbacksAndMessages(null)
        main.removeCallbacks(connectTimeout)
        main.removeCallbacks(answerTimeout)
        main.removeCallbacks(statsTick)
        main.removeCallbacks(hostStatsTick)
        main.removeCallbacks(prioritize)
        main.removeCallbacks(rumbleTick)
        main.removeCallbacks(askTell)
        main.removeCallbacks(askTimeout)
        asking = false
        gather?.cancel()

        val wasStreaming = streaming
        if (streaming) {
            streaming = false
            // Unblocks both loops at once: their polls return -1.
            NativeApp.remotePlayStop()
        }
        // Takes no more presses from here, so none lands after the release at the end.
        inputThread.quitSafely()
        // Player 2's rumble goes back to this device's second pad. The port keeps its pad until
        // the game closes: unplugging it would show the game a controller pulled out mid-play.
        if (wasStreaming) RemotePlayCore.hostPlayer2(false)
        wifi.release()
        upnp?.close()

        // A few times, since the channel drops what it cannot deliver.
        val why = byeReason ?: reason
        audioChannel?.takeIf { it.state() == DataChannel.State.OPEN }?.let { channel ->
            val bye = RemotePlayProtocol.byeCode(why)
            repeat(3) { channel.send(DataChannel.Buffer(RemotePlayProtocol.encodeBye(bye), true)) }
        }
        link.close(RemotePlayRtc.bye(why))

        val pc = peer
        val input = inputChannel
        val audio = audioChannel
        val track = videoTrack
        val source = videoSource
        val factory = rtc
        val video = videoThread
        val sound = audioThread
        peer = null
        audioChannel = null
        inputChannel = null
        // Every wait is on a thread of its own. The host's game takes its input through the main
        // thread, and joining the stream's threads there held it for up to 1.2 s.
        //
        // A loop still running would use what is disposed here (a freed video source, a disposed
        // channel): a crash. Both are on their way out (their polls return -1, the GL waits are
        // bounded), so they are waited for before anything goes.
        thread(name = "rp-end", isDaemon = true) {
            video?.join()
            sound?.join()
            runCatching { inputThread.join(200) }
            applyPad(RemotePlayProtocol.PadState())
            runCatching { pc?.close() }
            runCatching { input?.unregisterObserver() }
            runCatching { input?.dispose() }
            runCatching { audio?.dispose() }
            runCatching { pc?.dispose() }
            runCatching { track?.dispose() }
            runCatching { source?.dispose() }
            factory?.dispose()
            // The encoder is gone with the peer, so nothing draws the frame textures any more.
            // Taken only now: the video thread builds the pipeline, and it has ended.
            frames?.release()
            frames = null
        }
    }
}
