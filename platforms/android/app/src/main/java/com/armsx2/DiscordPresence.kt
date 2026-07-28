package com.armsx2

import android.app.Activity
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
import kr.co.iefriends.pcsx2.NativeApp

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
object DiscordPresence {
    private const val TAG = "DiscordPresence"
    private const val PREF_ENABLED = "discord.enabled"
    private const val PREF_TOKEN = "discord.token"

    // Mirrors BridgeStatus in cpp/discord_bridge.cpp.
    const val DISABLED = 0
    const val DISCONNECTED = 1
    const val AUTHORIZING = 2
    const val CONNECTING = 3
    const val CONNECTED = 4
    const val FAILED = 5

    val status = mutableStateOf(DISABLED)
    val friends = mutableStateOf<List<String>>(emptyList())
    val error = mutableStateOf<String?>(null)

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var pollJob: Job? = null
    private var presenceJob: Job? = null
    private var started = false

    /** False when the SDK was not staged at build time — the UI hides the whole section. */
    fun available(): Boolean = runCatching { NativeApp.discordAvailable() }.getOrDefault(false)

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

    // Held weakly: this is the Activity, and the object outlives it.
    private var activityRef: java.lang.ref.WeakReference<Activity>? = null
    private var engineActivitySet = false

    /**
     * Remember the Activity. Deliberately does NOT touch the SDK.
     *
     * Touching any com.discord class runs DiscordSocialSdkInit's static initializer, which
     * System.loadLibrary's the SDK and runs its JNI_OnLoad — and that path ABORTS THE PROCESS on
     * any problem rather than throwing. It is a native abort inside a static initializer, so
     * runCatching cannot catch it and the app simply dies at boot; a missing proguard keep rule
     * did exactly that. An opt-in feature must not be able to kill someone who never opted in, so
     * the SDK is not loaded at all until the user asks for it.
     */
    fun attachActivity(activity: Activity) {
        activityRef = java.lang.ref.WeakReference(activity)
    }

    /** Actually hand the Activity over. Only ever called once the user has opted in. */
    private fun bindEngineActivity() {
        if (engineActivitySet) return
        val activity = activityRef?.get() ?: return
        runCatching { com.discord.socialsdk.DiscordSocialSdkInit.setEngineActivity(activity) }
            .onSuccess { engineActivitySet = true }
            .onFailure { Log.w(TAG, "setEngineActivity failed: ${it.message}") }
    }

    /** Bring the client up, reusing a stored token when there is one so sign-in is once, not daily. */
    fun start() {
        if (!available() || !enabled || started) return
        started = true
        bindEngineActivity()
        runCatching { NativeApp.discordStart(savedToken) }
            .onFailure { Log.w(TAG, "discordStart failed: ${it.message}"); started = false; return }
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
        bindEngineActivity()
        error.value = null
        runCatching { NativeApp.discordAuthorize() }
            .onFailure { Log.w(TAG, "discordAuthorize failed: ${it.message}") }
    }

    fun signOut() {
        pollJob?.cancel(); pollJob = null
        presenceJob?.cancel(); presenceJob = null
        started = false
        savedToken = ""
        runCatching { NativeApp.discordStop() }
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
            while (true) {
                val s = runCatching { NativeApp.discordStatus() }.getOrDefault(DISABLED)
                status.value = s

                // The token surfaces exactly once, right after a successful sign-in. Persisting it
                // here is what makes the browser a one-time cost instead of every launch.
                runCatching { NativeApp.discordTakeToken() }.getOrNull()?.let { fresh ->
                    if (fresh.isNotBlank()) {
                        savedToken = fresh
                        Log.i(TAG, "authorization stored")
                    }
                }

                friends.value = if (s == CONNECTED) {
                    runCatching { NativeApp.discordFriends() }.getOrDefault("")
                        .split('\n').filter { it.isNotBlank() }
                } else {
                    emptyList()
                }

                error.value = if (s == FAILED) {
                    runCatching { NativeApp.discordError() }.getOrNull()
                } else {
                    null
                }

                delay(if (s == AUTHORIZING || s == CONNECTING) 300L else 1_000L)
            }
        }
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
            snapshotFlow { MainActivityRuntime.currentGame.value }.collect { game ->
                val serial = game?.serial.orEmpty()
                val title = game?.let { it.displayTitle(false) }.orEmpty()
                runCatching { NativeApp.discordSetPlaying(serial, title) }
            }
        }
    }
}
