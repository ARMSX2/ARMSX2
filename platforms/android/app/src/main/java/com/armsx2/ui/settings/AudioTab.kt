package com.armsx2.ui.settings

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.i18n.str
import com.armsx2.input.MicDevices
import com.armsx2.input.rememberRecordAudioAccess
import com.armsx2.ui.InGameOverlay
import kr.co.iefriends.pcsx2.NativeApp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

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
            field = "audioVolume",
        )
        SettingsDivider()
        ToggleRow(str("audio.mute.label"), s.audio.audioMuted, field = "audioMuted") { apply(s.copy(audio = s.audio.copy(audioMuted = it))) }
        SettingsDivider()
        ToggleRow(
            str("audio.synchronization.label"),
            s.audio.audioTimeStretch,
            description = str("audio.synchronization.description"),
            field = "audioTimeStretch",
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
            field = "audioBufferMs",
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
            field = "audioOutputLatencyMs",
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
            field = "audioFastForwardVolume",
        )
        SettingsDivider()
        ToggleRow(
            str("audio.swapChannels.label"),
            s.audio.audioSwapChannels,
            description = str("audio.swapChannels.description"),
            field = "audioSwapChannels",
        ) { apply(s.copy(audio = s.audio.copy(audioSwapChannels = it))) }
        SettingsDivider()
        ToggleRow(
            str("audio.spu2Simd.label"),
            s.audio.spu2NeonReverb,
            description = str("audio.spu2Simd.description"),
            field = "spu2NeonReverb",
        ) { apply(s.copy(audio = s.audio.copy(spu2NeonReverb = it))) }
        SettingsDivider()
        ToggleRow(
            str("audio.openSles.label"),
            s.audio.audioOpenSLES,
            description = str("audio.openSles.description"),
            field = "audioOpenSLES",
        ) { apply(s.copy(audio = s.audio.copy(audioOpenSLES = it))) }
        SettingsDivider()
        ToggleRow(
            str("audio.recordingCompatible.label"),
            s.audio.audioRecordingCompatible,
            description = str("audio.recordingCompatible.description"),
            field = "audioRecordingCompatible",
        ) { apply(s.copy(audio = s.audio.copy(audioRecordingCompatible = it))) }
        SettingsDivider()
        ToggleRow(
            str("audio.lightweight.label"),
            s.audio.spu2LightweightMix,
            description = str("audio.lightweight.description"),
            field = "spu2LightweightMix",
        ) { apply(s.copy(audio = s.audio.copy(spu2LightweightMix = it))) }

        MicrophoneSection(s, ::apply)
    }
}

/**
 * The microphone a game is given (the emulated USB mic, captured through Oboe): the kind, then which
 * device. Mirrors ARMSX3's Audio settings section so both ports share one experience; the PS2 core
 * offers Standard (one microphone) and SingStar (two singers, one microphone each).
 *
 * Android lists the devices to Java only (MicDevices). Each is stored by its kind and product name,
 * so the choice survives unplugging it; one that is not plugged in stays listed as not connected.
 */
