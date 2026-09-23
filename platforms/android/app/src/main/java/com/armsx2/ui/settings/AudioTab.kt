package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.i18n.str
import com.armsx2.ui.InGameOverlay

/**
 * SPU2 audio output settings. Volume + mute apply live to the open audio
 * stream (NativeApp.setAudioVolume / setAudioMuted) and persist via ConfigStore.
 */
@Composable
fun AudioTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth(),
    ) {
        Text(
            str("audio.header.description"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )
        IntSliderRow(
            label = str("audio.volume.label"),
            value = s.audio.audioVolume.coerceIn(0, 150),
            min = 0,
            max = 150,
            description = str("audio.volume.description"),
            valueFormatter = { "$it%" },
            onChange = { apply(s.copy(audio = s.audio.copy(audioVolume = it))) },
        )
        SettingsDivider()
        ToggleRow(str("audio.mute.label"), s.audio.audioMuted) { apply(s.copy(audio = s.audio.copy(audioMuted = it))) }
        SettingsDivider()
        ToggleRow(
            str("audio.synchronization.label"),
            s.audio.audioTimeStretch,
            description = str("audio.synchronization.description"),
        ) { apply(s.copy(audio = s.audio.copy(audioTimeStretch = it))) }
        SettingsDivider()
        IntSliderRow(
            label = str("audio.buffer.label"),
            value = s.audio.audioBufferMs.coerceIn(20, 200),
            min = 20,
            max = 200,
            description = str("audio.buffer.description"),
            valueFormatter = { "$it ms" },
            onChange = { apply(s.copy(audio = s.audio.copy(audioBufferMs = it))) },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("audio.outputLatency.label"),
            value = s.audio.audioOutputLatencyMs.coerceIn(5, 100),
            min = 5,
            max = 100,
            description = str("audio.outputLatency.description"),
            valueFormatter = { "$it ms" },
            onChange = { apply(s.copy(audio = s.audio.copy(audioOutputLatencyMs = it))) },
        )
        SettingsDivider()
        IntSliderRow(
            label = str("audio.fastForwardVolume.label"),
            value = s.audio.audioFastForwardVolume.coerceIn(0, 100),
            min = 0,
            max = 100,
            description = str("audio.fastForwardVolume.description"),
            valueFormatter = { "$it%" },
            onChange = { apply(s.copy(audio = s.audio.copy(audioFastForwardVolume = it))) },
        )
        SettingsDivider()
        ToggleRow(
            str("audio.swapChannels.label"),
            s.audio.audioSwapChannels,
            description = str("audio.swapChannels.description"),
        ) { apply(s.copy(audio = s.audio.copy(audioSwapChannels = it))) }
        SettingsDivider()
        ToggleRow(
            str("audio.spu2Simd.label"),
            s.audio.spu2NeonReverb,
            description = str("audio.spu2Simd.description"),
        ) { apply(s.copy(audio = s.audio.copy(spu2NeonReverb = it))) }
        SettingsDivider()
        ToggleRow(
            str("audio.openSles.label"),
            s.audio.audioOpenSLES,
            description = str("audio.openSles.description"),
        ) { apply(s.copy(audio = s.audio.copy(audioOpenSLES = it))) }
        SettingsDivider()
        ToggleRow(
            str("audio.lightweight.label"),
            s.audio.spu2LightweightMix,
            description = str("audio.lightweight.description"),
        ) { apply(s.copy(audio = s.audio.copy(spu2LightweightMix = it))) }
    }
}
