// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.input

import android.content.Context
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat

/**
 * RECORD_AUDIO for the emulated PS2 mic. The SingStar device reads the host's built-in microphone
 * through Oboe (pcsx2/USB/usb-mic/audiodev-oboe.cpp), so attaching a mic port needs this permission.
 * It is declared in AndroidManifest.xml and requested here, only when the user explicitly attaches a
 * SingStar port — nothing else in the app touches the microphone, and online play's WebRTC path never
 * opens one either (RemotePlayRtc.kt).
 */
internal object Microphone {
    const val PERMISSION = "android.permission.RECORD_AUDIO"

    fun granted(context: Context): Boolean =
        ContextCompat.checkSelfPermission(context, PERMISSION) == PackageManager.PERMISSION_GRANTED
}

/**
 * Asks for the microphone permission where it is needed and not held yet, then calls back with
 * whether the microphone can be used. Calls back at once where there is nothing to ask.
 */
@Composable
internal fun rememberRecordAudioAccess(): (onResult: (Boolean) -> Unit) -> Unit {
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
            if (Microphone.granted(context)) {
                onResult(true)
            } else {
                waiting[0] = onResult
                runCatching { launcher.launch(Microphone.PERMISSION) }.onFailure {
                    waiting[0] = null
                    onResult(false)
                }
            }
        }
    }
}
