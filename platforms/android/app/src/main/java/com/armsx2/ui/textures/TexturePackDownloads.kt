package com.armsx2.ui.textures

import android.content.Context
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import com.armsx2.TextureCatalog
import com.armsx2.TexturePackInstaller
import com.armsx2.i18n.I18n
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * The texture pack being installed, one at a time, for as long as the app runs. It used to belong to
 * the catalog list on the texture screen; now the packs are in a browser that can be closed and
 * opened again mid-download, so the download outlives it, and the texture screen's card shows it too.
 */
internal object TexturePackDownloads {
    /** The pack downloading, or null. */
    val busyPackId = mutableStateOf<String?>(null)
    val progressText = mutableStateOf("")
    val progressFraction = mutableFloatStateOf(0f)

    /** True once the installer reports Installing: the commit (directory swap and state write) is a
     *  short synchronous transaction, and cancelling it halfway means nothing, so Cancel goes. */
    val commitStarted = mutableStateOf(false)

    /** How the last download ended, shown until the next one starts. */
    val status = mutableStateOf("")

    /** Goes up each time a pack finishes installing, for the texture screen's installed list. */
    val installedCount = mutableIntStateOf(0)

    @Volatile private var cancelRequested = false

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    /** Installs [pack] into [targetSerial]'s folder: one of the pack's own serials (see
     *  [TextureCatalog.Pack.installSerialFor]), so it works with no game running and cannot drop a
     *  pack into another game's folder. Does nothing while another is going. */
    fun start(context: Context, pack: TextureCatalog.Pack, targetSerial: String) {
        if (busyPackId.value != null) return
        val app = context.applicationContext
        busyPackId.value = pack.id
        cancelRequested = false
        commitStarted.value = false
        status.value = ""
        progressFraction.floatValue = 0f
        progressText.value = I18n.get("textures.online.starting")
        scope.launch {
            val outcome = withContext(Dispatchers.IO) {
                TexturePackInstaller.install(
                    app, pack, targetSerial,
                    onProgress = { p ->
                        when (p) {
                            is TexturePackInstaller.Progress.Downloading -> {
                                progressFraction.floatValue = if (p.total > 0) p.read.toFloat() / p.total else 0f
                                progressText.value = I18n.get("textures.online.downloading")
                                    .replace("%1s", packMb(p.read)).replace("%2s", packMb(p.total))
                            }
                            TexturePackInstaller.Progress.Verifying ->
                                progressText.value = I18n.get("textures.online.verifying")
                            is TexturePackInstaller.Progress.Extracting -> {
                                progressFraction.floatValue = if (p.total > 0) p.done.toFloat() / p.total else 0f
                                progressText.value = I18n.get("textures.online.extracting")
                                    .replace("%1s", p.done.toString()).replace("%2s", p.total.toString())
                            }
                            TexturePackInstaller.Progress.Installing -> {
                                commitStarted.value = true
                                progressText.value = I18n.get("textures.online.installing")
                            }
                        }
                    },
                    isCancelled = { cancelRequested },
                )
            }
            busyPackId.value = null
            progressText.value = ""
            status.value = when {
                outcome.ok -> I18n.get("textures.online.done")
                outcome.error == null -> I18n.get("textures.online.cancelled")
                else -> outcome.error
            }
            if (outcome.ok) {
                installedCount.intValue++
                // For Popular Today: an install the player chose, counted anonymously.
                TexturePackStats.countDownload(pack.id)
            }
        }
    }

    /** Stops the download at its next check, unless it is already committing. */
    fun cancel() {
        if (!commitStarted.value) cancelRequested = true
    }
}

/** A pack's size the way the catalog has always shown it: "241 MB", "1.2 GB". */
internal fun packMb(bytes: Long): String = when {
    bytes >= 1024L * 1024 * 1024 -> String.format(java.util.Locale.US, "%.1f GB", bytes / 1024.0 / 1024 / 1024)
    else -> "${bytes / 1024 / 1024} MB"
}
