// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import java.io.File

/**
 * Settings a launch intent can carry for that launch, for driving the app from a workstation with no hands on the
 * device. They are not in the UI and are never saved: each launch intent replaces them (an intent without them clears
 * them), and a launch from the app's own library clears them.
 *
 *   adb shell am start -n <package>/com.armsx2.Main -a android.intent.action.VIEW --es path <disc> \
 *       --ez armsx2.pine true --ei armsx2.pine_slot 28011 \
 *       --es armsx2.driver <installed driver's id or name, or "system"> \
 *       --es armsx2.driver_import <path on the device to a driver .zip>
 *
 * `armsx2.driver_import` installs the pack (the install is kept, as an import from the driver manager is) and runs the
 * launch on it; `armsx2.driver` is then not needed. The log line "launch extras:" says what was taken, and
 * "launch driver:" which driver the launch resolved to.
 */
object LaunchExtras {
    private const val TAG = "LaunchExtras"

    const val PINE = "armsx2.pine"
    const val PINE_SLOT = "armsx2.pine_slot"
    const val DRIVER = "armsx2.driver"
    const val DRIVER_IMPORT = "armsx2.driver_import"
    const val SYSTEM_DRIVER = "system"

    @Volatile var pine: Boolean? = null
        private set
    @Volatile var pineSlot: Int? = null
        private set
    @Volatile private var driver: String? = null
    @Volatile private var driverImport: String? = null

    /** Takes the extras of a launch intent, replacing those of the launch before. */
    fun take(intent: Intent?) {
        val extras = intent?.extras
        pine = if (extras?.containsKey(PINE) == true) readBoolean(intent, PINE) else null
        pineSlot = if (extras?.containsKey(PINE_SLOT) == true) readInt(intent, PINE_SLOT) else null
        driver = intent?.getStringExtra(DRIVER)?.trim()?.takeIf { it.isNotEmpty() }
        driverImport = intent?.getStringExtra(DRIVER_IMPORT)?.trim()?.takeIf { it.isNotEmpty() }
        if (pine != null || pineSlot != null || driver != null || driverImport != null)
            Log.i(TAG, "launch extras: pine=$pine pine_slot=$pineSlot driver=$driver driver_import=$driverImport")
    }

    fun clear() {
        pine = null
        pineSlot = null
        driver = null
        driverImport = null
    }

    /**
     * The driver this launch runs on when the extras name one: a pair of (true, driver), where a null driver is the
     * system driver. (false, null) when the extras say nothing about the driver, and the settings decide. An import
     * that fails, or a name that matches no installed driver, falls back to the settings and says so in the log.
     * Runs on the VM launch thread: an import reads and unpacks the zip.
     */
    fun resolveDriver(context: Context, installed: List<CustomDriver.InstalledDriver>): Pair<Boolean, CustomDriver.InstalledDriver?> {
        driverImport?.let { path ->
            driverImport = null
            val imported = CustomDriver.installFromUri(context, Uri.fromFile(File(path)))
            if (imported != null) {
                driver = imported.id
                Log.i(TAG, "launch driver: imported '$path' as ${imported.id}")
                return true to imported
            }
            Log.w(TAG, "launch driver: could not import '$path'; the settings decide")
        }
        val wanted = driver ?: return false to null
        if (wanted.equals(SYSTEM_DRIVER, ignoreCase = true)) {
            Log.i(TAG, "launch driver: system")
            return true to null
        }
        val match = installed.firstOrNull { it.id == wanted }
            ?: installed.firstOrNull { it.name.equals(wanted, ignoreCase = true) }
        if (match == null) {
            Log.w(TAG, "launch driver: no installed driver is '$wanted' (installed: " +
                installed.joinToString { "${it.id} '${it.name}'" } + "); the settings decide")
            return false to null
        }
        Log.i(TAG, "launch driver: ${match.id} '${match.name}'")
        return true to match
    }

    // `am start --ez` sends a boolean, `--es` a string, `--ei` an int; take any of them.
    @Suppress("DEPRECATION")
    private fun readBoolean(intent: Intent, key: String): Boolean? =
        when (val v = intent.extras?.get(key)) {
            is Boolean -> v
            is String -> v.trim().lowercase().let { it == "true" || it == "1" || it == "on" }
            is Int -> v != 0
            else -> null
        }

    @Suppress("DEPRECATION")
    private fun readInt(intent: Intent, key: String): Int? =
        when (val v = intent.extras?.get(key)) {
            is Int -> v
            is Long -> v.toInt()
            is String -> v.trim().toIntOrNull()
            else -> null
        }
}
