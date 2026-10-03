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
    /** What is in the folder of the game whose imports are open (see [ArcadeViewModel.showFiles]). */
    val files: ArcadeLibrary.GameFiles? = null,
    /** The game being imported, while it is. */
    val importing: ArcadeLibrary.Title? = null,
    /** The game being uninstalled, while it is. */
    val removing: ArcadeLibrary.Title? = null,
    val message: String? = null,
    val error: String? = null,
)

class ArcadeViewModel(application: Application) : AndroidViewModel(application) {
    var state = mutableStateOf(ArcadeUiState())
        private set

    // Apart from [state]: written by the copy on its own thread, many times a second.
    val downloadProgress = mutableFloatStateOf(0f)
    val importProgress = mutableFloatStateOf(0f)
    val removeProgress = mutableFloatStateOf(0f)

    // Set by the progress window's Cancel, read by the copy at every megabyte.
    private val cancelRequested = java.util.concurrent.atomic.AtomicBoolean(false)

    private var refreshJob: Job? = null

    /** The game whose imports are open, whose files [refresh] looks at too. */
    private var filesOf: String? = null

    /** Opens the imports of the game [id] (null: closes them): what of it is in the folder, kept current. */
    fun showFiles(id: String?) {
        filesOf = id
        state.value = state.value.copy(files = null)
        if (id != null) refresh()
    }

    fun refresh() {
        // The newest look wins: one still reading from before a change must not land after it.
        refreshJob?.cancel()
        refreshJob = viewModelScope.launch {
            val app = getApplication<Application>()
            val filesId = filesOf
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
                    files = filesId?.let { ArcadeLibrary.files(app, it) },
                )
            }
            // Keep whatever started while this was reading (a download, an import), and the files only
            // while they are still the open game's.
            state.value = next.copy(
                files = next.files?.takeIf { it.id == filesOf },
                downloading = state.value.downloading,
                importing = state.value.importing,
                removing = state.value.removing,
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

    /** Imports one part of [title] from [source]. Done, its imports show it in; a failure says why. */
    fun import(title: ArcadeLibrary.Title, part: ArcadeLibrary.Part, source: Uri) {
        if (state.value.importing != null || state.value.removing != null) return
        state.value = state.value.copy(importing = title)
        importProgress.floatValue = 0f
        cancelRequested.set(false)
        viewModelScope.launch {
            val result = ArcadeLibrary.import(
                getApplication(), title, part, source,
                onProgress = { p -> importProgress.floatValue = p },
                stop = cancelRequested::get,
            )
            // A cancelled import says nothing: the player asked for it, and nothing of it is left.
            val failure = result.exceptionOrNull()?.takeIf { it !is ArcadeLibrary.ImportCancelled }
            state.value = state.value.copy(
                importing = null,
                error = failure?.let { e -> I18n.get("arcade.import.failed").format(e.message ?: e.toString()) },
            )
            // The library shows the game without a tap on its refresh button.
            if (result.isSuccess) LibraryRefresh.request()
            refresh()
        }
    }

    /** Stops the import in progress at its next megabyte, leaving nothing of it behind. */
    fun cancelImport() {
        cancelRequested.set(true)
    }

    /** Uninstalls [title] from the arcade folder (ArcadeLibrary.uninstall says what goes and what stays). */
    fun uninstall(title: ArcadeLibrary.Title) {
        if (state.value.importing != null || state.value.removing != null) return
        state.value = state.value.copy(removing = title)
        removeProgress.floatValue = 0f
        viewModelScope.launch {
            val result = ArcadeLibrary.uninstall(getApplication(), title.id) { p -> removeProgress.floatValue = p }
            state.value = state.value.copy(
                removing = null,
                message = if (result.isSuccess) I18n.get("arcade.uninstall.done").format(title.name) else null,
                error = result.exceptionOrNull()?.let { e ->
                    I18n.get("arcade.uninstall.failed").format(e.message ?: e.toString())
                },
            )
            // Gone from the library too, without a tap on its refresh button; and a failure part way
            // still changed the folder.
            LibraryRefresh.request()
            refresh()
        }
    }

    fun dismissMessage() {
        state.value = state.value.copy(message = null, error = null)
    }
}
