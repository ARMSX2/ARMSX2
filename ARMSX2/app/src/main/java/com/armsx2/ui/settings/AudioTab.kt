package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.ui.InGameOverlay

/**
 * SPU2 audio output settings. Volume + mute apply live to the open audio
 * stream (NativeApp.setAudioVolume / setAudioMuted) and persist via ConfigStore.
 */
@Composable
fun AudioTab(state: MutableState<Settings>) {
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
            "PS2 audio (SPU2) output. Volume/mute apply instantly; the rest reapply the moment you change them.",
            color = Color(0xFFB0B0B0),
            fontSize = 11.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )
        IntSliderRow(
            label = "Volume",
            value = s.audioVolume.coerceIn(0, 150),
            min = 0,
            max = 150,
            description = "Above 100% boosts/amplifies SPU2 output — handy for quiet games, but very high levels can distort/clip.",
            valueFormatter = { "$it%" },
            onChange = { apply(s.copy(audioVolume = it)) },
        )
        SettingsDivider()
        ToggleRow("Mute", s.audioMuted) { apply(s.copy(audioMuted = it)) }
        SettingsDivider()
        ToggleRow(
            "Audio Synchronization",
            s.audioTimeStretch,
            description = "Time Stretch — keeps audio pitch stable when emulation speed varies (PCSX2 2.7 \"Synchronization\"). Off = a bit less CPU, but pitch/clock can drift under load.",
        ) { apply(s.copy(audioTimeStretch = it)) }
        SettingsDivider()
        IntSliderRow(
            label = "Audio Buffer",
            value = s.audioBufferMs.coerceIn(20, 200),
            min = 20,
            max = 200,
            description = "Bigger buffer = fewer crackles/dropouts but more latency. Raise this if audio stutters on low-end devices.",
            valueFormatter = { "$it ms" },
            onChange = { apply(s.copy(audioBufferMs = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = "Output Latency",
            value = s.audioOutputLatencyMs.coerceIn(5, 100),
            min = 5,
            max = 100,
            description = "Target output latency. Lower is more responsive; higher is safer against dropouts.",
            valueFormatter = { "$it ms" },
            onChange = { apply(s.copy(audioOutputLatencyMs = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = "Fast-Forward Volume",
            value = s.audioFastForwardVolume.coerceIn(0, 100),
            min = 0,
            max = 100,
            description = "Output volume while fast-forwarding.",
            valueFormatter = { "$it%" },
            onChange = { apply(s.copy(audioFastForwardVolume = it)) },
        )
        SettingsDivider()
        ToggleRow(
            "Swap Left/Right Channels",
            s.audioSwapChannels,
            description = "Swaps the stereo output (L↔R). Useful when a device's Type-C port forces reverse-landscape and flips the physical speakers (e.g. the Clamp gamepad), which otherwise reverses the stereo image in racing games. Applies instantly.",
        ) { apply(s.copy(audioSwapChannels = it)) }
        SettingsDivider()
        ToggleRow(
            "SPU2 SIMD audio (experimental)",
            s.spu2NeonReverb,
            description = "NEON fast path for reverb audio processing — frees up CPU, which can help performance on CPU-limited devices. Off (default) uses the standard path with unchanged audio. Reboot the game to switch.",
        ) { apply(s.copy(spu2NeonReverb = it)) }
    }
}
