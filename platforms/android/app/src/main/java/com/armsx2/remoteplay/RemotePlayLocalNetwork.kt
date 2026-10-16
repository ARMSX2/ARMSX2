// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat

/**
 * Android 17's local network permission. An app that targets Android 17, as this one does, cannot
 * reach the local network without ACCESS_LOCAL_NETWORK: it finds no host on the Wi-Fi, connects to
 * no device on it in either direction, and cannot ask the router to open a port. The internet is
 * not affected, so online play between two networks works either way. The permission is in the
 * Nearby devices group, so a player who allowed Bluetooth before is not asked again.
 */
internal object RemotePlayLocalNetwork {
    const val PERMISSION = "android.permission.ACCESS_LOCAL_NETWORK"

    // Android 17, by number: older SDKs have no name for it.
    private const val FIRST_SDK = 37

    fun granted(context: Context): Boolean =
        Build.VERSION.SDK_INT < FIRST_SDK ||
            ContextCompat.checkSelfPermission(context, PERMISSION) == PackageManager.PERMISSION_GRANTED
}

/**
 * Asks for the local network permission where it is needed and not held yet, then calls back with
 * whether the local network can be used. Calls back at once where there is nothing to ask.
 */
@Composable
internal fun rememberLocalNetworkAccess(): (onResult: (Boolean) -> Unit) -> Unit {
    val context = LocalContext.current
    // The callback waiting for the system's answer. Not state: holding it must not compose again.
    val waiting = remember { arrayOfNulls<(Boolean) -> Unit>(1) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        val callback = waiting[0]
        waiting[0] = null
        callback?.invoke(granted)
    }
    return remember(context, launcher) {
        { onResult ->
            if (RemotePlayLocalNetwork.granted(context)) {
                onResult(true)
            } else {
                waiting[0] = onResult
                runCatching { launcher.launch(RemotePlayLocalNetwork.PERMISSION) }.onFailure {
                    waiting[0] = null
                    onResult(false)
                }
            }
        }
    }
}
