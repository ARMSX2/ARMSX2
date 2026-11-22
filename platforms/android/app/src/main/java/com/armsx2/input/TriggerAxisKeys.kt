// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.input

/**
 * Decides which L2/R2 key events are duplicates of a trigger axis.
 *
 * Many pads report an analog trigger twice: as an axis that ramps 0..1 and as a
 * KEYCODE_BUTTON_L2/R2 key that the pad sends once the trigger passes its own digital
 * threshold. The axis path (MainActivityRuntime.sendTrigger) sends the bound PS2 control the
 * proportional travel. If the key also reached the pad it would write a full press over that
 * value as soon as the pad's threshold trips: a stick direction jumps to full deflection, a
 * pressure button to full pressure.
 *
 * A key event is a duplicate once the axis on that side of that device has moved. A pad that
 * declares the axis but never moves it, or has no axis, keeps its key. The decision is made on
 * the fresh press and kept for that press's key repeats and its release, so a release always
 * goes where its press went and nothing is left held. A release with no press recorded (the
 * press was taken by a hotkey, or came while the game was paused) is dropped once the axis
 * has moved, so it cannot zero the value the axis path is sending.
 *
 * Main thread only, like the input dispatch that calls it.
 */
internal class TriggerAxisKeys {
    private val movedAxes = HashSet<Long>()
    private val pressDropped = HashMap<Long, Boolean>()

    private fun side(deviceId: Int, left: Boolean): Long =
        (deviceId.toLong() shl 1) or (if (left) 0L else 1L)

    /** The [left]/right trigger axis of [deviceId] has reported travel past the deadzone. */
    fun axisMoved(deviceId: Int, left: Boolean) {
        movedAxes.add(side(deviceId, left))
    }

    /**
     * True when this L2/R2 key event must not reach the pad. [repeat] is a key repeat of a held
     * press. [needsKeyEdges] is true when the bound target is turbo or tap-to-hold, which act on
     * key edges the axis path does not produce.
     */
    fun drop(deviceId: Int, left: Boolean, down: Boolean, repeat: Boolean, needsKeyEdges: Boolean): Boolean {
        val k = side(deviceId, left)
        if (!down) return pressDropped.remove(k) ?: (k in movedAxes)
        if (repeat) pressDropped[k]?.let { return it }
        val dropped = !needsKeyEdges && k in movedAxes
        pressDropped[k] = dropped
        return dropped
    }

    /** The device was removed or changed; what it reports must be learned again. */
    fun forgetDevice(deviceId: Int) {
        for (left in booleanArrayOf(true, false)) {
            movedAxes.remove(side(deviceId, left))
            pressDropped.remove(side(deviceId, left))
        }
    }
}
