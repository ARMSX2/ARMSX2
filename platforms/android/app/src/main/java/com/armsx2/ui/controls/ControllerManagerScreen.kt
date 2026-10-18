package com.armsx2.ui.controls

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.armsx2.input.ControllerMappings
import com.armsx2.i18n.str
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.common.SectionTitle
import com.armsx2.ui.common.SettingSwitchRow
import com.armsx2.ui.settings.controllerFocusable

@Composable
fun ControllerManagerScreen(onBack: () -> Unit, viewModel: ControllerManagerViewModel = viewModel()) {
    val state = viewModel.state.value
    LaunchedEffect(Unit) { viewModel.refresh() }
    DisposableEffect(viewModel) { onDispose(viewModel::cancelCapture) }

    ArmsBackdrop {
        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .onPreviewKeyEvent { event ->
                    if (state.capturingAction != null && event.type == KeyEventType.KeyDown) {
                        viewModel.captureKey(event.nativeKeyEvent.keyCode)
                    } else false
                },
        ) {
            ArmsTopBar(
                title = str("tab.controls"),
                subtitle = str("pad.editing.description"),
                leading = { RoundAction("←", str("action.back"), onBack) },
                actions = { RoundAction("↺", str("action.reset"), viewModel::resetPlayer) },
            )
            BoxWithConstraints(Modifier.fillMaxWidth()) {
                val compact = maxWidth < 840.dp
                if (compact) {
                    Column(Modifier.fillMaxWidth().padding(horizontal = 8.dp)) {
                        ControllerOptions(state, viewModel, Modifier.fillMaxWidth(), compact = true)
                        Spacer(Modifier.padding(top = 10.dp))
                        ControllerBindings(state, viewModel, Modifier.fillMaxWidth())
                    }
                } else {
                    Row(
                        Modifier.fillMaxWidth().padding(start = 8.dp, end = 8.dp, bottom = 8.dp),
                        horizontalArrangement = Arrangement.spacedBy(14.dp),
                    ) {
                        ControllerOptions(state, viewModel, Modifier.width(286.dp), compact = false)
                        ControllerBindings(state, viewModel, Modifier.weight(1f))
                    }
                }
            }
        }
    }

    // No capture dialog. A Compose AlertDialog is a separate focused WINDOW that swallowed all
    // controller key events before either this Activity's dispatchKeyEvent or the main Compose
    // tree could see them — so no button ever bound (the 2.6.0 "can't remap buttons" bug). The
    // bind prompt now shows inline on the row (highlight + "press a button" text, like hotkeys),
    // and the key is captured in MainActivityRuntime.dispatchKeyEvent via capturePadAction.
}

