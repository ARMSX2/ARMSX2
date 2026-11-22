// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+
package com.armsx2

import android.content.Context
import androidx.compose.runtime.mutableStateOf
import com.armsx2.config.ConfigStore
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Offers the newest release of the ARMSX2 GPU driver for this GPU once per app version, to anyone
 * whose global driver is not that release: players on the system driver, on an older malisx2 or
 * armsx2-turnip, or on someone else's pack. People update the app and never revisit the driver
 * list, so without this they run whatever driver they installed months ago.
 *
 * The release list is read only on the first launch of a new version, never in the background:
 * the newest driver then is the one this app version was released beside, so the offer cannot
 * run ahead of the emulator. A fresh install is asked by the setup wizard's driver page instead,
 * and finishing setup counts as the answer for that version.
 */
object DriverUpdateOffer {
    /** The versionCode the offer was last settled for: answered, or nothing to offer. */
    private const val KEY_SETTLED_VERSION = "driver.offerSettledVersion"

    enum class Stage { Offer, Installing, Installed, Failed }

    data class Offer(
        val pack: CustomDriver.RemoteDriver,
        /** Name of the global driver in use now, or null for the system driver. */
        val currentName: String?,
        /** The driver in use is an older release of the offered one, rather than another driver. */
        val upgrade: Boolean,
        /** The offer is malisx2, which can need a newer Mali kernel driver than the device has. */
        val mali: Boolean,
        val stage: Stage = Stage.Offer,
    )

    /** The offer on screen, or null. Shown by DriverUpdateOverlay while no game is running. */
    val pending = mutableStateOf<Offer?>(null)

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    /** Set when this process has looked. An unreachable release list is retried at the next launch. */
    private var looked = false

    /** Whether this launch should look for an offer. */
    internal fun due(settledVersion: Int, versionCode: Int): Boolean = settledVersion != versionCode

    /** Whether [newest] is worth offering to a player whose global driver id is [globalId] (blank
     *  for the system driver). */
    internal fun offers(globalId: String, newest: CustomDriver.RemoteDriver?): Boolean =
        newest != null && newest.id != globalId

    /** Records that this version needs no more asking. */
    fun settle() {
        MainActivityRuntime.prefs.edit().putInt(KEY_SETTLED_VERSION, BuildConfig.VERSION_CODE).apply()
    }

    /** Looks for an offer, once per process. Call when the library is on screen with no game running. */
    fun lookOnce(context: Context) {
        if (looked) return
        looked = true
        if (!due(MainActivityRuntime.prefs.getInt(KEY_SETTLED_VERSION, 0), BuildConfig.VERSION_CODE)) return
        val app = context.applicationContext
        scope.launch {
            val offer = withContext(Dispatchers.IO) { find(app) }
            if (offer != null) pending.value = offer
        }
    }

    /** Blocks on the network. Settles the version itself when there is nothing to offer. */
    private fun find(context: Context): Offer? {
        val renderer = GpuInfo.rendererName()
        if (CustomDriver.setupOfferLabel(renderer) == null) {
            settle()
            return null
        }
        // Null here is an unreachable release list, not "no driver": leave it unsettled.
        val newest = CustomDriver.fetchSetupOffer(renderer) ?: return null
        val globalId = runCatching { ConfigStore.loadGlobal().output.customDriverId }.getOrNull() ?: return null
        if (!offers(globalId, newest)) {
            settle()
            return null
        }
        val current = CustomDriver.listInstalled(context).firstOrNull { it.id == globalId }
        return Offer(
            pack = newest,
            currentName = current?.name,
            upgrade = current != null && CustomDriver.isFromSource(current.id, newest.source),
            mali = CustomDriver.offersMaliSX2(renderer),
        )
    }

    fun decline() {
        val stage = pending.value?.stage
        if (stage == Stage.Installing) return
        settle()
        pending.value = null
    }

    /** OK after the install. */
    fun close() {
        pending.value = null
    }

    fun install(context: Context) {
        val offer = pending.value ?: return
        if (offer.stage != Stage.Offer && offer.stage != Stage.Failed) return
        settle()
        pending.value = offer.copy(stage = Stage.Installing)
        val app = context.applicationContext
        scope.launch {
            val installed = CustomDriver.installAsGlobal(app, offer.pack)
            pending.value = pending.value?.copy(stage = if (installed != null) Stage.Installed else Stage.Failed)
        }
    }
}
