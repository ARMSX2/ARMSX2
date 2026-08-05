package com.armsx2

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import com.armsx2.i18n.I18n
import com.armsx2.ui.WelcomeBanner

/**
 * Warns once when the battery gets low or the pack gets hot during a session (requested by
 * BrianAR20: "low battery notification below 20%, and when battery temperature is above 35 C").
 *
 * Reads the sticky ACTION_BATTERY_CHANGED broadcast — no polling loop and no extra wakeups; the
 * system already sends this on every meaningful change. Warnings are EDGE-triggered with hysteresis
 * so a value hovering on the threshold cannot spam the banner: each warning arms only after the
 * reading has recovered past a margin. Charging suppresses the low-battery warning (it's rising,
 * not a problem), but NOT the temperature warning — charging while playing is exactly when a device
 * gets hottest.
 *
 * Shown through [WelcomeBanner], the transient in-game banner hosted at the Compose root, so it
 * reaches the player over a running game as well as in the library.
 */
object BatteryWatcher {
    /** Warn at or below this charge percentage. */
    private const val LOW_PCT = 20
    /** Re-arm the low warning once charge recovers above this (hysteresis). */
    private const val LOW_REARM_PCT = 25
    /** Warn at or above this pack temperature, in whole degrees Celsius. */
    private const val HOT_C = 35
    /** Re-arm the heat warning once it cools below this. */
    private const val HOT_REARM_C = 33

    private const val PREF_KEY = "battery.warnings"

    private var lowWarned = false
    private var hotWarned = false
    private var receiver: BroadcastReceiver? = null

    /** Master switch (App settings). Default on; persisted like the other app-level toggles. */
    val enabled = androidx.compose.runtime.mutableStateOf(true)

    fun load() {
        runCatching {
            enabled.value = com.armsx2.runtime.MainActivityRuntime.prefs.getBoolean(PREF_KEY, true)
        }
    }

    fun set(value: Boolean) {
        enabled.value = value
        runCatching {
            com.armsx2.runtime.MainActivityRuntime.prefs.edit().putBoolean(PREF_KEY, value).apply()
        }
    }

    fun start(context: Context) {
        if (receiver != null) return
        val r = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context?, intent: Intent?) {
                if (intent == null) return
                runCatching { handle(intent) }
            }
        }
        // Sticky broadcast: registering immediately delivers the current state, so a session that
        // starts already low/hot warns right away instead of waiting for the next change.
        runCatching { context.applicationContext.registerReceiver(r, IntentFilter(Intent.ACTION_BATTERY_CHANGED)) }
            .onSuccess { receiver = r }
    }

    fun stop(context: Context) {
        receiver?.let { r -> runCatching { context.applicationContext.unregisterReceiver(r) } }
        receiver = null
    }

    /** Forget both warnings, so a new game session can warn again. */
    fun resetForNewSession() {
        lowWarned = false
        hotWarned = false
    }

    private fun handle(intent: Intent) {
        if (!enabled.value) return
        val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
        val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
        val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
            status == BatteryManager.BATTERY_STATUS_FULL
        // EXTRA_TEMPERATURE is TENTHS of a degree Celsius (e.g. 352 = 35.2 C).
        val tempTenths = intent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)

        if (level in 0..Int.MAX_VALUE && scale > 0) {
            val pct = level * 100 / scale
            when {
                !charging && pct <= LOW_PCT && !lowWarned -> {
                    lowWarned = true
                    WelcomeBanner.show(I18n.get("battery.low.warn").format(pct))
                }
                // Recovered (or plugged in): allow the next dip to warn again.
                pct >= LOW_REARM_PCT || charging -> lowWarned = false
            }
        }

        if (tempTenths != Int.MIN_VALUE) {
            val celsius = tempTenths / 10
            when {
                celsius >= HOT_C && !hotWarned -> {
                    hotWarned = true
                    WelcomeBanner.show(I18n.get("battery.hot.warn").format(celsius))
                }
                celsius <= HOT_REARM_C -> hotWarned = false
            }
        }
    }
}
