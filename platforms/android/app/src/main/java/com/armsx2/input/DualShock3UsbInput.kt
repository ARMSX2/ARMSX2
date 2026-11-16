package com.armsx2.input

import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbRequest
import android.util.Log
import kr.co.iefriends.pcsx2.NativeApp
import java.nio.ByteBuffer
import java.util.concurrent.TimeoutException

/** Reads native 49-byte DualShock 3 reports from the claimed interrupt IN endpoint. */
internal class DualShock3UsbInput private constructor(
    private val connection: UsbDeviceConnection,
    private val request: UsbRequest,
    endpoint: UsbEndpoint,
    private val port: Int,
    private val onReaderExit: (DualShock3UsbInput) -> Unit,
) {
    private val buffer = ByteBuffer.allocateDirect(maxOf(REPORT_SIZE, endpoint.maxPacketSize))
    private val report = ByteArray(REPORT_SIZE)
    private var active = true
    private var delivered = false

    fun start() {
        Thread(::read, "dualshock3-usb").apply { isDaemon = true; start() }
    }

    private fun read() {
        try {
            val deadline = android.os.SystemClock.elapsedRealtime() + 5000
            var firstReport = true
            while (queue()) {
                val completed = if (firstReport) {
                    val timeout = deadline - android.os.SystemClock.elapsedRealtime()
                    if (timeout <= 0) throw TimeoutException()
                    connection.requestWait(timeout)
                } else {
                    connection.requestWait()
                }
                if (completed !== request) break
                if (buffer.position() == REPORT_SIZE) {
                    buffer.flip()
                    buffer.get(report)
                    if (report[0] == 1.toByte() && report[1] != (-1).toByte()) {
                        deliver()
                        firstReport = false
                    }
                }
            }
        } catch (_: TimeoutException) {
            Log.w(TAG, "DualShock 3 did not send a valid USB report")
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

    @Synchronized
    private fun queue(): Boolean {
        if (!active) return false
        buffer.clear()
        return request.queue(buffer)
    }

    @Synchronized
    private fun deliver() {
        if (!active) return
        if (!delivered) Log.i(TAG, "DualShock 3 native report received on port ${port + 1}")
        NativeApp.setDualShock3UsbReport(port, report)
        delivered = true
    }

    @Synchronized
    fun stop() {
        if (!active) return
        active = false
        request.cancel()
        if (delivered) {
            val neutral = ByteArray(REPORT_SIZE)
            neutral[0] = 1
            neutral.fill(127, fromIndex = 6, toIndex = 10)
            NativeApp.setDualShock3UsbReport(port, neutral)
        }
    }

    companion object {
        private const val TAG = "ARMSX2DualShock3"
        private const val REPORT_SIZE = 49

        fun open(
            connection: UsbDeviceConnection,
            endpoint: UsbEndpoint,
            port: Int,
            onReaderExit: (DualShock3UsbInput) -> Unit,
        ): DualShock3UsbInput? {
            val request = UsbRequest()
            if (!request.initialize(connection, endpoint)) {
                request.close()
                return null
            }
            return DualShock3UsbInput(connection, request, endpoint, port, onReaderExit)
        }
    }
}
