// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import android.content.Context
import android.view.KeyEvent

/** How an on-screen element of the viewer shows itself. Tap to reveal is the default for both. */
enum class RevealMode(val key: String) {
    TAP("tap"),
    ON("on"),
    OFF("off");

    companion object {
        fun of(key: String?) = entries.firstOrNull { it.key == key } ?: TAP
    }
}

/** The viewer's own choices, set on the Remote play screen. They only ever change the viewer. */
object RemotePlayPrefs {
    private const val FILE = "remoteplay"
    private const val KEY_EXIT = "exitButton"
    private const val KEY_TOUCH = "touchControls"
    private const val KEY_STATS = "streamStats"
    private const val KEY_QOS = "wifiQos"

    private fun prefs(context: Context) = context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    fun exitButton(context: Context) = RevealMode.of(prefs(context).getString(KEY_EXIT, null))
    fun touchControls(context: Context) = RevealMode.of(prefs(context).getString(KEY_TOUCH, null))
    fun streamStats(context: Context) = prefs(context).getBoolean(KEY_STATS, false)
    fun wifiQos(context: Context) = prefs(context).getBoolean(KEY_QOS, false)

    fun setExitButton(context: Context, mode: RevealMode) = prefs(context).edit().putString(KEY_EXIT, mode.key).apply()
    fun setTouchControls(context: Context, mode: RevealMode) = prefs(context).edit().putString(KEY_TOUCH, mode.key).apply()
    fun setStreamStats(context: Context, on: Boolean) = prefs(context).edit().putBoolean(KEY_STATS, on).apply()
    fun setWifiQos(context: Context, on: Boolean) = prefs(context).edit().putBoolean(KEY_QOS, on).apply()
}

/**
 * Player 2's pad as the on-screen controls hold it. The controls are ARMSX2's own
 * (com.armsx2.ui.touch.RemotePlayTouchControls), and they send what they would send a game on this
 * device: Android keycodes for buttons, and pairs of half-axis codes carrying 0..32767 for each
 * stick. Main thread, like the controls' touch handlers.
 */
internal class TouchPadState(private val onChange: (RemotePlayProtocol.PadState) -> Unit) {
    private var buttons = 0
    private var l2 = false
    private var r2 = false

    // Half-axes in STICK_CODES order: left x+, x-, y+, y-, then the right stick's.
    private val half = IntArray(STICK_CODES.size)

    fun onKey(keycode: Int, value: Int, pressed: Boolean) {
        val axis = STICK_CODES.indexOf(keycode)
        when {
            axis >= 0 -> half[axis] = if (pressed) value.coerceIn(0, AXIS_MAX) else 0
            keycode == KeyEvent.KEYCODE_BUTTON_L2 -> l2 = pressed
            keycode == KeyEvent.KEYCODE_BUTTON_R2 -> r2 = pressed
            else -> {
                val bit = BUTTON_BITS[keycode] ?: return
                buttons = if (pressed) buttons or bit else buttons and bit.inv()
            }
        }
        onChange(state())
    }

    /** Lets go of everything, for when the controls hide. */
    fun clear() {
        buttons = 0
        l2 = false
        r2 = false
        half.fill(0)
        onChange(state())
    }

    private fun state() = RemotePlayProtocol.PadState(
        buttons = buttons,
        lx = axis(half[0], half[1]),
        ly = axis(half[2], half[3]),
        rx = axis(half[4], half[5]),
        ry = axis(half[6], half[7]),
        l2 = if (l2) 255 else 0,
        r2 = if (r2) 255 else 0,
    )

    private fun axis(positive: Int, negative: Int) = RemotePlayProtocol.axisByte((positive - negative) / AXIS_MAX.toFloat())

    private companion object {
        const val AXIS_MAX = 32767

        // The touch controls' stick codes (StickCodes in TouchControlsOverlay): x+, x-, y+, y-.
        val STICK_CODES = intArrayOf(111, 113, 112, 110, 121, 123, 122, 120)

        val BUTTON_BITS = mapOf(
            KeyEvent.KEYCODE_DPAD_UP to RemotePlayProtocol.PAD_UP,
            KeyEvent.KEYCODE_DPAD_RIGHT to RemotePlayProtocol.PAD_RIGHT,
            KeyEvent.KEYCODE_DPAD_DOWN to RemotePlayProtocol.PAD_DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT to RemotePlayProtocol.PAD_LEFT,
            KeyEvent.KEYCODE_BUTTON_SELECT to RemotePlayProtocol.PAD_SELECT,
            KeyEvent.KEYCODE_BUTTON_START to RemotePlayProtocol.PAD_START,
            KeyEvent.KEYCODE_BUTTON_THUMBL to RemotePlayProtocol.PAD_L3,
            KeyEvent.KEYCODE_BUTTON_THUMBR to RemotePlayProtocol.PAD_R3,
            KeyEvent.KEYCODE_BUTTON_Y to RemotePlayProtocol.PAD_TRIANGLE,
            KeyEvent.KEYCODE_BUTTON_B to RemotePlayProtocol.PAD_CIRCLE,
            KeyEvent.KEYCODE_BUTTON_A to RemotePlayProtocol.PAD_CROSS,
            KeyEvent.KEYCODE_BUTTON_X to RemotePlayProtocol.PAD_SQUARE,
            KeyEvent.KEYCODE_BUTTON_L1 to RemotePlayProtocol.PAD_L1,
            KeyEvent.KEYCODE_BUTTON_R1 to RemotePlayProtocol.PAD_R1,
        )
    }
}
