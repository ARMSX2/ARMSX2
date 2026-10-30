// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.input

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import androidx.core.content.ContextCompat
import kr.co.iefriends.pcsx2.NativeApp

/**
 * The phone's microphones, for games that use one (Audio settings, Microphone).
 *
 * Android lists input devices to Java only, so this hands them to the core's capture
 * (audiodev-oboe.cpp) under the names settings store: the kind of device and its product name, which
 * survive unplugging it. Android's id for a device does not, so the list goes again whenever one comes
 * or goes, and before each game starts.
 */
object MicDevices {
    /** Lets Android choose: a plugged-in USB microphone when there is one, the built-in one otherwise. */
    const val DEFAULT = "Default"
    const val BUILTIN = "builtin"

    /** [product] is empty for the built-in microphone, which the UI names itself. */
    data class Device(val name: String, val product: String, val id: Int)

    fun supported(context: Context): Boolean = runCatching {
        context.packageManager.getPackageInfo(context.packageName, PackageManager.GET_PERMISSIONS)
            .requestedPermissions?.contains(Manifest.permission.RECORD_AUDIO) == true
    }.getOrDefault(false)

    fun granted(context: Context): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    /** What a game can be given: the built-in microphone once, and whatever is plugged in. */
    fun list(context: Context): List<Device> {
        val manager = context.getSystemService(AudioManager::class.java) ?: return emptyList()
        val devices = mutableListOf<Device>()
        val seen = HashMap<String, Int>()

        for (info in manager.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
            val kind = when (info.type) {
                AudioDeviceInfo.TYPE_BUILTIN_MIC -> BUILTIN
                AudioDeviceInfo.TYPE_USB_DEVICE,
                AudioDeviceInfo.TYPE_USB_HEADSET,
                AudioDeviceInfo.TYPE_USB_ACCESSORY -> "usb"
                AudioDeviceInfo.TYPE_WIRED_HEADSET -> "headset"
                AudioDeviceInfo.TYPE_LINE_ANALOG,
                AudioDeviceInfo.TYPE_LINE_DIGITAL -> "line"
                else -> continue
            }

            if (kind == BUILTIN) {
                // Listed once per microphone hole on most phones; a game wants the one
                if (devices.none { it.name == BUILTIN }) devices += Device(BUILTIN, "", info.id)
                continue
            }

            val product = info.productName?.toString()?.trim().orEmpty().ifEmpty { kind }
            val base = "$kind:$product"
            // Two of the same microphone, as SingStar has, are told apart by the order Android lists them
            val n = (seen[base] ?: 0) + 1
            seen[base] = n
            devices += Device(if (n == 1) base else "$base #$n", if (n == 1) product else "$product #$n", info.id)
        }

        return devices
    }

    /** Hands the current list to the core. Any thread. */
    fun push(context: Context) {
        NativeApp.setMicDevices(list(context).joinToString("\n") { "${it.id}\t${it.name}" })
    }

    private var watching = false

    /** Keeps the core's list current while microphones are plugged in and out. Main thread. */
    fun watch(context: Context) {
        if (watching || !supported(context)) return
        val manager = context.getSystemService(AudioManager::class.java) ?: return
        val app = context.applicationContext

        manager.registerAudioDeviceCallback(object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) = push(app)
            override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) = push(app)
        }, null)

        watching = true
    }
}
