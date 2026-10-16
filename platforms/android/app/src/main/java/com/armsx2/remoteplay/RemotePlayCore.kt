// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import kotlin.concurrent.thread
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Remote play's view of the emulator on ARMSX2: player 2's pad, rumble and the paused state. The
 * streaming code came from ARMSX3, where these calls went to its own core; here they go to PCSX2's
 * pad through the same entry points local co-op uses ([NativeApp.setPadButtonForPort], port 1).
 */
internal object RemotePlayCore {
    // The direction keys applyPadButton (cpp/native-lib.cpp) takes for each stick, with their
    // deflection as the range (0 to 32767): up, right, down, left.
    private val LEFT_STICK = intArrayOf(110, 111, 112, 113)
    private val RIGHT_STICK = intArrayOf(120, 121, 122, 123)

    /** A button, or a trigger with its pressure in [value] (0 to 32767, 0 for a plain press). */
    fun setPadButton(port: Int, keycode: Int, value: Int, pressed: Boolean) {
        NativeApp.setPadButtonForPort(port, keycode, value, pressed)
    }

    /** A stick, as the remote play pad packet carries it: 0 to 255 per axis, 128 at rest. */
    fun setStick(port: Int, left: Boolean, x: Int, y: Int) {
        val keys = if (left) LEFT_STICK else RIGHT_STICK
        axis(port, negative = keys[3], positive = keys[1], value = x)
        axis(port, negative = keys[0], positive = keys[2], value = y)
    }

    // One axis as its two opposing direction keys. A press with range 0 means a full press to
    // applyPadButton, so a deflection too small to round up is sent as a release, not a press.
    private fun axis(port: Int, negative: Int, positive: Int, value: Int) {
        val d = value.coerceIn(0, 255) - 128
        val n = if (d < 0) d / 128f else d / 127f
        val range = (abs(n) * 32767f).roundToInt().coerceIn(0, 32767)
        NativeApp.setPadButtonForPort(port, positive, if (n > 0f) range else 0, n > 0f && range > 0)
        NativeApp.setPadButtonForPort(port, negative, if (n < 0f) range else 0, n < 0f && range > 0)
    }

    /**
     * Host: player 2 is now on the friend's device. Its rumble goes to them (NativeApp.onPadRumble),
     * and PS2 port 2 gets a DualShock 2 if it had none, which pauses the game for a moment, so off
     * the main thread.
     */
    fun hostPlayer2(on: Boolean) {
        NativeApp.sRemotePlayer2 = on
        NativeApp.sRemotePlayer2Rumble = 0
        if (on) thread(name = "rp-pad2", isDaemon = true) { runCatching { NativeApp.enablePad2() } }
    }

    /**
     * Host: the running game's name, or null when no game is running. The library's name when the
     * game was opened from there; the core's otherwise, since a game booted from a file or a disc
     * swap leaves no library entry behind.
     */
    fun runningGame(): String? {
        if (!runCatching { NativeApp.hasActiveVM() }.getOrDefault(false)) return null
        return MainActivityRuntime.currentGame.value?.title?.takeIf { it.isNotBlank() }
            ?: runCatching { NativeApp.getPauseGameTitle() }.getOrNull().orEmpty()
    }

    /** Host: player 2's motors as the game last set them, (large shl 8) or small; 0 while paused. */
    fun remoteRumble(): Int = if (isPaused()) 0 else NativeApp.sRemotePlayer2Rumble

    fun isPaused(): Boolean = runCatching { NativeApp.remotePlayPaused() }.getOrDefault(false)

    /** Viewer: the host's rumble on this device's controller ([deviceId], -1 for none yet). */
    fun playRemoteRumble(deviceId: Int, large: Int, small: Int) = NativeApp.playRemoteRumble(deviceId, large, small)
}
