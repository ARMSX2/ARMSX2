package com.armsx2.discord

import android.app.Service
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.util.Log

/**
 * The Discord helper, running in its own process (:discord, see AndroidManifest).
 *
 * Why a whole process for this: ARMSX2 is GPL-3.0+ and the Discord Social SDK is proprietary with
 * no published licence. Linking them into one binary would make the emulator a combined work with a
 * library whose source cannot be supplied. Two programs exchanging messages is a different thing
 * entirely, and the process boundary is what makes it true rather than merely claimed — the SDK is
 * loaded here and nowhere else, and libemucore has no reference to it at all.
 *
 * Everything the SDK does happens on this process's main thread, which is also the thread that
 * creates the client, because the SDK delivers its callbacks only to that thread. The app process
 * polls for state rather than being pushed to, which keeps the protocol one-directional and means
 * a dead helper degrades to "nothing new" instead of a hang.
 */
class DiscordService : Service() {
    private companion object {
        const val TAG = "ARMSX2DiscordSvc"
        // The SDK's callback queue wants draining often; the pump is cheap when idle.
        const val PUMP_INTERVAL_MS = 50L
    }

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var messenger: Messenger

    private var started = false
    private var loaded = false

    private val pump = object : Runnable {
        override fun run() {
            if (loaded && started) {
                runCatching { DiscordNative.pump() }
                    .onFailure { Log.w(TAG, "pump failed: ${it.message}") }
            }
            handler.postDelayed(this, PUMP_INTERVAL_MS)
        }
    }

    override fun onCreate() {
        super.onCreate()
        loaded = DiscordNative.load()
        Log.i(TAG, "helper process up (native loaded=$loaded)")
        messenger = Messenger(Handler(Looper.getMainLooper()) { msg -> handle(msg); true })
        handler.post(pump)
    }

    override fun onBind(intent: Intent?): IBinder = messenger.binder

    override fun onDestroy() {
        handler.removeCallbacks(pump)
        if (loaded && started) runCatching { DiscordNative.stop() }
        super.onDestroy()
    }

    private fun handle(msg: Message) {
        when (msg.what) {
            DiscordIpc.MSG_START -> {
                if (!loaded) return
                val token = msg.data?.getString(DiscordIpc.DATA_TOKEN).orEmpty()
                runCatching { DiscordNative.start(token) }
                    .onSuccess { started = true }
                    .onFailure { Log.w(TAG, "start failed: ${it.message}") }
            }

            DiscordIpc.MSG_AUTHORIZE -> {
                if (!loaded) return
                // The SDK opens the browser through an Activity it has been handed, and that
                // Activity has to live in THIS process — one in the app process would be a
                // reference the SDK here cannot use. DiscordAuthActivity exists only to be that.
                runCatching {
                    val i = Intent(this, DiscordAuthActivity::class.java)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    startActivity(i)
                }.onFailure { Log.w(TAG, "auth activity failed: ${it.message}") }
            }

            DiscordIpc.MSG_SET_PLAYING -> {
                if (!loaded || !started) return
                val d = msg.data ?: Bundle.EMPTY
                runCatching {
                    DiscordNative.setPlaying(
                        d.getString(DiscordIpc.DATA_SERIAL).orEmpty(),
                        d.getString(DiscordIpc.DATA_TITLE).orEmpty(),
                        d.getString(DiscordIpc.DATA_COVER).orEmpty(),
                    )
                }
            }

            DiscordIpc.MSG_STOP -> {
                if (!loaded) return
                runCatching { DiscordNative.stop() }
                started = false
            }

            DiscordIpc.MSG_QUERY -> reply(msg.replyTo)
        }
    }

    /** One snapshot, on request. Everything the app's UI renders comes through here. */
    private fun reply(to: Messenger?) {
        val target = to ?: return
        val data = Bundle().apply {
            putBoolean(
                DiscordIpc.DATA_AVAILABLE,
                loaded && runCatching { DiscordNative.available() }.getOrDefault(false),
            )
            if (loaded && started) {
                putInt(DiscordIpc.DATA_STATUS, runCatching { DiscordNative.status() }.getOrDefault(0))
                putString(DiscordIpc.DATA_FRIENDS, runCatching { DiscordNative.friends() }.getOrDefault(""))
                putString(DiscordIpc.DATA_SELF, runCatching { DiscordNative.self() }.getOrDefault(""))
                putString(DiscordIpc.DATA_ERROR, runCatching { DiscordNative.error() }.getOrNull())
                // Surfaces exactly once, right after a successful sign-in; the app persists it so
                // the browser is a one-time cost rather than a per-launch one.
                putString(DiscordIpc.DATA_FRESH_TOKEN, runCatching { DiscordNative.takeToken() }.getOrDefault(""))
            }
        }
        runCatching {
            target.send(Message.obtain(null, DiscordIpc.MSG_STATE).apply { this.data = data })
        }.onFailure {
            // The app process went away mid-reply. Not an error worth logging every second.
        }
    }
}
