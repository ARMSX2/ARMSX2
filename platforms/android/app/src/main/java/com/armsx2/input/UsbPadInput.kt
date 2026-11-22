package com.armsx2.input

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import com.armsx2.runtime.MainActivityRuntime

/** Common frontend delivery for USB PlayStation pads, including DS3 button pressure.
 * Key edges go through the activity so menus, bindings, hotkeys and macros keep their precedence.
 * Pressure changes update only a gameplay press that the activity actually accepted.
 */
internal class UsbPadInput(port: Int) {
    private val deviceId = nextDeviceId--
    class Report(val buttons: Int, val axes: IntArray, val pressure: IntArray? = null)

    @Volatile private var active = true
    private var previous: Report? = null
    private var current: Report? = null

    init {
        pads[deviceId] = this
        PadRouter.setSyntheticPad(deviceId, port)
    }

    fun submit(report: Report) {
        main.post {
            if (!active) return@post
            val activity = MainActivityRuntime.instance ?: return@post
            val before = previous
            current = report
            val changed = report.buttons xor (before?.buttons ?: 0)
            // Preserve the existing takeover order: new presses before releases in the same report.
            for (bit in KEYCODE.indices) {
                if (changed and report.buttons and (1 shl bit) != 0)
                    sendKey(activity, KEYCODE[bit], true)
            }
            for (bit in KEYCODE.indices) {
                if (changed and report.buttons.inv() and (1 shl bit) != 0)
                    sendKey(activity, KEYCODE[bit], false)
            }
            for (bit in KEYCODE.indices) {
                if (changed and (1 shl bit) == 0 && report.pressure != null &&
                    report.buttons and (1 shl bit) != 0 && report.pressure[bit] != before?.pressure?.get(bit)
                ) activity.updateUsbButtonPressure(deviceId, KEYCODE[bit])
            }
            if (before == null || !report.axes.contentEquals(before.axes)) sendMotion(activity, report.axes)
            previous = report
        }
    }

    fun close() {
        active = false
        val release = Runnable {
            release()
            pads.remove(deviceId, this)
            PadRouter.clearSyntheticPad(deviceId)
        }
        if (Looper.myLooper() == Looper.getMainLooper()) release.run() else main.post(release)
    }

    private fun release() {
        val activity = MainActivityRuntime.instance
        val before = previous
        if (activity != null && before != null) {
            for (bit in KEYCODE.indices) {
                if (before.buttons and (1 shl bit) != 0) sendKey(activity, KEYCODE[bit], false)
            }
            sendMotion(activity, intArrayOf(128, 128, 128, 128, 0, 0))
            activity.releaseUsbPadInput(deviceId)
        }
        current = null
        previous = null
    }

    private fun sendKey(activity: MainActivityRuntime, keyCode: Int, down: Boolean) {
        val now = SystemClock.uptimeMillis()
        activity.dispatchKeyEvent(KeyEvent(
            now, now, if (down) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP, keyCode, 0, 0,
            deviceId, 0, 0, InputDevice.SOURCE_GAMEPAD,
        ))
    }

    private fun sendMotion(activity: MainActivityRuntime, axes: IntArray) {
        val now = SystemClock.uptimeMillis()
        val coords = MotionEvent.PointerCoords().apply {
            setAxisValue(MotionEvent.AXIS_X, centred(axes[0]))
            setAxisValue(MotionEvent.AXIS_Y, centred(axes[1]))
            setAxisValue(MotionEvent.AXIS_Z, centred(axes[2]))
            setAxisValue(MotionEvent.AXIS_RZ, centred(axes[3]))
            if (axes.size >= 6) {
                setAxisValue(MotionEvent.AXIS_LTRIGGER, axes[4] / 255f)
                setAxisValue(MotionEvent.AXIS_BRAKE, axes[4] / 255f)
                setAxisValue(MotionEvent.AXIS_RTRIGGER, axes[5] / 255f)
                setAxisValue(MotionEvent.AXIS_GAS, axes[5] / 255f)
            }
        }
        val props = MotionEvent.PointerProperties().apply { id = 0 }
        val event = MotionEvent.obtain(
            now, now, MotionEvent.ACTION_MOVE, 1, arrayOf(props), arrayOf(coords),
            0, 0, 1f, 1f, deviceId, 0, InputDevice.SOURCE_JOYSTICK, 0,
        )
        try { activity.dispatchGenericMotionEvent(event) } finally { event.recycle() }
    }

    companion object {
        private val main by lazy { Handler(Looper.getMainLooper()) }
        // Accessed on the main looper, as are activity input dispatch and PadRouter registration.
        private val pads = HashMap<Int, UsbPadInput>()
        private var nextDeviceId = -1000

        fun resetAll() { pads.values.toList().forEach { it.release() } }
        private val KEYCODE = intArrayOf(
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
            KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2,
            KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_START,
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR,
            KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT,
        )
        // DS3 byte 2, then byte 3, in the common DS4/DualSense button order above.
        private val DS3_BITS = intArrayOf(8, 10, 11, 9, 13, 14, 15, 16, 6, 7, 4, 5, 3, 1, 0, 2)

        fun decodeDualShock3(bytes: ByteArray): Report {
            require(bytes.size == 49 && bytes[0] == 1.toByte() && bytes[1] != (-1).toByte())
            val digital = (bytes[2].toInt() and 255) or ((bytes[3].toInt() and 255) shl 8)
            var buttons = if (bytes[4].toInt() and 1 != 0) 1 shl 12 else 0
            val pressure = IntArray(KEYCODE.size) { 255 }
            for (bit in DS3_BITS.indices) {
                val target = DS3_BITS[bit]
                if (digital and (1 shl bit) != 0) buttons = buttons or (1 shl target)
                pressure[target] = if (bit < 4) 255 else bytes[bit + 10].toInt() and 255
            }
            return Report(buttons, IntArray(4) { bytes[it + 6].toInt() and 255 }, pressure)
        }

        fun pressureFor(deviceId: Int, keyCode: Int): Int? {
            val bit = KEYCODE.indexOf(keyCode)
            if (bit < 0) return null
            return pads[deviceId]?.current?.pressure?.get(bit)
        }

        fun hasAxis(deviceId: Int, axis: Int): Boolean? {
            val pad = pads[deviceId] ?: return null
            return when (axis) {
                MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ -> true
                MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
                MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS -> (pad.current?.axes?.size ?: 0) >= 6
                else -> false
            }
        }

        // Round up when converting to the existing JNI range: truncation on both sides would
        // otherwise turn a DS3 pressure byte into the byte below it. Never 0, which applyPadButton
        // reads as a full press: a held button whose pressure byte is still 0 is sent as 1, which
        // the pad stores as pressed with pressure 0.
        fun pressureRange(pressure: Int): Int =
            maxOf(1, (pressure.coerceIn(0, 255) * 32767 + 254) / 255)

        private fun centred(v: Int) = ((v - 128) / 127f).coerceIn(-1f, 1f)
    }
}
