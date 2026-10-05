// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.arcade

import android.app.Application
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.arcade.Arcade
import com.armsx2.arcade.ArcadeLibrary
import com.armsx2.data.library.GameLibraryRepository
import com.armsx2.data.library.LibraryRefresh
import com.armsx2.i18n.I18n
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** An arcade game the library found, what still keeps it from starting (empty: ready to play), and
 *  whether it only reaches its attract demo so far (Arcade.ATTRACT_ONLY). */
data class ArcadeGame(val title: String, val id: String, val missing: List<String>, val attractOnly: Boolean = false)

data class ArcadeUiState(
    val loaded: Boolean = false,
    /** How many games the downloaded boot files cover, 0 when there are none. */
    val bootGames: Int = 0,
    val downloading: Boolean = false,
    /** The arcade BIOS there, all in use (the System 256 one first, BiosTools' FindArcadeBiosFor), or null. */
    val bios: String? = null,
    /** The arcade games in the player's game folders, as the library found them last. */
    val games: List<ArcadeGame> = emptyList(),
    /** While the game folders are looked through again. */
    val looking: Boolean = false,
    val error: String? = null,
)

class ArcadeViewModel(application: Application) : AndroidViewModel(application) {
    var state = mutableStateOf(ArcadeUiState())
        private set

    // Apart from [state]: written by the download on its own thread, many times a second.
    val downloadProgress = mutableFloatStateOf(0f)

    private var refreshJob: Job? = null

    /** Reads everything the screen shows again: the boot files, the BIOS, and the arcade games the
     *  library found, each checked for what it still lacks. */
    fun refresh() {
        // The newest look wins: one still reading from before a change must not land after it.
        refreshJob?.cancel()
        refreshJob = viewModelScope.launch {
            val app = getApplication<Application>()
            val next = withContext(Dispatchers.IO) {
                state.value.copy(
                    loaded = true,
                    bootGames = ArcadeLibrary.bootGames(app).size,
                    bios = ArcadeLibrary.biosNames(app),
                    games = arcadeGames(GameLibraryRepository(app).loadCached().games),
                )
            }
            // Keep whatever started while this was reading (a download, a look through the folders).
            state.value = next.copy(
                downloading = state.value.downloading,
                looking = state.value.looking,
                error = state.value.error,
            )
        }
    }

    private fun arcadeGames(games: List<com.armsx2.GameInfo>): List<ArcadeGame> {
        val app = getApplication<Application>()
        return games.filter { it.extension == Arcade.BADGE }.map { game ->
            val id = game.serial.orEmpty()
            ArcadeGame(game.title, id, Arcade.missing(app, game.uri.toString()), id.uppercase() in Arcade.ATTRACT_ONLY)
        }.sortedBy { it.title.lowercase() }
    }

    /** Looks through the game folders again, for games put there since the library last did; the
     *  library itself looks again too, the next time it is on screen. */
    fun lookAgain() {
        if (state.value.looking) return
        val directories = MainActivityRuntime.romsDirs.value
        state.value = state.value.copy(looking = true)
        viewModelScope.launch {
            runCatching { GameLibraryRepository(getApplication()).scan(directories) }
            LibraryRefresh.request()
            state.value = state.value.copy(looking = false)
            refresh()
        }
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

    fun dismissMessage() {
        state.value = state.value.copy(error = null)
    }
}
