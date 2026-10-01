// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.arcade

import android.app.Application
import android.net.Uri
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.arcade.ArcadeLibrary
import com.armsx2.data.library.LibraryRefresh
import com.armsx2.i18n.I18n
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class ArcadeUiState(
    val loaded: Boolean = false,
    val folderName: String? = null,
    /** How many games the downloaded boot files cover, 0 when there are none. */
    val bootGames: Int = 0,
    val downloading: Boolean = false,
    /** The arcade BIOS file the core will boot with, or null for none. */
    val bios: String? = null,
    /** The games that can be imported: known to the database and in the boot files. */
    val titles: List<ArcadeLibrary.Title> = emptyList(),
    val installed: List<ArcadeLibrary.Installed> = emptyList(),
    /** The game being imported, while it is. */
    val importing: ArcadeLibrary.Title? = null,
    val message: String? = null,
    val error: String? = null,
)

class ArcadeViewModel(application: Application) : AndroidViewModel(application) {
    var state = mutableStateOf(ArcadeUiState())
        private set

    // Apart from [state]: written by the copy on its own thread, many times a second.
    val downloadProgress = mutableFloatStateOf(0f)
    val importProgress = mutableFloatStateOf(0f)

    private var refreshJob: Job? = null

    fun refresh() {
        // The newest look wins: one still reading from before a change must not land after it.
        refreshJob?.cancel()
        refreshJob = viewModelScope.launch {
            val app = getApplication<Application>()
            val next = withContext(Dispatchers.IO) {
                val boot = ArcadeLibrary.bootGames(app)
                val titles = ArcadeLibrary.titles()
                state.value.copy(
                    loaded = true,
                    folderName = ArcadeLibrary.folderName(app),
                    bootGames = boot.size,
                    bios = ArcadeLibrary.biosName(app),
                    titles = titles.filter { it.id in boot },
                    installed = ArcadeLibrary.installed(app, titles),
                )
            }
            // Keep whatever started while this was reading (a download, an import).
            state.value = next.copy(
                downloading = state.value.downloading,
                importing = state.value.importing,
                message = state.value.message,
                error = state.value.error,
            )
        }
    }

    fun chooseFolder(uri: Uri) {
        ArcadeLibrary.setFolder(getApplication(), uri)
        // A folder new to the library, or one with games in it already: the library scans again.
        LibraryRefresh.request()
        refresh()
    }

    fun downloadBootFiles() {
        if (state.value.downloading) return
        state.value = state.value.copy(downloading = true)
        downloadProgress.floatValue = 0f
        viewModelScope.launch {
            val ok = ArcadeLibrary.downloadBootFiles(getApplication()) { p -> downloadProgress.floatValue = p }
            state.value = state.value.copy(
                downloading = false,
                error = if (ok) null else I18n.get("arcade.boot.failed"),
            )
            refresh()
        }
    }

    fun import(title: ArcadeLibrary.Title, image: Uri, dongle: Uri, card: Uri?) {
        if (state.value.importing != null) return
        state.value = state.value.copy(importing = title)
        importProgress.floatValue = 0f
        viewModelScope.launch {
            val result = ArcadeLibrary.import(getApplication(), title, image, dongle, card) { p -> importProgress.floatValue = p }
            state.value = state.value.copy(
                importing = null,
                message = if (result.isSuccess) I18n.get("arcade.import.done").format(title.name) else null,
                error = result.exceptionOrNull()?.let { e ->
                    I18n.get("arcade.import.failed").format(e.message ?: e.toString())
                },
            )
            // The library shows the game without a tap on its refresh button.
            if (result.isSuccess) LibraryRefresh.request()
            refresh()
        }
    }

    fun dismissMessage() {
        state.value = state.value.copy(message = null, error = null)
    }
}
