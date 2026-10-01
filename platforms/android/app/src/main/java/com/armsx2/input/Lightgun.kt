package com.armsx2.input

import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.mutableStateOf
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp

/**
 * GunCon 2 lightgun, aimed with the touchscreen.
 *
 * The core already emulates the device (`usb_lightgun::GunCon2Device`, `DEVTYPE_GUNCON2`). This is
 * the Android half: it decides which USB port carries it, converts a touch into an aim, and turns
 * taps into trigger pulls.
 *
 * Why the touchscreen rather than a real gun: a CRT lightgun works by timing the scanline sweep,
 * which no LCD reproduces, so every modern lightgun setup is really "a pointing device pretending
 * to be one". A finger is a perfectly good pointing device, and it is the one every phone has.
 *
 * ★ The device type is RESTART-REQUIRED. Swapping a USB device on a live VM is the emulated
 * equivalent of yanking the plug mid-poll: the game has already probed the port and cached what it
 * found. The setting is written immediately so the next boot picks it up.
 */
object Lightgun {
    private const val KEY_ENABLED = "lightgun.enabled"
    private const val KEY_PORT = "lightgun.port"
    /** Off-screen reload margin, as a fraction of the shorter screen edge. */
    private const val EDGE_RELOAD_FRAC = 0.06f
    /** How long a calibration shot holds Recalibrate: several polls, under the core's 12-poll exchange. */
    private const val CALIBRATION_PRESS_MS = 150L

    /** Mirrors the pref so Compose recomposes. */
    val enabled = mutableStateOf(false)
    /** USB port index: 0 = Port 1, 1 = Port 2. */
    val port = mutableStateOf(0)

    fun load() {
        enabled.value = MainActivityRuntime.prefs.getBoolean(KEY_ENABLED, false)
        port.value = MainActivityRuntime.prefs.getInt(KEY_PORT, 0).coerceIn(0, 1)
    }

    /**
     * Persist the choice and push it to the core's USB config.
     *
     * "guncon2" is the core's own type name (`GunCon2Device::TypeName()`), and "None" detaches.
     * Written even with no VM up — it lands in the settings ini, which is what the next boot reads.
     */
    fun setEnabled(on: Boolean) {
        enabled.value = on
        MainActivityRuntime.prefs.edit().putBoolean(KEY_ENABLED, on).apply()
        push()
    }

    fun setPort(p: Int) {
        val newPort = p.coerceIn(0, 1)
        // Detach the old port first, or enabling Port 2 would leave a gun on Port 1 as well.
        if (newPort != port.value && enabled.value) {
            runCatching { NativeApp.usbSetDeviceType(port.value, "None") }
        }
        port.value = newPort
        MainActivityRuntime.prefs.edit().putInt(KEY_PORT, newPort).apply()
        push()
    }

    private fun push() {
        runCatching {
            NativeApp.usbSetDeviceType(port.value, if (enabled.value) "guncon2" else "None")
        }
    }

    /** Re-assert at boot — the ini is authoritative, but this covers a first run with no ini yet. */
    fun applyAtBoot() {
        if (enabled.value) push()
    }

    /**
     * Called when [UsbDevices] changes a port, so aiming follows the device rather than a stale
     * pref. Without this, picking a drum kit on the gun's port would leave the aim layer live and
     * every touch would be swallowed as a shot at a device that isn't there.
     */
    fun syncFromPort(changedPort: Int, type: String) {
        val isGun = type == "guncon2"
        if (isGun) {
            port.value = changedPort
            enabled.value = true
        } else if (changedPort == port.value) {
            enabled.value = false
        }
        MainActivityRuntime.prefs.edit()
            .putBoolean(KEY_ENABLED, enabled.value)
            .putInt(KEY_PORT, port.value)
            .apply()
    }

    // ---- runtime input -------------------------------------------------------

    private var triggerDown = false

    /** Cal was tapped: the next touch on the screen is a calibration shot, not a trigger pull. */
    val calibrateNext = mutableStateOf(false)

    private val mainHandler by lazy { Handler(Looper.getMainLooper()) }
    private val releaseCalibration = Runnable {
        runCatching { NativeApp.usbLightgunButton(port.value, NativeApp.GUNCON_RECALIBRATE, false) }
    }

    /**
     * Aim at a touch position, given with the size of the area it was measured in.
     *
     * Sent as a fraction of that area, which native scales to the game surface. The overlay and the
     * surface cover the same area, but the surface can have fewer pixels than the screen (the
     * performance downscale, a resolution override), and the core reads the pointer in surface
     * pixels: raw screen pixels put every shot below and right of the finger whenever they differ.
     */
    fun aim(x: Float, y: Float, widthPx: Float, heightPx: Float) {
        if (!enabled.value || widthPx <= 0f || heightPx <= 0f) return
        runCatching { NativeApp.usbLightgunAim(x / widthPx, y / heightPx) }
    }

    /**
     * Fire a calibration shot where the finger landed, instead of pulling the trigger.
     *
     * Some games (Time Crisis) will not pass their setup screen on a plain shot: after the trigger
     * they blank the screen and wait for the gun to report that it sees nothing, which a finger
     * never does. The core's Recalibrate binding plays that whole exchange at the current aim, so
     * aim first, then press it. Let go after [CALIBRATION_PRESS_MS] whatever the finger does: the
     * exchange lasts 12 polls, and a press still held when it ends starts another one.
     */
    fun calibrationShot(x: Float, y: Float, widthPx: Float, heightPx: Float) {
        calibrateNext.value = false
        if (!enabled.value) return
        aim(x, y, widthPx, heightPx)
        mainHandler.removeCallbacks(releaseCalibration)
        runCatching { NativeApp.usbLightgunButton(port.value, NativeApp.GUNCON_RECALIBRATE, true) }
        mainHandler.postDelayed(releaseCalibration, CALIBRATION_PRESS_MS)
    }

    /**
     * Pull or release the trigger.
     *
     * A touch within [EDGE_RELOAD_FRAC] of an edge fires OFF-SCREEN instead, which is how these
     * games reload — Time Crisis has no reload button, you point away from the screen. Without it
     * the games are unplayable past the first magazine.
     */
    fun trigger(down: Boolean, x: Float, y: Float, widthPx: Float, heightPx: Float) {
        if (!enabled.value) return
        if (down == triggerDown) return
        triggerDown = down
        val margin = minOf(widthPx, heightPx) * EDGE_RELOAD_FRAC
        val offscreen = x <= margin || y <= margin || x >= widthPx - margin || y >= heightPx - margin
        val bind = if (offscreen) NativeApp.GUNCON_SHOOT_OFFSCREEN else NativeApp.GUNCON_TRIGGER
        runCatching { NativeApp.usbLightgunButton(port.value, bind, down) }
        // Release BOTH on lift: a drag that starts on-screen and ends in the reload margin would
        // otherwise leave the on-screen trigger stuck down.
        if (!down) {
            runCatching {
                NativeApp.usbLightgunButton(port.value, NativeApp.GUNCON_TRIGGER, false)
                NativeApp.usbLightgunButton(port.value, NativeApp.GUNCON_SHOOT_OFFSCREEN, false)
            }
        }
    }

    /** Press/release one of the gun's own buttons (A / B / C / Start / Select / Recalibrate). */
    fun button(bind: Int, down: Boolean) {
        if (!enabled.value) return
        runCatching { NativeApp.usbLightgunButton(port.value, bind, down) }
    }
}
