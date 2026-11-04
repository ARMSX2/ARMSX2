// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import android.content.Context
import android.os.Process
import com.armsx2.CustomDriver
import com.armsx2.DiscIdentity
import com.armsx2.EmuState
import com.armsx2.GameInfo
import com.armsx2.config.ConfigStore
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp

/**
 * Works out what a report is about from the app's state at the moment the user asks for it. Each
 * entry point has one function here, so the screens that start the flow only say where they are.
 */
object ReportOrigins {
    /** The game that is running now, with a save state and GS dump available. */
    fun running(context: Context): ReportOrigin {
        val current = MainActivityRuntime.currentGame.value
        val serial = runCatching { NativeApp.getGameSerial() }.getOrNull().orEmpty().ifBlank { current?.serial.orEmpty() }
        // The running VM reports 0 for some ISO and CHD boots. The marker holds the CRC the app
        // worked out from the image before booting, which is the same game's.
        val crc = ReportCrc.firstKnown(
            runCatching { NativeApp.getGameCRC() }.getOrNull(),
            CrashMarker.read(context)?.takeIf { it.pid == Process.myPid() }?.crc,
        )
        val title = current?.title ?: runCatching { NativeApp.getPauseGameTitle() }.getOrNull().orEmpty()
        val game = if (serial.isBlank() && title.isBlank()) null
        else ReportGame(serial, crc, title, current?.settingsKey ?: serial.ifBlank { null })
        val driverId = MainActivityRuntime.customDriverId.value.orEmpty()
        return ReportOrigin(
            game = game,
            canCapture = vmActive(),
            driverId = driverId,
            driverName = driverName(context, driverId),
            perfAtOpen = runCatching { NativeApp.getPerfSnapshot() }.getOrNull(),
        )
    }

    /** From the drawer: the running game if there is one, otherwise no game at all. */
    fun general(context: Context): ReportOrigin {
        if (vmActive() && MainActivityRuntime.eState.value != EmuState.STOPPED) return running(context)
        val global = ConfigStore.loadGlobal()
        val driverId = effectiveDriverId(context, global.output.customDriverId, global.output.customDriverId)
        return ReportOrigin(game = null, canCapture = false, driverId = driverId, driverName = driverName(context, driverId))
    }

    /** From a library game's menu: that game, and the driver its settings would run it on. No capture. */
    suspend fun libraryGame(context: Context, info: GameInfo): ReportOrigin {
        val serial = info.serial.orEmpty()
        val crc = ReportCrc.normalize(runCatching { DiscIdentity.resolve(info.uri, info.serial) }.getOrNull())
        val resolved = ConfigStore.resolveForGame(info.settingsKey)
        val global = ConfigStore.loadGlobal()
        val driverId = effectiveDriverId(context, resolved.output.customDriverId, global.output.customDriverId)
        return ReportOrigin(
            game = ReportGame(serial, crc, info.title, info.settingsKey),
            canCapture = false,
            driverId = driverId,
            driverName = driverName(context, driverId),
        )
    }

    /** From the crash prompt or an unsent line in "My reports". */
    fun crash(pending: PendingCrash): ReportOrigin = ReportOrigin(
        game = if (pending.serial.isBlank() && pending.title.isBlank()) null
        else ReportGame(pending.serial, ReportCrc.normalize(pending.marker.crc), pending.title, pending.serial.ifBlank { null }),
        canCapture = false,
        driverId = pending.marker.driverId,
        driverName = pending.marker.driverName,
        crashDir = pending.dir,
        crashMarker = pending.marker,
    )

    /**
     * From the "the game could not start" notice: the game and driver it tried, nothing to capture.
     * The logs are the failed boot's own, which the next boot has not overwritten yet.
     */
    fun bootFailure(context: Context, failure: BootFailure): ReportOrigin = ReportOrigin(
        game = failure.game,
        canCapture = false,
        driverId = failure.driverId,
        driverName = driverName(context, failure.driverId),
    )

    private fun vmActive(): Boolean = runCatching { NativeApp.hasActiveVM() }.getOrDefault(false)

    private fun effectiveDriverId(context: Context, gameId: String, globalId: String): String =
        CustomDriver.effectiveId(gameId, globalId, CustomDriver.listInstalled(context).map { it.id }).orEmpty()

    private fun driverName(context: Context, id: String): String =
        if (id.isBlank()) "" else CustomDriver.listInstalled(context).firstOrNull { it.id == id }?.name.orEmpty()
}
