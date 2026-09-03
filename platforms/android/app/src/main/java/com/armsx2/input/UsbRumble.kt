package com.armsx2.input

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.util.Log

/**
 * Rumble for a PlayStation controller, written straight to the pad over USB.
 *
 * Android's input API cannot drive these motors on every handheld. A device whose firmware
 * bridges an attached controller through its own HID node re-presents it under the handheld's
 * own vendor id, advertises a full vibrator inventory for it -- ids, hasVibrator, the lot --
 * accepts every vibrate() call without error, and moves nothing, because force feedback is
 * never forwarded to the pad. Nothing in the input API distinguishes that from a working motor.
 *
 * The real device is still on the USB bus underneath, so this addresses it directly with a HID
 * SET_REPORT and bypasses the bridge entirely.
 *
 * Deliberately a CONTROL transfer on endpoint 0, not an interrupt endpoint: claiming the
 * interface would detach whatever driver owns it and the pad's BUTTONS would stop working, which
 * is a far worse bug than no rumble. Control transfers need no claim, so input is untouched.
 *
 * USB only. Bluetooth HID output reports are not reachable from an app.
 */
object UsbRumble {
    private const val TAG = "ARMSX2Rumble"

    private const val VENDOR_SONY = 0x054C
    // DualSense, DualSense Edge.
    private val DUALSENSE = setOf(0x0CE6, 0x0DF2)
    // DualShock 4 v1, v2, and the USB adapter.
    private val DUALSHOCK4 = setOf(0x05C4, 0x09CC, 0x0BA0)

    private const val ACTION_PERMISSION = "com.armsx2.action.USB_RUMBLE_PERMISSION"

    /** An open pad we can write reports to. */
    class Pad(
        val label: String,
        val productName: String,
        val connection: UsbDeviceConnection,
        val iface: android.hardware.usb.UsbInterface,
        private val endpointOut: android.hardware.usb.UsbEndpoint?,
        val endpointIn: android.hardware.usb.UsbEndpoint?,
        val dualsense: Boolean,
    ) {
        private var lastLarge = -1
        private var lastSmall = -1

        /**
         * How we are talking to the pad.
         *
         * Start unclaimed, because claiming detaches whatever driver owns the interface and the
         * pad's BUTTONS would stop working. If an unclaimed control transfer is refused -- which
         * it is when something else holds the interface -- there is no way to reach the motors
         * without taking it, so that is tried once and the outcome recorded rather than retried
         * on every report.
         */
        private enum class Mode { UNCLAIMED_CONTROL, CLAIMED, DEAD }
        private var mode = Mode.UNCLAIMED_CONTROL
        private var claimed = false

        /** False once the pad has refused us, so callers fall back to the input API. */
        fun usable(): Boolean = mode != Mode.DEAD

        /** Take the interface so both input and the motors come through us. */
        fun claim(): Boolean {
            if (claimed) return true
            claimed = runCatching { connection.claimInterface(iface, true) }.getOrDefault(false)
            if (claimed) mode = Mode.CLAIMED
            return claimed
        }

        /** Motor levels, 0..255. Returns true when the pad accepted the report. */
        @Synchronized
        fun rumble(large: Int, small: Int): Boolean {
            if (large == lastLarge && small == lastSmall) return true
            lastLarge = large
            lastSmall = small

            val report = if (dualsense) dualsenseReport(large, small) else dualshock4Report(large, small)
            if (mode == Mode.DEAD) return false

            if (mode == Mode.UNCLAIMED_CONTROL) {
                val sent = control(report)
                if (sent >= 0) return true
                if (claimed) { mode = Mode.CLAIMED } else {

                // Claiming the interface WOULD get the motors working -- measured, it does -- but
                // a DualSense has exactly one HID interface (index 3; the rest are audio), so
                // taking it detaches the driver feeding the pad's input and every button, stick
                // and trigger goes dead. Rumble is not worth a controller that cannot play games.
                // Reaching the motors therefore requires reading input over USB as well and
                // feeding it to the core, which is a feature, not a fallback -- until that exists,
                // stand down and let the input API have it.
                Log.i(TAG, "usb: control transfer refused ($sent) on $label; not claiming (it would kill the pad's input)")
                mode = Mode.DEAD
                return false
                }
            }

            // Claimed: the interrupt OUT endpoint is the pad's normal channel, with the control
            // transfer as a second chance for devices that only accept SET_REPORT.
            var sent = -1
            val ep = endpointOut
            if (ep != null) {
                sent = runCatching { connection.bulkTransfer(ep, report, report.size, 500) }.getOrDefault(-1)
            }
            if (sent < 0) sent = control(report)

            if (sent < 0) Log.i(TAG, "usb rumble write failed on $label ($sent) (large=$large small=$small)")
            return sent >= 0
        }

        /** HID SET_REPORT: 0x21 = host-to-device | class | interface, 0x09 = SET_REPORT,
         *  wValue high byte 0x02 = Output report, low byte = the report id. */
        private fun control(report: ByteArray): Int = runCatching {
            connection.controlTransfer(
                0x21, 0x09, 0x0200 or (report[0].toInt() and 0xFF), iface.id,
                report, report.size, 500,
            )
        }.getOrDefault(-1)

        @Synchronized
        fun stop() {
            rumble(0, 0)
        }

        @Synchronized
        fun close() {
            runCatching { rumble(0, 0) }
            // Release before closing so whatever owned the interface can have it back.
            if (claimed) runCatching { connection.releaseInterface(iface) }
            runCatching { connection.close() }
        }

        /**
         * DualSense USB output report.
         *
         * Byte 0 is the report id; the common block follows, so valid_flag0 lands at 1,
         * valid_flag1 at 2, then the right (high-frequency) and left (low-frequency) motors.
         * flag0 0x03 is COMPATIBLE_VIBRATION | HAPTICS_SELECT -- the pair that puts the pad in
         * classic rumble mode rather than its haptic actuators.
         */
        private fun dualsenseReport(large: Int, small: Int) = ByteArray(48).also {
            it[0] = 0x02
            it[1] = 0x03
            it[2] = 0x00
            it[3] = small.coerceIn(0, 255).toByte()
            it[4] = large.coerceIn(0, 255).toByte()
        }

        /** DualShock 4 USB output report: flags byte 0x01 = rumble only, leaving the lightbar alone. */
        private fun dualshock4Report(large: Int, small: Int) = ByteArray(32).also {
            it[0] = 0x05
            it[1] = 0x01
            it[4] = small.coerceIn(0, 255).toByte()
            it[5] = large.coerceIn(0, 255).toByte()
        }
    }