@Composable
private fun ControllerOptions(
    state: ControllerManagerUiState,
    viewModel: ControllerManagerViewModel,
    modifier: Modifier,
    compact: Boolean,
) {
    GlassPanel(modifier) {
        Column(verticalArrangement = Arrangement.spacedBy(7.dp)) {
            if (!compact) SectionTitle(str("pad.section.playerRumble"), str("pad.editing.description"))
            Row(horizontalArrangement = Arrangement.spacedBy(7.dp)) {
                ChoiceRow(str("pad.player1"), state.player == 0, { viewModel.setPlayer(0) }, Modifier.weight(1f).controllerFocusable("controls.player1", RoundedCornerShape(14.dp), onConfirm = { viewModel.setPlayer(0) }))
                ChoiceRow(str("pad.player2"), state.player == 1, { viewModel.setPlayer(1) }, Modifier.weight(1f).controllerFocusable("controls.player2", RoundedCornerShape(14.dp), onConfirm = { viewModel.setPlayer(1) }))
            }
            // Scrolls like a long chip row should: three sections do not fit side by side everywhere.
            Row(
                horizontalArrangement = Arrangement.spacedBy(7.dp),
                modifier = Modifier.horizontalScroll(rememberScrollState()),
            ) {
                ChoiceRow(str("pad.section.buttonMapping"), state.section == ControllerSection.Buttons, { viewModel.setSection(ControllerSection.Buttons) }, Modifier.controllerFocusable("controls.section.buttons", RoundedCornerShape(14.dp), onConfirm = { viewModel.setSection(ControllerSection.Buttons) }))
                ChoiceRow(str("pad.section.padProfiles"), state.section == ControllerSection.Profiles, { viewModel.setSection(ControllerSection.Profiles) }, Modifier.controllerFocusable("controls.section.profiles", RoundedCornerShape(14.dp), onConfirm = { viewModel.setSection(ControllerSection.Profiles) }))
                ChoiceRow(str("tab.hotkeys"), state.section == ControllerSection.Hotkeys, { viewModel.setSection(ControllerSection.Hotkeys) }, Modifier.controllerFocusable("controls.section.hotkeys", RoundedCornerShape(14.dp), onConfirm = { viewModel.setSection(ControllerSection.Hotkeys) }))
            }
            if (!compact) Spacer(Modifier.weight(1f))
            SettingSwitchRow(
                str("pad.rumble.label"), str("pad.rumble.description"), state.rumble, viewModel::setRumble,
                modifier = Modifier.controllerFocusable(
                    "controls.rumble",
                    onConfirm = { viewModel.setRumble(!state.rumble) },
                ),
            )
            SettingSwitchRow(
                str("pad.multitap.label"), str("pad.multitap.description"), state.multitap, viewModel::setMultitap,
                modifier = Modifier.controllerFocusable(
                    "controls.multitap",
                    onConfirm = { viewModel.setMultitap(!state.multitap) },
                ),
            )
            SettingSwitchRow(
                str("pad.dpadAsLeftStick.label"), str("pad.dpadAsLeftStick.description"), state.dpadAsStick, viewModel::setDpadAsStick,
                modifier = Modifier.controllerFocusable(
                    "controls.dpadAsStick",
                    onConfirm = { viewModel.setDpadAsStick(!state.dpadAsStick) },
                ),
            )
            // Here as well as in the Pad tab: docking happens mid-game, and this is the screen open then.
            SettingSwitchRow(
                str("pad.allToPlayer1.label"), str("pad.allToPlayer1.description"), state.allToPlayer1, viewModel::setAllToPlayer1,
                modifier = Modifier.controllerFocusable(
                    "controls.allToPlayer1",
                    onConfirm = { viewModel.setAllToPlayer1(!state.allToPlayer1) },
                ),
            )
            // Which player each connected controller is. Several can share one. Only the players
            // Multitap arms, like the Pad tab: a pin past them would route input nowhere.
            if (!state.allToPlayer1 && state.pads.isNotEmpty()) {
                SectionTitle(str("pad.section.controllers"), str("pad.assign.help"))
                val slotCount = if (state.multitap) com.armsx2.input.PadRouter.MAX_PADS else 2
                state.pads.forEach { pad ->
                    val pinned = state.pins[pad.descriptor]
                    Text(pad.name, style = MaterialTheme.typography.labelLarge, maxLines = 1)
                    Row(
                        horizontalArrangement = Arrangement.spacedBy(7.dp),
                        modifier = Modifier.horizontalScroll(rememberScrollState()),
                    ) {
                        val auto = { viewModel.setPin(pad.descriptor, null) }
                        ChoiceRow(str("pad.assign.auto"), pinned == null, auto, Modifier.controllerFocusable("controls.pad.${pad.descriptor}.auto", RoundedCornerShape(14.dp), onConfirm = auto))
                        for (p in 0 until slotCount) {
                            val pick = { viewModel.setPin(pad.descriptor, p) }
                            ChoiceRow("P${p + 1}", pinned == p, pick, Modifier.controllerFocusable("controls.pad.${pad.descriptor}.p${p + 1}", RoundedCornerShape(14.dp), onConfirm = pick))
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ControllerBindings(state: ControllerManagerUiState, viewModel: ControllerManagerViewModel, modifier: Modifier) {
    Column(modifier) {
        SectionTitle(
            when (state.section) {
                ControllerSection.Buttons -> str("pad.section.buttonMapping")
                ControllerSection.Profiles -> str("pad.section.padProfiles")
                ControllerSection.Hotkeys -> str("hotkeys.header")
            },
            when (state.section) {
                ControllerSection.Buttons -> str("pad.instruction.tapThenPress")
                ControllerSection.Profiles -> str("pad.padProfiles.controlsInfo")
                ControllerSection.Hotkeys -> str("hotkeys.help")
            },
        )
        if (state.section == ControllerSection.Profiles) {
            ProfilePanel(state, viewModel)
        } else if (state.section == ControllerSection.Buttons) {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                ControllerMappings.actions.forEach { action ->
                    BindingRow(
                        controllerId = "controls.button.${action.id}",
                        label = action.label,
                        binding = ControllerMappings.labelForKey(ControllerMappings.physicalFor(action, state.player)),
                        capturing = state.capturingAction == action,
                        onBind = { if (state.capturingAction == action) viewModel.cancelCapture() else viewModel.beginCapture(action) },
                        onClear = { viewModel.clear(action) },
                    )
                }
            }
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                ControllerMappings.hotkeysInDisplayOrder.forEach { hotkey ->
                    BindingRow(
                        controllerId = "controls.hotkey.${hotkey.name}",
                        label = hotkey.label,
                        binding = ControllerMappings.hotkeyLabel(hotkey),
                        capturing = ControllerMappings.captureHotkey.value == hotkey,
                        onBind = { viewModel.beginHotkeyCapture(hotkey) },
                        onClear = { viewModel.clearHotkey(hotkey) },
                    )
                }
            }
        }
    }
}

/**
 * Saved mapping profiles: one tap loads a profile into the player picked on the left, mid-game.
 * Saving happens here too, so a map made for one game is a tap away in the next.
 */
@Composable
private fun ProfilePanel(state: ControllerManagerUiState, viewModel: ControllerManagerViewModel) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (state.profiles.isEmpty()) {
            Text(
                str("pad.padProfiles.none"),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        state.profiles.forEach { name ->
            val load = { viewModel.loadProfile(name) }
            ChoiceRow(
                name,
                state.loadedProfile == (state.player to name),
                load,
                Modifier.fillMaxWidth().controllerFocusable("controls.profile.$name", RoundedCornerShape(14.dp), onConfirm = load),
            )
        }
        Text(
            str("pad.padProfiles.saveNewLabel"),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        val newName = remember { mutableStateOf("") }
        Row(
            Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            // Inline, never a dialog: a Dialog is its own window and would swallow the pad keys.
            OutlinedTextField(
                value = newName.value,
                onValueChange = { newName.value = it },
                singleLine = true,
                placeholder = { Text(str("pad.padProfiles.namePlaceholder")) },
                modifier = Modifier.weight(1f),
            )
            val save = { if (viewModel.saveProfile(newName.value)) newName.value = "" }
            OutlinedButton(
                onClick = save,
                enabled = newName.value.isNotBlank(),
                modifier = Modifier.controllerFocusable("controls.profile.save", onConfirm = save),
            ) { Text(str("pad.padProfiles.saveAs")) }
        }
    }
}

@Composable
private fun ChoiceRow(label: String, selected: Boolean, onClick: () -> Unit, modifier: Modifier = Modifier) {
    Surface(
        onClick = onClick,
        modifier = modifier.defaultMinSize(minHeight = 54.dp),
        shape = RoundedCornerShape(14.dp),
        color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
    ) {
        Text(label, Modifier.padding(horizontal = 13.dp, vertical = 11.dp), style = MaterialTheme.typography.labelLarge)
    }
}

@Composable
private fun BindingRow(controllerId: String, label: String, binding: String, capturing: Boolean, onBind: () -> Unit, onClear: () -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(17.dp),
        color = if (capturing) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, if (capturing) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline.copy(alpha = 0.46f)),
    ) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(label, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
                Text(
                    binding.ifBlank { str("hotkeys.notSet") },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                )
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                TextButton(onClick = onClear, modifier = Modifier.controllerFocusable("$controllerId.clear", onConfirm = onClear)) { Text(str("pad.action.clear")) }
                OutlinedButton(onClick = onBind, modifier = Modifier.controllerFocusable("$controllerId.bind", onConfirm = onBind)) { Text(if (capturing) str("hotkeys.capturePrompt") else str("pad.action.bind")) }
            }
        }
    }
}
