package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.nativeKeyCode
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.input.ControllerMappings
import com.armsx2.ui.Colors
import com.armsx2.ui.touch.TouchControls

@Composable
fun PadTab(@Suppress("UNUSED_PARAMETER") state: MutableState<Settings>) {
    val scroll = remember { ScrollState(0) }
    ControllerAutoScroll(scroll)
    val capture = remember { mutableStateOf<ControllerMappings.Action?>(null) }
    val refreshToken = remember { mutableStateOf(0) }
    val focusRequester = remember { FocusRequester() }

    LaunchedEffect(capture.value) {
        if (capture.value != null)
            focusRequester.requestFocus()
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .focusRequester(focusRequester)
            .focusable()
            .onPreviewKeyEvent { event ->
                // Regular button capture only — the menu button is captured in
                // Main.dispatchKeyEvent so it can grab BACK / back-paddle keys.
                val action = capture.value ?: return@onPreviewKeyEvent false
                if (event.type != KeyEventType.KeyDown)
                    return@onPreviewKeyEvent true
                val nativeKeyCode = event.key.nativeKeyCode
                if (nativeKeyCode == android.view.KeyEvent.KEYCODE_UNKNOWN)
                    return@onPreviewKeyEvent true
                ControllerMappings.bind(action, nativeKeyCode)
                capture.value = null
                refreshToken.value++
                true
            }
            .verticalScroll(scroll),
    ) {
        Text(
            "Tap an action, then press a physical controller button.",
            color = Color(0xFFBBBBBB),
            fontSize = 12.sp,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 6.dp),
        )
        SettingsDivider()
        @Suppress("UNUSED_EXPRESSION")
        refreshToken.value
        ControllerMappings.actions.forEach { action ->
            val physical = ControllerMappings.physicalFor(action)
            PadBindingRow(
                action = action,
                physical = physical,
                capturing = capture.value == action,
                onClick = { capture.value = action },
            )
            SettingsDivider()
        }
        Box(
            Modifier
                .fillMaxWidth()
                .height(30.dp)
                .background(rowAura())
                .clickable {
                    ControllerMappings.reset()
                    capture.value = null
                    refreshToken.value++
                }
                .controllerFocusable(
                    controllerId = "pad-reset",
                    onConfirm = {
                        ControllerMappings.reset()
                        capture.value = null
                        refreshToken.value++
                    },
                )
                .padding(horizontal = 6.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Text("Reset Controller Mappings", color = Colors.pasx2_blue, fontSize = 13.sp, fontWeight = FontWeight.Bold)
        }

        // Controller hotkeys now live in their own dedicated "Hotkeys" tab
        // (see HotkeysTab) so they're easier to find than buried under Pad.
        SettingsDivider()
        IntSliderRow(
            label = "On-screen controls",
            value = TouchControls.visibilityMode.value,
            min = 0,
            max = 11,
            description = "On-screen touch buttons. Never = always hidden (for physical-controls devices — also hides the settings cog so nothing overlaps R1). 1–10s = auto-hide after that long without a touch. Auto = show on touch, hide when you use a controller.",
            valueFormatter = { when (it) { 0 -> "Never"; 11 -> "Auto"; else -> "${it}s" } },
            onChange = { TouchControls.setVisibilityMode(it) },
        )
    }
}

@Composable
private fun PadBindingRow(
    action: ControllerMappings.Action,
    physical: Int,
    capturing: Boolean,
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .height(30.dp)
            .background(rowAura())
            .clickable(onClick = onClick)
            .controllerFocusable(
                controllerId = "pad:${action.id}",
                onConfirm = onClick,
            )
            .padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(action.label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.weight(1f))
        Text(
            if (capturing) "Press a button..." else ControllerMappings.labelForKey(physical),
            color = if (capturing) Color(0xFFFFD33A) else Color(0xFFCCCCCC),
            fontSize = 12.sp,
            fontWeight = FontWeight.Bold,
        )
    }
}