    @Volatile private var pad: Pad? = null
    private var receiver: BroadcastReceiver? = null
    private var appContext: Context? = null

    private const val KEY_TAKEOVER = "usb_pad_takeover"

    /**
     * Off by default, and deliberately so.
     *
     * Turning it on claims the pad's only HID interface, which detaches the driver feeding
     * Android's input for it. Everything then comes through us instead -- buttons, sticks,
     * analog triggers and both rumble motors. That is the whole point, but it also means the
     * pad stops existing for the rest of the system while a game is running, and a bug in here
     * costs the user their controller rather than just their rumble. It has to be a choice.
     */
    @Volatile var takeover: Boolean = false
        private set

    fun loadTakeover() {
        takeover = runCatching {
            com.armsx2.runtime.MainActivityRuntime.prefs.getBoolean(KEY_TAKEOVER, false)
        }.getOrDefault(false)
    }

    fun setTakeover(on: Boolean) {
        takeover = on
        runCatching {
            com.armsx2.runtime.MainActivityRuntime.prefs.edit().putBoolean(KEY_TAKEOVER, on).apply()
        }
        val open = pad
        if (on) {
            if (open != null) engageTakeover(open)
        } else {
            UsbPadTakeover.stop()
            Log.i(TAG, "usb pad takeover off — replug the controller to give its input back to Android")
        }
    }

    /**
     * Resolve the player slot BEFORE claiming, because claiming makes the Android input device
     * disappear and PadRouter would have nothing left to answer with.
     */
    private fun engageTakeover(open: Pad) {
        val ep = open.endpointIn
        if (ep == null) {
            Log.i(TAG, "usb pad takeover: ${open.productName} has no interrupt IN endpoint")
            return
        }
        var port = 0
        for (candidate in PadRouter.connectedPads()) {
            if (!namesMatch(candidate.name, open.productName)) continue
            port = PadRouter.pins()[candidate.descriptor] ?: 0
            break
        }
        if (!open.claim()) {
            Log.i(TAG, "usb pad takeover: could not claim ${open.productName}'s interface")
            return
        }
        UsbPadTakeover.start(open.connection, ep, open.dualsense, port)
    }

    /** True when a PlayStation pad is open and can actually be driven. */
    fun available(): Boolean = pad?.usable() == true

    /**
     * The open pad, if [dev] looks like the same controller.
     *
     * Matched on NAME, because the bridge is exactly what hides the identity: the input node
     * carries the handheld's vendor id, so only the product string survives to be compared.
     */
    fun padFor(dev: android.view.InputDevice?): Pad? {
        val p = pad?.takeIf { it.usable() } ?: return null
        val name = dev?.name ?: return null
        return if (namesMatch(name, p.productName)) p else null
    }

