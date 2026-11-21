package com.armsx2.input

/** Physical trigger pressures, kept across button edges, joystick samples and gyro updates. */
internal class TriggerStickRemap {
    private data class Pressures(val port: Int, val left: Float, val right: Float)

    private val devices = HashMap<Int, Pressures>()

    /** A null side has no new sample. Returns the old port if the device moved to a new one. */
    fun update(deviceId: Int, port: Int, left: Float?, right: Float?): Int? {
        val previous = devices[deviceId]
        val nextLeft = left ?: previous?.left ?: 0f
        val nextRight = right ?: previous?.right ?: 0f
        if (nextLeft == 0f && nextRight == 0f) {
            devices.remove(deviceId)
        } else if (previous?.port != port || previous.left != nextLeft || previous.right != nextRight) {
            devices[deviceId] = Pressures(port, nextLeft, nextRight)
        }
        return previous?.port?.takeIf { it != port }
    }

    /** In Android stick coordinates, negative is up (R2) and positive is down (L2). */
    fun vertical(port: Int): Float {
        var left = 0f
        var right = 0f
        for (pressures in devices.values) {
            if (pressures.port != port) continue
            left = maxOf(left, pressures.left)
            right = maxOf(right, pressures.right)
        }
        return left - right
    }

    /** Merge with the existing stick writers, using one half-axis when a trigger deflects Y. */
    inline fun emitVertical(port: Int, stickUp: Float, stickDown: Float, emit: (Int, Float) -> Unit) {
        val trigger = vertical(port)
        if (trigger == 0f) {
            emit(RIGHT_STICK_UP, stickUp)
            emit(RIGHT_STICK_DOWN, stickDown)
            return
        }
        // The native pad gives one half-axis priority when both are set. Resolve opposing
        // pressures here so the stronger deflection wins in either direction.
        val combined = maxOf(stickDown, trigger) - maxOf(stickUp, -trigger)
        emit(RIGHT_STICK_UP, (-combined).coerceAtLeast(0f))
        emit(RIGHT_STICK_DOWN, combined.coerceAtLeast(0f))
    }

    fun remove(deviceId: Int): Int? = devices.remove(deviceId)?.port

    fun portForDevice(deviceId: Int): Int? = devices[deviceId]?.port

    fun clear(): Set<Int> {
        if (devices.isEmpty()) return emptySet()
        val ports = devices.values.mapTo(HashSet()) { it.port }
        devices.clear()
        return ports
    }

    companion object {
        const val RIGHT_STICK_UP = 120
        const val RIGHT_STICK_DOWN = 122
    }
}
