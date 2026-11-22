package com.armsx2.input

import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.util.Log

/**
 * Reads a PlayStation pad's input reports straight off USB and feeds them to the core.
 *
 * This exists because of a single hard constraint. On a handheld whose Android build rewrites
 * controller identity, the input API's motors for the pad are fiction: it advertises vibrators,
 * accepts every vibrate() call, and moves nothing. The only route an app has to the real motors
 * is UsbManager, and reaching them means claiming the HID interface -- which detaches the driver
 * feeding the pad's INPUT. A DualSense has exactly one HID interface (index 3; the rest are
 * audio), so it cannot be held for output while leaving input alone: claim it and every button,
 * stick and trigger goes dead.
 *
 * The way out is to stop half-using the device. Once claimed, read its input reports here too and
 * push them to the core, so the pad works entirely through USB. That also picks up what the
 * platform was dropping anyway -- genuine analog triggers, and both motors driven independently
 * rather than flattened into one amplitude.
 *
 * USB only. Bluetooth HID output reports need the privileged HID host profile, which no ordinary
 * app can obtain, so a wireless pad cannot be reached this way.
 */
object UsbPadTakeover {
    private const val TAG = "ARMSX2Rumble"

    private const val CROSS = 0; private const val CIRCLE = 1
    private const val SQUARE = 2; private const val TRIANGLE = 3
    private const val BL1 = 4; private const val BR1 = 5
    private const val BL2 = 6; private const val BR2 = 7
    private const val SELECT = 8; private const val START = 9
    private const val THUMBL = 10; private const val THUMBR = 11
    private const val MODE = 12
    private const val DUP = 13; private const val DRIGHT = 14
    private const val DDOWN = 15; private const val DLEFT = 16

    /** A trigger counts as pressed here at the same point a real pad's digital bit trips. */
    private const val TRIGGER_DIGITAL_POINT = 32

    @Volatile private var reader: Thread? = null
    @Volatile private var running = false
    @Volatile private var input: UsbPadInput? = null

    fun active(): Boolean = running

    /** Start reading [connection] and delivering its input as Android events. */
    fun start(
        connection: UsbDeviceConnection,
        endpointIn: UsbEndpoint,
        dualsense: Boolean,
        port: Int,
    ) {
        stop()
        // Claim the slot for our synthetic id before any event goes out, or the first press
        // arrives as player 1 regardless of what the user pinned.
        val delivery = UsbPadInput(port)
        input = delivery
        running = true
        reader = Thread {
            val buf = ByteArray(64)
            var reports = 0L

            while (running && input === delivery) {
                // A report that did not arrive is not an error: the pad simply had nothing to say
                // inside the timeout, which is most of the time when nobody is touching it.
                val n = runCatching {
                    connection.bulkTransfer(endpointIn, buf, buf.size, 200)
                }.getOrDefault(-1)
                if (n <= 0) continue

                val next = runCatching {
                    if (dualsense) decodeDualSense(buf, n) else decodeDualShock4(buf, n)
                }.getOrNull() ?: continue

                if (reports++ == 0L) Log.i(TAG, "usb pad: first report, $n bytes")

                delivery.submit(next)
            }
        }.apply { isDaemon = true; name = "usb-pad"; start() }
        Log.i(TAG, "usb pad takeover started on port ${port + 1} (dualsense=$dualsense)")
    }

    fun stop() {
        running = false
        reader?.interrupt()
        reader = null
        input?.close()
        input = null
    }

