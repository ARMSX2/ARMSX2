package com.armsx2.ui

import android.app.Activity
import androidx.compose.runtime.mutableStateOf
import com.armsx2.runtime.MainActivityRuntime

/**
 * Opt-in screen pinning, so a stray controller Home button can't dump you out of a game (#425).
 *
 * The GameSir G8+ "GS" button — and the equivalent on several other pads — is wired to Android's
 * HOME key rather than to `KEYCODE_BUTTON_MODE`. That distinction is the whole problem: HOME is
 * consumed by the system in `PhoneWindowManager.interceptKeyBeforeDispatching` and is **never**
 * delivered to a normal app, so there is nothing for ARMSX2 to intercept, swallow, or rebind. Any
 * pad button that does reach us is already bindable — `labelForKey` falls through to
 * `KeyEvent.keyCodeToString`, so even exotic keycodes work today. HOME simply never arrives.
 *
 * Screen pinning is the one documented API that actually stops it. `Activity.startLockTask()` on
 * an app that is not a device owner enters the user-confirmed "Pin screen?" flow, and while pinned
 * the system itself blocks HOME and Recents. That is a real behavioural change to the whole device
 * UI, so it is strictly opt-in and off by default; unpin the normal way (hold Back + Recents, or
 * Back + Home) or just end the pinned session by leaving the game.
 */
object ScreenPinning {
    private const val KEY = "ui.blockHomeButton"

    val enabled = mutableStateOf(false)

    /** True while we actually hold a pinned session, so we only stop what we started. */
    private var pinned = false

    fun load() {
        enabled.value = runCatching { MainActivityRuntime.prefs.getBoolean(KEY, false) }
            .getOrDefault(false)
    }

    fun set(on: Boolean) {
        enabled.value = on
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(KEY, on).apply() }
        // Take effect immediately rather than at the next game launch — the user almost certainly
        // just got kicked out of a game and is turning this on to stop it happening again.
        if (!on) stop()
    }

    /** Called when emulation starts. No-op unless the user opted in. */
    fun start(activity: Activity) {
        if (!enabled.value || pinned) return
        // Throws IllegalStateException if the activity isn't resumed, and is a no-op on devices
        // where pinning is disabled by policy — neither is worth interrupting a game launch for.
        runCatching {
            activity.startLockTask()
            pinned = true
        }
    }

    /** Called when returning to the library, so the device isn't left pinned outside a game. */
    fun stop() {
        if (!pinned) return
        pinned = false
        runCatching { MainActivityRuntime.instance?.stopLockTask() }
    }
}
