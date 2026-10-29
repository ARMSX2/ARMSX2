// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import android.content.Context
import android.net.ConnectivityManager
import android.net.wifi.WifiInfo
import android.net.wifi.WifiManager
import android.os.Build

/**
 * Keeps the Wi-Fi radio awake while a stream runs. A radio in power saving only picks up waiting
 * packets every 100 ms or so, which shows up as delay spikes in the picture on one side and in
 * player 2's input on the other. Held from the start of a session to its end and never otherwise,
 * so it costs nothing when remote play is not in use. Android only honours it while the app is in
 * front with the screen on, which is exactly when a stream runs.
 */
internal class RemotePlayWifi(context: Context) {
    private val lock = runCatching {
        (context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager)
            .createWifiLock(
                // The low-latency mode is new in Android 10. Before it, high performance is the
                // closest: it keeps the radio out of power saving too.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                } else {
                    @Suppress("DEPRECATION")
                    WifiManager.WIFI_MODE_FULL_HIGH_PERF
                },
                "armsx2:remoteplay",
            )
            .apply { setReferenceCounted(false) }
    }.getOrNull()

    fun hold() {
        runCatching { lock?.acquire() }
    }

    fun release() {
        runCatching { if (lock?.isHeld == true) lock.release() }
    }
}

/**
 * This device's Wi-Fi link, for the stream's stats: the band (from the channel frequency), the
 * link speeds and the signal. Periodic stalls with packet loss are what a weak or crowded link
 * gives, a 2.4 GHz one most of all (Bluetooth shares that band too), so the stats show it beside
 * the stream's own numbers. Read through the network's capabilities, which needs no permission
 * beyond the network state the app already has. Null when not on Wi-Fi.
 */
internal class WifiLink(val frequencyMhz: Int, val txMbps: Int, val rxMbps: Int, val rssiDbm: Int) {
    fun describe(): String {
        val band = when {
            frequencyMhz >= 5925 -> "6 GHz"
            frequencyMhz >= 4900 -> "5 GHz"
            frequencyMhz > 0 -> "2.4 GHz"
            else -> "?"
        }
        return "wifi $band ${txMbps.coerceAtLeast(0)}/${rxMbps.coerceAtLeast(0)} Mbps $rssiDbm dBm"
    }

    companion object {
        fun read(context: Context): WifiLink? {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return null
            return runCatching {
                val connectivity = context.applicationContext.getSystemService(ConnectivityManager::class.java)
                val info = connectivity.getNetworkCapabilities(connectivity.activeNetwork)?.transportInfo as? WifiInfo
                info?.let { WifiLink(it.frequency, it.txLinkSpeedMbps, it.rxLinkSpeedMbps, it.rssi) }
            }.getOrNull()
        }
    }
}
