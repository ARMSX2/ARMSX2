package com.armsx2

import android.app.Activity
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.util.Log
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.snapshotFlow
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import com.armsx2.discord.DiscordIpc

/**
 * Discord rich presence, and which friends are in ARMSX2 right now.
 *
 * Discord carries the entire social graph — the friendships, the online status, and the "is this
 * person in the same game" grouping are all theirs. That is the whole point of doing it this way:
 * no account system, no friend database, no server, and nothing of the user's kept anywhere by us
 * except an OAuth token in this app's own prefs.
 *
 * Foreground only, by construction. The SDK stops when the app does, so friend changes are seen
 * while ARMSX2 is open and not otherwise. Background delivery would mean push infrastructure, which
 * is exactly the cost this design exists to avoid.
 *
 * Opt-in and off by default: it is an account link, so nothing happens until the user asks.
 */
/**
 * A Discord friend who has ARMSX2 open.
 *
 * [game] and [serial] come from the rich presence they are publishing — which this same object
 * wrote on their device — so "what are they playing" needs no lookup service. A friend sitting in
 * the library publishes neither, which is what [inLibrary] keys off.
 */
data class DiscordFriend(
    val name: String,
    val game: String,
    val serial: String,
    val avatarUrl: String,
) {
    /** In ARMSX2 but not in a game. Distinct from being in a game we could not name. */
    val inLibrary: Boolean get() = game.isBlank() && serial.isBlank()

    /**
     * Cover art for what they are playing, resolved the same way the library resolves its own.
     *
     * Derived from the serial here rather than read out of their presence assets: Discord may hand
     * back a proxied form of an image URL, and re-deriving keeps a friend's row looking exactly like
     * the same game does on the home screen — including honouring the 2D/3D preference, which is
     * this device's choice to make, not theirs.
     */
    val coverUrl: String? get() = serial.takeIf { it.isNotBlank() }?.let { s ->
        if (CoverArtStyle.use3d.value)
            "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/3d/$s.png"
        else
            "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/$s.jpg"
    }
}

object DiscordPresence {
    private const val TAG = "DiscordPresence"
    private const val PREF_ENABLED = "discord.enabled"
    private const val PREF_TOKEN = "discord.token"
    private const val PREF_NOTIFY = "discord.notifyInGame"

    // Mirrors BridgeStatus in cpp/discord_bridge.cpp.
    const val DISABLED = 0
    const val DISCONNECTED = 1
    const val AUTHORIZING = 2
    const val CONNECTING = 3
    const val CONNECTED = 4
    const val FAILED = 5

    val status = mutableStateOf(DISABLED)
    val friends = mutableStateOf<List<DiscordFriend>>(emptyList())
    val error = mutableStateOf<String?>(null)

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var pollJob: Job? = null
    private var presenceJob: Job? = null
    private var started = false

    // ---- helper-process link -------------------------------------------------------------
    //
    // The Discord SDK runs in :discord, not here. That is a licensing boundary: ARMSX2 is GPL-3.0+
    // and the SDK is proprietary, so they are two programs exchanging messages rather than one
    // linked binary. Nothing in this file may reference a com.discord class or DiscordNative —
    // both would fail to resolve in this process, which is exactly the intended guarantee.

    private var helper: Messenger? = null
    private var bound = false

    /** Last snapshot the helper sent. Read by the poll loop; written on the main thread. */
    private var lastState: Bundle? = null

