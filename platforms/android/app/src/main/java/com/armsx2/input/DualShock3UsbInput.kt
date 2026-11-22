package com.armsx2.input

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.hardware.usb.UsbRequest
import android.os.SystemClock
import android.util.Log
import java.nio.ByteBuffer
import java.util.concurrent.TimeoutException

/** Reads native 49-byte DualShock 3 reports from the claimed interrupt IN endpoint. */
internal class DualShock3UsbInput private constructor(
    private val connection: UsbDeviceConnection,
    private val iface: UsbInterface,
    private val request: UsbRequest,
    endpoint: UsbEndpoint,
    val port: Int,
    private val onReady: (DualShock3UsbInput) -> Unit,
    private val onReaderExit: (DualShock3UsbInput) -> Unit,
) {
    private val buffer = ByteBuffer.allocateDirect(maxOf(REPORT_SIZE, endpoint.maxPacketSize))
    private val report = ByteArray(REPORT_SIZE)
    @Volatile private var active = true
    private var delivered = false
    // Motor levels the game asked for (setRumble), and what the last output report carried.
    @Volatile private var rumbleLarge = 0
    @Volatile private var rumbleSmall = 0
    private var sentPlayer = -1
    private var sentLarge = 0
    private var sentSmall = 0
    private val input = UsbPadInput(port)
    @Volatile var ready = false
        private set
    @Volatile private var publishing = false

    fun enableInput() {
        publishing = true
    }

    fun start() {
        Thread(::read, "dualshock3-usb").apply { isDaemon = true; start() }
    }

    private fun read() {
        var rejectedPackets = 0
        var lastSize = 0
        try {
            wakeController()
            var deadline = SystemClock.elapsedRealtime() + INITIAL_REPORT_WAIT_MS
            var firstReport = true
            var retried = false
            while (queue()) {
                val completed = if (firstReport) {
                    try {
                        waitForReport(deadline)
                    } catch (error: TimeoutException) {
                        if (retried || !active) throw error
                        retried = true
                        // The interrupt request stays queued while we wake the controller again.
                        wakeController()
                        deadline = SystemClock.elapsedRealtime() + RETRY_REPORT_WAIT_MS
                        try {
                            waitForReport(deadline)
                        } catch (_: TimeoutException) {
                            Log.i(TAG, "DualShock 3 waiting for activation on port ${port + 1}; press PS")
                            waitForActivation()
                        }
                    }
                } else {
                    connection.requestWait()
                }
                if (completed !== request) break
                lastSize = buffer.position()
                if (lastSize == REPORT_SIZE) {
                    buffer.flip()
                    buffer.get(report)
                    if (report[0] == 1.toByte() && report[1] != (-1).toByte()) {
                        if (firstReport) {
                            firstReport = false
                            ready = true
                            onReady(this)
                        }
                        // Player LED after activation and when All controllers play as P1 changes,
                        // motors when the game changes them.
                        updateOutput()
                        if (publishing) deliver()
                        continue
                    }
                }
                if (firstReport) rejectedPackets++
            }
        } catch (_: TimeoutException) {
            if (active) Log.w(
                TAG,
                "DualShock 3 did not send a valid USB report on port ${port + 1} " +
                    "(rejected packets: $rejectedPackets, last size: $lastSize)",
            )
        } catch (error: RuntimeException) {
            Log.w(TAG, "DualShock 3 report reader stopped", error)
        } finally {
            stop()
            try {
                request.close()
            } finally {
                onReaderExit(this)
            }
        }
    }

    private fun wakeController() {
        val f2 = ByteArray(17)
        val f5 = ByteArray(8)
        val statusF2 = connection.controlTransfer(0xa1, 0x01, 0x03f2, iface.id, f2, f2.size, 500)
        val statusF5 = connection.controlTransfer(0xa1, 0x01, 0x03f5, iface.id, f5, f5.size, 250)
        Log.d(TAG, "DualShock 3 USB wake on port ${port + 1} (F2: $statusF2, F5: $statusF5)")
    }

    private fun waitForReport(deadline: Long): UsbRequest? {
        val remaining = deadline - SystemClock.elapsedRealtime()
        if (remaining <= 0) throw TimeoutException()
        return connection.requestWait(remaining)
    }

    private fun waitForActivation(): UsbRequest? {
        while (active) {
            try {
                return connection.requestWait(1000)
            } catch (_: TimeoutException) {
                // Keep the same interrupt request queued until PS is pressed or USB is closed.
            }
        }
        return null
    }

    @Synchronized
    private fun queue(): Boolean {
        if (!active) return false
        buffer.clear()
        return request.queue(buffer)
    }

    @Synchronized
    private fun deliver() {
        if (!active) return
        if (!delivered) Log.i(TAG, "DualShock 3 input received on port ${port + 1}")
        input.submit(UsbPadInput.decodeDualShock3(report))
        delivered = true
    }

    /** The player this pad plays as, which is the one whose rumble it gets. */
    fun playerPort(): Int = if (PadRouter.allToPlayer1) 0 else port

    /** Motor levels 0..255 from the game, on any thread. The reader thread sends them with the
     *  next input report, which a DualShock 3 on USB sends continuously; the caller never waits
     *  on USB. The small motor is on or off. */
    fun setRumble(large: Int, small: Int) {
        rumbleLarge = large.coerceIn(0, 255)
        rumbleSmall = if (small > 0) 1 else 0
    }

    /** Sends output report 0x01 when the player LED or a motor level differs from the last one
     *  sent, so the control pipe carries one report per change and none while nothing changes. */
    @Synchronized
    private fun updateOutput() {
        if (!active) return
        val player = playerPort()
        val large = rumbleLarge
        val small = rumbleSmall
        if (player == sentPlayer && large == sentLarge && small == sentSmall) return
        val ledChanged = player != sentPlayer
        sentPlayer = player
        sentLarge = large
        sentSmall = small
        val sent = sendOutput(outputReport(PLAYER_LED_MASKS[player], large, small))
        if (sent < 0) {
            Log.w(TAG, "DualShock 3 output report failed on port ${port + 1} ($sent)")
        } else if (ledChanged) {
            Log.i(TAG, "DualShock 3 player LED set for port ${player + 1}")
        }
    }

    // Over USB the report ID is in wValue, outside the payload.
    private fun sendOutput(output: ByteArray): Int = runCatching {
        connection.controlTransfer(0x21, 0x09, 0x0201, iface.id, output, output.size, 250)
    }.getOrDefault(-1)

    @Synchronized
    fun stop() {
        if (!active) return
        active = false
        ready = false
        // The motor durations are 0xff (no time limit), so stop them before letting go.
        if (sentPlayer >= 0 && (sentLarge != 0 || sentSmall != 0))
            sendOutput(outputReport(PLAYER_LED_MASKS[sentPlayer], 0, 0))
        input.close()
        runCatching { request.cancel() }
        runCatching { connection.releaseInterface(iface) }
        connection.close()
    }

    companion object {
        private const val TAG = "ARMSX2DualShock3"
        private const val REPORT_SIZE = 49
        private const val INITIAL_REPORT_WAIT_MS = 5_000L
        private const val RETRY_REPORT_WAIT_MS = 10_000L
        // Same LED combinations as Linux hid-sony for player indices 1 through 8.
        private val PLAYER_LED_MASKS = byteArrayOf(0x02, 0x04, 0x08, 0x10, 0x12, 0x14, 0x18, 0x1a)

        /**
         * Sixaxis output report 0x01 payload. Byte 1 is the small (right) motor's duration and
         * byte 2 turns it on or off; byte 3 is the large (left) motor's duration and byte 4 its
         * force 0..255. Byte 9 is the LED mask, then the timing of LEDs 4 to 1, five bytes each,
         * all set to steady on.
         */
        internal fun outputReport(ledMask: Byte, large: Int, small: Int): ByteArray {
            val output = ByteArray(35)
            output[0] = 0x01
            output[1] = 0xff.toByte()
            output[2] = if (small > 0) 1 else 0
            output[3] = 0xff.toByte()
            output[4] = large.coerceIn(0, 255).toByte()
            output[9] = ledMask
            val steadyLed = byteArrayOf(0xff.toByte(), 0x27, 0x10, 0x00, 0x32)
            repeat(4) { led -> steadyLed.copyInto(output, 10 + led * steadyLed.size) }
            return output
        }

        fun open(
            usb: UsbManager,
            device: UsbDevice,
            port: Int,
            onReady: (DualShock3UsbInput) -> Unit,
            onReaderExit: (DualShock3UsbInput) -> Unit,
        ): DualShock3UsbInput? {
            val iface = (0 until device.interfaceCount).asSequence().map(device::getInterface)
                .firstOrNull { it.interfaceClass == UsbConstants.USB_CLASS_HID } ?: return null
            val endpoint = (0 until iface.endpointCount).asSequence().map(iface::getEndpoint)
                .firstOrNull {
                    it.direction == UsbConstants.USB_DIR_IN && it.type == UsbConstants.USB_ENDPOINT_XFER_INT
                } ?: return null
            val connection = usb.openDevice(device) ?: return null
            val request = UsbRequest()
            var opened = false
            try {
                if (!connection.claimInterface(iface, true)) return null
                if (!request.initialize(connection, endpoint)) return null
                val input = DualShock3UsbInput(connection, iface, request, endpoint, port, onReady, onReaderExit)
                opened = true
                return input
            } finally {
                if (!opened) {
                    runCatching { request.close() }
                    runCatching { connection.releaseInterface(iface) }
                    connection.close()
                }
            }
        }
    }
}