    private fun namesMatch(inputName: String, productName: String): Boolean {
        val a = inputName.lowercase()
        val b = productName.lowercase()
        if (b.isNotBlank() && (a.contains(b) || b.contains(a))) return true
        // Family names only, and only ones that actually identify a PlayStation pad.
        //
        // "wireless controller" used to be in this list, and it matches an Xbox pad just as well:
        // on a handheld that renames every controller "<something> Wireless Controller", the USB
        // DualSense matched EVERY pad and answered for every player slot, leaving the others with
        // no rumble at all. A shared noun is not an identity.
        return listOf("dualsense", "dualshock").any { a.contains(it) && b.contains(it) }
    }

    /** Scan, ask for permission if needed, and follow attach/detach. Safe to call repeatedly. */
    fun start(ctx: Context) {
        val app = ctx.applicationContext
        appContext = app
        if (receiver == null) {
            val r = object : BroadcastReceiver() {
                override fun onReceive(context: Context, intent: Intent) {
                    when (intent.action) {
                        ACTION_PERMISSION, UsbManager.ACTION_USB_DEVICE_ATTACHED -> refresh(app)
                        UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                            val gone = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
                            if (gone == null || isPlayStationPad(gone)) {
                                pad?.close()
                                pad = null
                                Log.i(TAG, "usb pad detached")
                            }
                        }
                    }
                }
            }
            val filter = IntentFilter().apply {
                addAction(ACTION_PERMISSION)
                addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
                addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            }
            runCatching {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    app.registerReceiver(r, filter, Context.RECEIVER_NOT_EXPORTED)
                } else {
                    @Suppress("UnspecifiedRegisterReceiverFlag")
                    app.registerReceiver(r, filter)
                }
                receiver = r
            }
        }
        refresh(app)
    }

    fun stop() {
        UsbPadTakeover.stop()
        pad?.close()
        pad = null
        receiver?.let { r -> runCatching { appContext?.unregisterReceiver(r) } }
        receiver = null
    }

    private fun isPlayStationPad(dev: UsbDevice): Boolean =
        dev.vendorId == VENDOR_SONY && (dev.productId in DUALSENSE || dev.productId in DUALSHOCK4)

    /** Open the first PlayStation pad on the bus, requesting permission if we do not have it. */
    private fun refresh(ctx: Context) {
        val usb = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return
        val device = usb.deviceList.values.firstOrNull { isPlayStationPad(it) }

        if (device == null) {
            pad?.close()
            pad = null
            return
        }
        if (pad != null) return

        if (!usb.hasPermission(device)) {
            // The system prompt is the only way in. Asking on attach means it appears while the
            // user is plugging the pad in, which is when it makes sense to them.
            val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
            val intent = PendingIntent.getBroadcast(
                ctx, 0, Intent(ACTION_PERMISSION).setPackage(ctx.packageName), flags,
            )
            runCatching { usb.requestPermission(device, intent) }
            Log.i(TAG, "usb rumble: asked for permission on ${device.productName}")
            return
        }

        val dualsense = device.productId in DUALSENSE
        // The HID interface, which is the one SET_REPORT is addressed to.
        var hid: android.hardware.usb.UsbInterface? = null
        for (i in 0 until device.interfaceCount) {
            val candidate = device.getInterface(i)
            if (candidate.interfaceClass == UsbConstants.USB_CLASS_HID) { hid = candidate; break }
        }
        val iface = hid
        if (iface == null) {
            Log.i(TAG, "usb rumble: ${device.productName} exposes no HID interface")
            return
        }
        var out: android.hardware.usb.UsbEndpoint? = null
        var inEp: android.hardware.usb.UsbEndpoint? = null
        for (i in 0 until iface.endpointCount) {
            val ep = iface.getEndpoint(i)
            if (ep.direction == UsbConstants.USB_DIR_OUT && out == null) out = ep
            if (ep.direction == UsbConstants.USB_DIR_IN && inEp == null) inEp = ep
        }

        val conn = runCatching { usb.openDevice(device) }.getOrNull()
        if (conn == null) {
            Log.i(TAG, "usb rumble: could not open ${device.productName}")
            return
        }

        val name = device.productName ?: (if (dualsense) "DualSense" else "DualShock 4")
        val opened = Pad("$name (USB)", name, conn, iface, out, inEp, dualsense)
        pad = opened
        Log.i(TAG, "usb rumble ready: $name iface=${iface.id} out=${out != null} in=${inEp != null} dualsense=$dualsense")
        if (takeover) engageTakeover(opened)
    }
}
