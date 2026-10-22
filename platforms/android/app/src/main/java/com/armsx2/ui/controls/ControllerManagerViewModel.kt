package com.armsx2.ui.controls

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.input.ControllerMappings
import com.armsx2.input.PadRouter
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

enum class ControllerSection { Buttons, Profiles, Hotkeys }

data class ControllerManagerUiState(
    val player: Int = 0,
    val section: ControllerSection = ControllerSection.Buttons,
    val capturingAction: ControllerMappings.Action? = null,
    val tick: Int = 0,
    val rumble: Boolean = true,
    val multitap: Boolean = false,
    val dpadAsStick: Boolean = false,
    val allToPlayer1: Boolean = false,
    val pads: List<PadRouter.Pad> = emptyList(),
    val pins: Map<String, Int> = emptyMap(),
    val profiles: List<String> = emptyList(),
    /** The profile last loaded, and into which player, so its row shows as the current one. */
    val loadedProfile: Pair<Int, String>? = null,
)

class ControllerManagerViewModel(application: Application) : AndroidViewModel(application) {
    var state = androidx.compose.runtime.mutableStateOf(ControllerManagerUiState())
        private set

    fun refresh() {
        state.value = state.value.copy(
            tick = state.value.tick + 1,
            rumble = ControllerMappings.rumbleEnabled(),
            multitap = ControllerMappings.multitapEnabled(),
            dpadAsStick = ControllerMappings.dpadAsLeftStick(),
            allToPlayer1 = PadRouter.allToPlayer1,
            pads = PadRouter.connectedPads(),
            pins = PadRouter.pins(),
        )
        refreshProfiles()
    }

    // listProfiles() also reads the portable inputprofiles/ folder, so it stays off the main thread.
    private fun refreshProfiles() {
        viewModelScope.launch {
            val names = withContext(Dispatchers.IO) { ControllerMappings.listProfiles() }
            state.value = state.value.copy(profiles = names)
        }
    }

    fun setAllToPlayer1(on: Boolean) {
        PadRouter.setAllToPlayer1(on)
        refresh()
    }

    fun setPin(descriptor: String, port: Int?) {
        PadRouter.setPin(descriptor, port)
        refresh()
    }

    // The running game, when it has its own map for [player]. That map shadows the global one
    // key by key, so a profile loaded globally would change nothing in this game.
    private fun gameTier(player: Int): String? =
        MainActivityRuntime.currentGame.value?.serial
            ?.takeIf { it.isNotEmpty() && ControllerMappings.hasGameOverrides(it, player) }

    /** Load [name] into the player being edited, at once and mid-game: into the global map,
     *  or into the running game's own map when it has one. */
    fun loadProfile(name: String) {
        cancelCapture()
        val player = state.value.player
        if (ControllerMappings.applyProfile(name, player, gameTier(player))) {
            state.value = state.value.copy(loadedProfile = player to name)
        }
        refresh()
    }

    /** Save the map the edited player is using now under [name]. False for a blank name. */
    fun saveProfile(name: String): Boolean {
        val saved = ControllerMappings.saveProfile(name, state.value.player, gameTier(state.value.player))
        if (saved) {
            state.value = state.value.copy(loadedProfile = state.value.player to name.trim())
            refreshProfiles()
        }
        return saved
    }

    fun setPlayer(player: Int) {
        cancelCapture()
        state.value = state.value.copy(player = player.coerceIn(0, 1))
    }

    fun setSection(section: ControllerSection) {
        cancelCapture()
        state.value = state.value.copy(section = section)
    }

    fun beginCapture(action: ControllerMappings.Action) {
        ControllerMappings.padCapturing.value = true
        // Bind via the Activity's dispatchKeyEvent (see ControllerMappings.capturePadAction) so any
        // button binds even though the bind prompt is no longer a focus-stealing dialog.
        ControllerMappings.capturePadAction.value = { kc -> captureKey(kc) }
        state.value = state.value.copy(capturingAction = action)
    }

    fun captureKey(keyCode: Int): Boolean {
        val action = state.value.capturingAction ?: return false
        ControllerMappings.bind(action, keyCode, state.value.player, gameTier(state.value.player))
        ControllerMappings.padCapturing.value = false
        ControllerMappings.capturePadAction.value = null
        state.value = state.value.copy(capturingAction = null, tick = state.value.tick + 1, loadedProfile = null)
        return true
    }

    fun cancelCapture() {
        ControllerMappings.padCapturing.value = false
        ControllerMappings.capturePadAction.value = null
        state.value = state.value.copy(capturingAction = null)
    }

    fun clear(action: ControllerMappings.Action) {
        ControllerMappings.clearAction(action, state.value.player, gameTier(state.value.player))
        state.value = state.value.copy(loadedProfile = null)
        refresh()
    }

    fun resetPlayer() {
        val player = state.value.player
        val serial = gameTier(player)
        if (serial != null) ControllerMappings.clearGameOverrides(serial, player)
        else ControllerMappings.reset(player)
        state.value = state.value.copy(loadedProfile = null)
        refresh()
    }

    fun beginHotkeyCapture(hotkey: ControllerMappings.SysHotkey) {
        ControllerMappings.beginHotkeyCapture(hotkey)
        state.value = state.value.copy(tick = state.value.tick + 1)
    }

    fun clearHotkey(hotkey: ControllerMappings.SysHotkey) {
        ControllerMappings.clearHotkey(hotkey)
        refresh()
    }

    fun setRumble(value: Boolean) {
        ControllerMappings.setRumbleEnabled(value)
        refresh()
    }

    fun setMultitap(value: Boolean) {
        ControllerMappings.setMultitapEnabledForRunningGame(value)
        refresh()
    }

    fun setDpadAsStick(value: Boolean) {
        ControllerMappings.setDpadAsLeftStickForRunningGame(value)
        refresh()
    }

    override fun onCleared() {
        cancelCapture()
        super.onCleared()
    }
}

