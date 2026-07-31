package com.armsx2.input

import androidx.compose.runtime.mutableStateOf
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp

/**
 * Which device is plugged into each emulated USB port.
 *
 * The core emulates eighteen of them — Buzz buzzers, a Rock Band drum kit, Keyboardmania, a DJ
 * turntable, the Printer, EyeToy, GunCon 2 and more. The list is read FROM the core's own registry
 * rather than hardcoded here, so it cannot drift from what this build actually supports.
 *
 * Buttons need no per-device setup. Native mirrors every pad press onto the attached device using
 * the generic mapping each binding already declares (see RebuildUsbGenericBinds in native-lib.cpp),
 * so a player's existing controls — physical pad, on-screen buttons, macros — drive a Buzz buzzer or
 * a drum pad with nothing extra to configure. Aiming for the lightgun is the one thing that needs
 * more than a button, and lives in [Lightgun].
 *
 * ★ Changing a port is RESTART-REQUIRED: the game probes the port at boot and caches what it found.
 */
object UsbDevices {
    data class Device(val type: String, val display: String, val subtypes: List<String>)

    /** Record and field separators used by the native enumeration. */
    private const val RS = "\u001e"
    private const val FS = "\u001f"

    private const val KEY_TYPE = "usb.port%d.type"
    private const val KEY_SUB = "usb.port%d.subtype"

    const val NONE = "None"

    /** Selected type per port. Mirrored as Compose state so the settings UI recomposes. */
    val portType = listOf(mutableStateOf(NONE), mutableStateOf(NONE))
    val portSubtype = listOf(mutableStateOf(0), mutableStateOf(0))

    private var cached: List<Device>? = null

    /** Devices this build can emulate, straight from the core. Cached — it cannot change at runtime. */
    fun available(): List<Device> {
        cached?.let { return it }
        val raw = runCatching { NativeApp.usbDeviceTypes() }.getOrNull().orEmpty()
        val list = raw.split(RS).mapNotNull { rec ->
            if (rec.isBlank()) return@mapNotNull null
            val parts = rec.split(FS)
            if (parts.size < 2) return@mapNotNull null
            Device(parts[0], parts[1], parts.drop(2))
        }
        cached = list
        return list
    }

    fun load() {
        for (p in 0..1) {
            portType[p].value =
                MainActivityRuntime.prefs.getString(KEY_TYPE.format(p), NONE) ?: NONE
            portSubtype[p].value = MainActivityRuntime.prefs.getInt(KEY_SUB.format(p), 0)
        }
    }

    /** Re-assert both ports at boot. The ini is authoritative; this covers a first run without one. */
    fun applyAtBoot() {
        for (p in 0..1) {
            if (portType[p].value != NONE) push(p)
        }
    }

    fun setType(port: Int, type: String) {
        if (port !in 0..1) return
        portType[port].value = type
        // A different device invalidates the old subtype index — they are per-device.
        portSubtype[port].value = 0
        MainActivityRuntime.prefs.edit()
            .putString(KEY_TYPE.format(port), type)
            .putInt(KEY_SUB.format(port), 0)
            .apply()
        push(port)
        // Keep the lightgun's own state honest: it owns aiming, and it must not believe a gun is
        // attached once this port holds a drum kit.
        Lightgun.syncFromPort(port, type)
    }

    fun setSubtype(port: Int, subtype: Int) {
        if (port !in 0..1) return
        portSubtype[port].value = subtype
        MainActivityRuntime.prefs.edit().putInt(KEY_SUB.format(port), subtype).apply()
        runCatching { NativeApp.usbSetDeviceSubtype(port, subtype) }
    }

    /** Display name for a type, falling back to the raw type if the core doesn't know it. */
    fun displayName(type: String): String =
        if (type == NONE) NONE else available().firstOrNull { it.type == type }?.display ?: type

    private fun push(port: Int) {
        runCatching { NativeApp.usbSetDeviceType(port, portType[port].value) }
        if (portSubtype[port].value != 0)
            runCatching { NativeApp.usbSetDeviceSubtype(port, portSubtype[port].value) }
    }
}
