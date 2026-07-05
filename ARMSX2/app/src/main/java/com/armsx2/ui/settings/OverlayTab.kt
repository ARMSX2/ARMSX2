package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.ui.InGameOverlay
import com.armsx2.ui.UiScale

/**
 * Performance Overlay element toggles. Lets the user show/hide individual
 * parts of the on-screen stats overlay (the master OSD pill on the Play tab
 * is still the quick all-on/all-off switch).
 *
 * The GPU toggle is special: turning it off also stops the GPU timing
 * queries (timestamp queries + per-frame readback have real overhead), so
 * it's a genuine performance lever, not just a display option — see GS.cpp
 * SetGPUTimingEnabled. Each toggle persists to base AND applies live via the
 * native osdShow* setters (see [InGameOverlay.applySafeLiveDelta]).
 */
@Composable
fun OverlayTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = remember { ScrollState(0) }
    ControllerAutoScroll(scroll)

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        Text(
            "Show or hide parts of the performance overlay. Turning GPU off also " +
                "stops the GPU timing queries, which gives back a little performance.",
            color = Color(0xFFB0B0B0),
            fontSize = 11.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )

        ToggleRow("GPU usage (saves perf when off)", s.osdShowGpu) {
            apply(s.copy(osdShowGpu = it))
        }
        SettingsDivider()
        ToggleRow("CPU usage", s.osdShowCpu) { apply(s.copy(osdShowCpu = it)) }
        SettingsDivider()
        ToggleRow("FPS", s.osdShowFps) { apply(s.copy(osdShowFps = it)) }
        SettingsDivider()
        ToggleRow("VPS (vblanks/sec)", s.osdShowVps) { apply(s.copy(osdShowVps = it)) }
        SettingsDivider()
        ToggleRow("Emulation speed %", s.osdShowSpeed) { apply(s.copy(osdShowSpeed = it)) }
        SettingsDivider()
        ToggleRow("Internal resolution", s.osdShowResolution) { apply(s.copy(osdShowResolution = it)) }
        SettingsDivider()
        ToggleRow("GS statistics", s.osdShowGsStats) { apply(s.copy(osdShowGsStats = it)) }
        SettingsDivider()
        ToggleRow("Frame times graph", s.osdShowFrameTimes) { apply(s.copy(osdShowFrameTimes = it)) }
        SettingsDivider()
        ToggleRow("Hardware info (CPU/GPU model)", s.osdShowHardwareInfo) { apply(s.copy(osdShowHardwareInfo = it)) }
        SettingsDivider()
        ToggleRow("GPU pipeline stats (VSI/PSI, Vulkan only)", s.osdShowGpuStats) { apply(s.copy(osdShowGpuStats = it)) }
        SettingsDivider()
        ToggleRow("Emulator version", s.osdShowVersion) { apply(s.copy(osdShowVersion = it)) }
        SettingsDivider()
        ToggleRow("Settings summary (bottom-right)", s.osdShowSettings) { apply(s.copy(osdShowSettings = it)) }
        SettingsDivider()
        ToggleRow("Control inputs (bottom-left)", s.osdShowInputs) { apply(s.copy(osdShowInputs = it)) }
        SettingsDivider()
        ToggleRow("On-screen notifications (shader compile, saves, etc.)", s.osdShowMessages) { apply(s.copy(osdShowMessages = it)) }
        SettingsDivider()
        // Android hotkey pop-ups (Fast-Forward on/off, etc.) — separate from the emulator
        // OSD, pref-backed. Cancel-previous already stops them stacking; this switches
        // them off entirely for heavy fast-forward users.
        val ffToasts = remember { mutableStateOf(com.armsx2.Main.prefs.getBoolean("ui.hotkeyToasts", true)) }
        ToggleRow("Fast-Forward pop-ups", ffToasts.value) {
            ffToasts.value = it
            com.armsx2.Main.prefs.edit().putBoolean("ui.hotkeyToasts", it).apply()
        }
        SettingsDivider()

        // Interface scaling (global, not per-game): resize the library / menu chrome
        // and text for different screen aspect ratios / handheld sizes. Does NOT
        // touch the game image or the on-screen touch controls.
        Text(
            "Interface scaling — resize the library and menu UI to fit your screen. " +
                "Does not affect the game image or on-screen touch controls.",
            color = Color(0xFFB0B0B0),
            fontSize = 11.sp,
            modifier = Modifier.padding(top = 10.dp, bottom = 6.dp),
        )
        IntSliderRow(
            label = "UI Size (borders)",
            value = (UiScale.borderScale.value * 100f).toInt(),
            min = (UiScale.MIN * 100f).toInt(),
            max = (UiScale.BORDER_MAX * 100f).toInt(),
            description = "Scales menu/library padding and control sizes. 100% = default.",
            valueFormatter = { "$it%" },
            onChange = { UiScale.setBorderScale(it / 100f) },
        )
        SettingsDivider()
        IntSliderRow(
            label = "UI Font Size",
            value = (UiScale.fontScale.value * 100f).toInt(),
            min = (UiScale.MIN * 100f).toInt(),
            max = (UiScale.MAX * 100f).toInt(),
            description = "Scales menu/library text. 100% = default.",
            valueFormatter = { "$it%" },
            onChange = { UiScale.setFontScale(it / 100f) },
        )
    }
}