    private val incoming = Messenger(Handler(Looper.getMainLooper()) { msg ->
        if (msg.what == DiscordIpc.MSG_STATE) lastState = msg.data
        true
    })

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            helper = service?.let { Messenger(it) }
            Log.i(TAG, "helper process connected")
            // Re-issue whatever the helper needs to know: a reconnect means a fresh process that
            // remembers nothing, so anything set before the bind has to be replayed.
            send(DiscordIpc.MSG_START, Bundle().apply { putString(DiscordIpc.DATA_TOKEN, savedToken) })
            pushPresence()
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            // The helper died — a crash in the SDK, or the system reclaiming it. Nothing here is
            // lost: rebinding replays the token and the presence.
            Log.w(TAG, "helper process disconnected")
            helper = null
        }
    }

    private fun bindHelper() {
        if (bound) return
        val ctx = MainActivityRuntime.instance?.applicationContext ?: return
        val intent = Intent(ctx, com.armsx2.discord.DiscordService::class.java)
        bound = runCatching {
            ctx.bindService(intent, connection, Context.BIND_AUTO_CREATE)
        }.getOrDefault(false)
        if (!bound) Log.w(TAG, "could not bind the Discord helper")
    }

    /**
     * Drop the binding, which is what actually ends the helper.
     *
     * The service is bound-only and never started, so the last unbind destroys the process — and
     * with it every trace of the Discord SDK. That is the property worth preserving: with the
     * feature off, the proprietary library is not merely idle, it is not loaded at all.
     */
    private fun unbindHelper() {
        val ctx = MainActivityRuntime.instance?.applicationContext
        if (bound && ctx != null) runCatching { ctx.unbindService(connection) }
        bound = false
        helper = null
        lastState = null
    }

    private fun send(what: Int, data: Bundle? = null) {
        val target = helper ?: return
        runCatching {
            target.send(Message.obtain(null, what).apply {
                if (data != null) this.data = data
                replyTo = incoming
            })
        }.onFailure { Log.w(TAG, "ipc send($what) failed: ${it.message}") }
    }

    /** Re-publish the current game; used on (re)connect and whenever the running game changes. */
    private fun pushPresence() {
        val game = MainActivityRuntime.currentGame.value
        send(DiscordIpc.MSG_SET_PLAYING, Bundle().apply {
            putString(DiscordIpc.DATA_SERIAL, game?.serial.orEmpty())
            putString(DiscordIpc.DATA_TITLE, game?.let { it.displayTitle(false) }.orEmpty())
            putString(DiscordIpc.DATA_COVER, game?.coverUrl.orEmpty())
        })
    }

    /**
     * False when the SDK was not staged at build time — the UI hides the whole section.
     *
     * Answered by the helper process, so it is unknown until the first snapshot lands. Defaults to
     * true meanwhile: hiding the feature and then revealing it a second later looks broken, while
     * the reverse is invisible on a build that has the SDK.
     */
    private var helperAvailable = true

    fun available(): Boolean = helperAvailable

    var enabled: Boolean
        get() = available() && runCatching {
            MainActivityRuntime.prefs.getBoolean(PREF_ENABLED, false)
        }.getOrDefault(false)
        set(value) {
            runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_ENABLED, value).apply() }
            if (value) start() else signOut()
        }

    private var savedToken: String
        get() = runCatching { MainActivityRuntime.prefs.getString(PREF_TOKEN, "") ?: "" }.getOrDefault("")
        set(value) {
            runCatching { MainActivityRuntime.prefs.edit().putString(PREF_TOKEN, value).apply() }
        }

    /** Announce on the in-game OSD when a friend joins. Default on: it is the point of the feature. */
    var notifyInGame: Boolean
        get() = runCatching { MainActivityRuntime.prefs.getBoolean(PREF_NOTIFY, true) }.getOrDefault(true)
        set(value) {
            runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_NOTIFY, value).apply() }
            notifyState.value = value
        }

    /** Mirror of [notifyInGame] for Compose, which cannot observe SharedPreferences. */
    val notifyState = mutableStateOf(true)

    // Who we had last poll, so a friend appearing can be told apart from a friend still being here.
    private var knownFriends: Set<String> = emptySet()

    // Whether the first poll after connecting has been absorbed.
    //
    // An explicit flag, NOT "is the previous set empty". Inferring it was a real bug: connect with
    // nobody online and the very next arrival looks identical to the seed poll — empty before,
    // non-empty now — so the one notification most worth showing was the one always swallowed.
    private var seededFriends = false

    /** Friend who just came online, for the library banner to show. Cleared once it has been seen. */
    val justOnline = mutableStateOf<DiscordFriend?>(null)

    /** The signed-in Discord account: name and avatar. Null until connected. */
    val self = mutableStateOf<DiscordFriend?>(null)

    private const val FIELD_SEP = DiscordIpc.FIELD_SEP
    private const val RECORD_SEP = DiscordIpc.RECORD_SEP

    /**
     * Kept for call-site compatibility; there is nothing to bind any more.
     *
     * The SDK used to need ARMSX2's Activity to launch sign-in. It now lives in :discord and uses
     * DiscordAuthActivity over there, so handing it an Activity from this process would be handing
     * it a reference from the wrong process. Deliberately a no-op rather than deleted: the callers
     * are lifecycle hooks, and a missing one would be silent.
     */
    fun attachActivity(activity: Activity) {
        // Binding early means the helper is warm before anyone opens the Friends screen.
        if (enabled) bindHelper()
    }

    /** Bring the client up, reusing a stored token when there is one so sign-in is once, not daily. */
    fun start() {
        if (!available() || !enabled || started) return
        started = true
        notifyState.value = notifyInGame
        bindHelper()
        // Send it here TOO, not only from onServiceConnected.
        //
        // onServiceConnected fires once per binding. After a Disconnect the binding is still up, so
        // signing back in never produced a second callback — the helper kept the torn-down client
        // from MSG_STOP, Connect sent an authorize into it, and nothing happened until the app was
        // restarted and the bind was fresh. MSG_START is idempotent in the bridge (it only creates
        // a client when there isn't one), so sending it on both paths is free.
        //
        // A no-op when the bind is still in flight: send() drops it, and onServiceConnected covers
        // that case a moment later.
        send(DiscordIpc.MSG_START, Bundle().apply { putString(DiscordIpc.DATA_TOKEN, savedToken) })
        startPolling()
        startPresenceWatch()
    }

    fun authorize() {
        if (!available()) return
        // Authorizing implies enabling: someone pressing Connect has opted in, and requiring the
        // toggle first would just be a second thing to press.
        if (!enabled) {
            runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_ENABLED, true).apply() }
        }
        if (!started) start()
        bindHelper()
        error.value = null
        Log.i(TAG, "authorize() requested (started=$started, enabled=$enabled)")
        send(DiscordIpc.MSG_AUTHORIZE)
    }

    fun signOut() {
        pollJob?.cancel(); pollJob = null
        presenceJob?.cancel(); presenceJob = null
        started = false
        savedToken = ""
        knownFriends = emptySet()
        seededFriends = false
        justOnline.value = null
        self.value = null
        send(DiscordIpc.MSG_STOP)
        // Then let go of the helper entirely, so the next Connect starts from a clean process
        // rather than reusing one whose client has just been destroyed.
        unbindHelper()
        status.value = if (available()) DISCONNECTED else DISABLED
        friends.value = emptyList()
        error.value = null
    }

    /**
     * Poll the bridge for status and friends.
     *
     * Polling rather than callbacks: the SDK fires on its own threads, so pushing into Kotlin would
     * need AttachCurrentThread and a global ref outliving the Activity. A one-second poll of two
     * cheap accessors costs nothing next to that, and this only runs while signed in.
     */
    private fun startPolling() {
        pollJob?.cancel()
        pollJob = scope.launch {
            var sinceRetry = 0
            var retryAfter = 5
            while (true) {
                // No pump here any more: the SDK's callback queue is drained inside the helper, on
                // the thread that created the client. This loop only asks for a snapshot.
                bindHelper()
                send(DiscordIpc.MSG_QUERY)
                delay(1000)

                val snap = lastState
                if (snap == null) {
                    // Helper not answering yet. Say "connecting" rather than "failed" — a bind
                    // takes a moment and the SDK is not the thing that is slow.
                    if (enabled && status.value == DISABLED) status.value = CONNECTING
                    continue
                }

                helperAvailable = snap.getBoolean(DiscordIpc.DATA_AVAILABLE, true)
                val s = snap.getInt(DiscordIpc.DATA_STATUS, DISABLED)
                status.value = s

                snap.getString(DiscordIpc.DATA_FRESH_TOKEN)?.takeIf { it.isNotBlank() }?.let { fresh ->
                    savedToken = fresh
                    Log.i(TAG, "authorization stored")
                }

                // Reconnect after a drop. The SDK does not retry, and its connection does not
                // survive the app being backgrounded for long — swapping apps, or a device
                // sleeping. MSG_START is idempotent in the helper, so it doubles as the retry.
                if (s == DISCONNECTED && enabled && savedToken.isNotBlank()) {
                    if (++sinceRetry >= retryAfter) {
                        sinceRetry = 0
                        retryAfter = (retryAfter * 2).coerceAtMost(60)
                        Log.i(TAG, "connection dropped — reconnecting (next retry in $retryAfter s)")
                        send(DiscordIpc.MSG_START, Bundle().apply {
                            putString(DiscordIpc.DATA_TOKEN, savedToken)
                        })
                    }
                } else if (s == CONNECTED) {
                    sinceRetry = 0
                    retryAfter = 5
                }

                // Only touch the friends list while actually connected. A drop reports nobody
                // online, and letting that through would clear the known set — so reconnecting
                // would then announce every friend as a fresh arrival.
                if (s == CONNECTED) {
                    val current = parseFriends(snap.getString(DiscordIpc.DATA_FRIENDS).orEmpty())
                    announceArrivals(current)
                    friends.value = current

                    if (self.value == null) {
                        val f = snap.getString(DiscordIpc.DATA_SELF).orEmpty().split(FIELD_SEP)
                        val name = f.getOrNull(0).orEmpty()
                        if (name.isNotBlank()) {
                            self.value = DiscordFriend(
                                name = name,
                                game = "",
                                serial = "",
                                avatarUrl = f.getOrNull(1).orEmpty(),
                            )
                        }
                    }
                }

                error.value = if (s == FAILED) snap.getString(DiscordIpc.DATA_ERROR) else null
            }
        }
    }

    /**
     * Tell the player when somebody joins, on the emulator's own OSD so it lands over the game
     * rather than requiring them to be looking at the Friends screen.
     *
     * Only ARRIVALS: diffing against the previous set means a friend who is simply still online
     * does not re-announce every second. Departures are deliberately silent — someone quitting is
     * not news worth covering the game for.
     *
     * The first poll after connecting seeds the set without announcing. Otherwise signing in would
     * dump one notification per friend already playing, which is a wall, not an alert.
     */
    private fun parseFriends(raw: String): List<DiscordFriend> {
        if (raw.isBlank()) return emptyList()
        return raw.split(RECORD_SEP).mapNotNull { record ->
            val f = record.split(FIELD_SEP)
            val name = f.getOrNull(0).orEmpty()
            if (name.isBlank()) return@mapNotNull null
            DiscordFriend(
                name = name,
                game = f.getOrNull(1).orEmpty(),
                serial = f.getOrNull(2).orEmpty(),
                avatarUrl = f.getOrNull(3).orEmpty(),
            )
        }
    }

    private fun announceArrivals(current: List<DiscordFriend>) {
        val currentSet = current.map { it.name }.toSet()
        val previous = knownFriends
        knownFriends = currentSet

        // Absorb the first poll after connecting without announcing: signing in should not dump one
        // notification per friend already playing.
        if (!seededFriends) {
            seededFriends = true
            return
        }
        if (!notifyInGame) return

        val arrivals = current.filterNot { it.name in previous }
        if (arrivals.isEmpty()) return

        // One surface for both cases. This used to branch: emulator OSD in a game, Compose banner
        // in the library. The OSD is text-only, so it could not show an avatar and looked nothing
        // like the library's version of the same event. The banner is composed at the Activity
        // root, so it draws over the game too and there is no reason to keep a second path.
        //
        // Last arrival wins if several land at once; a stack of banners over a game would be worse
        // than the one that is actually current.
        arrivals.forEach { friend -> justOnline.value = friend }
    }

    /**
     * Publish whatever is running.
     *
     * Watches [MainActivityRuntime.currentGame] instead of being called from the launch paths —
     * there are five places that assign it, and a snapshot flow catches every one of them, plus any
     * added later. A missed call site here would be invisible: presence would simply be stale.
     */
    private fun startPresenceWatch() {
        presenceJob?.cancel()
        presenceJob = scope.launch {
            // pushPresence reads currentGame itself, so this only needs to know it changed. The
            // cover it sends is the same URL the library renders — Discord takes a URL, so there
            // is nothing to upload per game and a title with no published cover falls back.
            snapshotFlow { MainActivityRuntime.currentGame.value }.collect { pushPresence() }
        }
    }
}