@Composable
private fun MicrophoneSection(s: Settings, apply: (Settings) -> Unit) {
    val context = LocalContext.current
    var allowed by remember { mutableStateOf(MicDevices.granted(context)) }
    var devices by remember { mutableStateOf(MicDevices.list(context)) }

    // Back from the permission prompt or Android settings, or after plugging a microphone in
    androidx.lifecycle.compose.LifecycleEventEffect(androidx.lifecycle.Lifecycle.Event.ON_RESUME) {
        allowed = MicDevices.granted(context)
        devices = MicDevices.list(context)
    }

    val requestPermission = rememberRecordAudioAccess()

    SettingsDivider()
    SegmentedGridRow(
        label = str("audio.mic.label"),
        options = listOf(str("common.off"), str("audio.mic.standard"), "SingStar"),
        selectedIndex = s.audio.micType.coerceIn(0, 2),
        columns = 3,
        description = str("audio.mic.description"),
        onChange = {
            apply(s.copy(audio = s.audio.copy(micType = it)))
            if (it != 0 && !allowed) requestPermission { allowed = it }
        },
    )

    if (s.audio.micType == 0) return

    if (!allowed) {
        SettingsDivider()
        MicActionRow("audio.mic.allow", str("audio.mic.allow"), str("audio.mic.allow.description")) {
            requestPermission { allowed = it }
        }
        // Android stops showing the prompt once it has been dismissed twice
        MicActionRow("audio.mic.settings", str("audio.mic.settings"), str("audio.mic.settings.description")) {
            runCatching {
                context.startActivity(
                    Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", context.packageName, null))
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
            }
        }
    }

    val noneLabel = str("audio.mic.none")
    val automaticLabel = str("audio.mic.automatic")
    val builtinLabel = str("audio.mic.builtin")
    val notConnectedLabel = str("audio.mic.notConnected")

    // Automatic first, then what is plugged in, then a saved one that is not
    fun options(saved: String, withNone: Boolean): Pair<List<String>, List<String>> {
        val names = mutableListOf<String>()
        val labels = mutableListOf<String>()

        if (withNone) {
            names += ""
            labels += noneLabel
        }

        names += MicDevices.DEFAULT
        labels += automaticLabel

        for (device in devices) {
            names += device.name
            labels += if (device.name == MicDevices.BUILTIN) builtinLabel else device.product
        }

        if (saved !in names) {
            names += saved
            labels += notConnectedLabel.format(saved.substringAfter(':'))
        }

        return names to labels
    }

    val (names1, labels1) = options(s.audio.micDevice1, withNone = false)
    SettingsDivider()
    SegmentedGridRow(
        label = str(if (s.audio.micType == 2) "audio.mic.singer1" else "audio.mic.device"),
        options = labels1,
        selectedIndex = names1.indexOf(s.audio.micDevice1).coerceAtLeast(0),
        columns = 1,
        description = str("audio.mic.device.description"),
        onChange = { apply(s.copy(audio = s.audio.copy(micDevice1 = names1[it]))) },
    )

    SettingsDivider()
    IntSliderRow(
        label = str("audio.mic.level"),
        value = s.audio.micLevel.coerceIn(0, 24),
        min = 0,
        max = 24,
        description = str("audio.mic.level.description"),
        valueFormatter = { "$it dB" },
        onChange = { apply(s.copy(audio = s.audio.copy(micLevel = it))) },
        field = "micLevel",
    )

    if (allowed) {
        MicTestRow(s.audio.micDevice1)
    }

    // SingStar: one microphone per singer
    if (s.audio.micType == 2) {
        val (names2, labels2) = options(s.audio.micDevice2, withNone = true)
        SettingsDivider()
        SegmentedGridRow(
            label = str("audio.mic.singer2"),
            options = labels2,
            selectedIndex = names2.indexOf(s.audio.micDevice2).coerceAtLeast(0),
            columns = 1,
            onChange = { apply(s.copy(audio = s.audio.copy(micDevice2 = names2[it]))) },
        )
    }
}

/**
 * Hears the chosen microphone through the same capture a game gets (audiodev-oboe in the core), with
 * a bar for how loud it is: if the bar moves, a game is sent the same sound.
 */
@Composable
private fun MicTestRow(device: String) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var testing by remember { mutableStateOf(false) }
    var failed by remember { mutableStateOf(false) }
    var level by remember { mutableFloatStateOf(0f) }

    // Never left recording: leaving the screen or the app ends the test
    DisposableEffect(Unit) { onDispose { NativeApp.micTestStop() } }
    androidx.lifecycle.compose.LifecycleEventEffect(androidx.lifecycle.Lifecycle.Event.ON_STOP) {
        NativeApp.micTestStop()
        testing = false
    }

    LaunchedEffect(testing) {
        while (testing) {
            val now = withContext(Dispatchers.IO) { NativeApp.micTestLevel() }
            if (now < 0f) break
            level = now
            delay(60)
        }
        level = 0f
    }

    val toggle: () -> Unit = {
        if (testing) {
            NativeApp.micTestStop()
            testing = false
        } else {
            scope.launch {
                val ok = withContext(Dispatchers.IO) {
                    MicDevices.push(context)
                    NativeApp.micTestStart(device)
                }
                failed = !ok
                testing = ok
            }
        }
    }

    SettingsDivider()
    MicActionRow(
        "audio.mic.test",
        str(if (testing) "audio.mic.test.stop" else "audio.mic.test"),
        str(if (failed) "audio.mic.test.failed" else "audio.mic.test.description"),
        toggle,
    )

    if (testing) {
        LinearProgressIndicator(
            progress = { level },
            modifier = Modifier.fillMaxWidth().padding(horizontal = 6.dp, vertical = 8.dp).height(10.dp),
        )
    }
}

/** A tappable label and subtitle, reachable with the pad like the shared rows. */
@Composable
private fun MicActionRow(controllerId: String, label: String, description: String, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .height(64.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .clickable(onClick = onClick)
            .controllerFocusable(controllerId, onConfirm = onClick)
            .padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column {
            Text(label, color = MaterialTheme.colorScheme.onSurface, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
            Text(description, color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 12.sp)
        }
    }
}