    /**
     * DualSense USB input report: id 0x01, then hid-playstation's dualsense_input_report --
     * sticks, both triggers, a sequence counter, then the button bytes. The D-pad arrives as a
     * HAT value (0..7 clockwise from north, 8 = centred), not as four bits.
     */
    private fun decodeDualSense(buf: ByteArray, len: Int): UsbPadInput.Report? {
        if (len < 11 || (buf[0].toInt() and 0xFF) != 0x01) return null
        val b0 = buf[8].toInt() and 0xFF
        val b1 = buf[9].toInt() and 0xFF
        val b2 = buf[10].toInt() and 0xFF
        val l2 = buf[5].toInt() and 0xFF
        val r2 = buf[6].toInt() and 0xFF

        var bits = hatBits(b0 and 0x0F)
        if (b0 and 0x10 != 0) bits = bits or (1 shl SQUARE)
        if (b0 and 0x20 != 0) bits = bits or (1 shl CROSS)
        if (b0 and 0x40 != 0) bits = bits or (1 shl CIRCLE)
        if (b0 and 0x80 != 0) bits = bits or (1 shl TRIANGLE)
        if (b1 and 0x01 != 0) bits = bits or (1 shl BL1)
        if (b1 and 0x02 != 0) bits = bits or (1 shl BR1)
        if (b1 and 0x10 != 0) bits = bits or (1 shl SELECT)
        if (b1 and 0x20 != 0) bits = bits or (1 shl START)
        if (b1 and 0x40 != 0) bits = bits or (1 shl THUMBL)
        if (b1 and 0x80 != 0) bits = bits or (1 shl THUMBR)
        if (b2 and 0x01 != 0) bits = bits or (1 shl MODE)
        // Follow the travel rather than the pad's own click bit, so a half-pull reads as half.
        if (l2 >= TRIGGER_DIGITAL_POINT) bits = bits or (1 shl BL2)
        if (r2 >= TRIGGER_DIGITAL_POINT) bits = bits or (1 shl BR2)

        return UsbPadInput.Report(
            bits,
            intArrayOf(
                buf[1].toInt() and 0xFF, buf[2].toInt() and 0xFF,
                buf[3].toInt() and 0xFF, buf[4].toInt() and 0xFF, l2, r2,
            ),
        )
    }

    /** DualShock 4 USB input report: same idea, different offsets, triggers at the end. */
    private fun decodeDualShock4(buf: ByteArray, len: Int): UsbPadInput.Report? {
        if (len < 10 || (buf[0].toInt() and 0xFF) != 0x01) return null
        val b0 = buf[5].toInt() and 0xFF
        val b1 = buf[6].toInt() and 0xFF
        val b2 = buf[7].toInt() and 0xFF
        val l2 = buf[8].toInt() and 0xFF
        val r2 = buf[9].toInt() and 0xFF

        var bits = hatBits(b0 and 0x0F)
        if (b0 and 0x10 != 0) bits = bits or (1 shl SQUARE)
        if (b0 and 0x20 != 0) bits = bits or (1 shl CROSS)
        if (b0 and 0x40 != 0) bits = bits or (1 shl CIRCLE)
        if (b0 and 0x80 != 0) bits = bits or (1 shl TRIANGLE)
        if (b1 and 0x01 != 0) bits = bits or (1 shl BL1)
        if (b1 and 0x02 != 0) bits = bits or (1 shl BR1)
        if (b1 and 0x10 != 0) bits = bits or (1 shl SELECT)
        if (b1 and 0x20 != 0) bits = bits or (1 shl START)
        if (b1 and 0x40 != 0) bits = bits or (1 shl THUMBL)
        if (b1 and 0x80 != 0) bits = bits or (1 shl THUMBR)
        if (b2 and 0x01 != 0) bits = bits or (1 shl MODE)
        if (l2 >= TRIGGER_DIGITAL_POINT) bits = bits or (1 shl BL2)
        if (r2 >= TRIGGER_DIGITAL_POINT) bits = bits or (1 shl BR2)

        return UsbPadInput.Report(
            bits,
            intArrayOf(
                buf[1].toInt() and 0xFF, buf[2].toInt() and 0xFF,
                buf[3].toInt() and 0xFF, buf[4].toInt() and 0xFF, l2, r2,
            ),
        )
    }

    /** HAT value to D-pad bits. 8, and anything unexpected, is centred. */
    private fun hatBits(hat: Int): Int = when (hat) {
        0 -> (1 shl DUP)
        1 -> (1 shl DUP) or (1 shl DRIGHT)
        2 -> (1 shl DRIGHT)
        3 -> (1 shl DDOWN) or (1 shl DRIGHT)
        4 -> (1 shl DDOWN)
        5 -> (1 shl DDOWN) or (1 shl DLEFT)
        6 -> (1 shl DLEFT)
        7 -> (1 shl DUP) or (1 shl DLEFT)
        else -> 0
    }
}
