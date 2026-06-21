package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.input.ControllerMappings
import com.armsx2.ui.Colors

/**
 * Dedicated controller-hotkey binding tab. Pulled out of the Pad tab so the
 * hotkeys (menu, quick save/load, slot cycle, texture-dump toggle, fast
 * forward, resolution ±, achievements, close game) have a home that's easy to
 * find. Binding happens via [ControllerMappings.captureHotkey] — tapping a row
 * arms it, and the next button seen by Main.dispatchKeyEvent is bound to it.
 */
@Composable
fun HotkeysTab(@Suppress("UNUSED_PARAMETER") state: MutableState<Settings>) {
    val scroll = remember { ScrollState(0) }
    ControllerAutoScroll(scroll)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        Text(
            "Controller hotkeys",
            color = Color.White,
            fontSize = 13.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 8.dp),
        )
        HelpText(
            "Bind physical buttons (back paddles work too) to in-game actions — " +
                "no on-screen cog needed. Quick Save/Load use the active slot " +
                "(change it with Cycle Save Slot).",
        )
        ControllerMappings.SysHotkey.values().forEach { hk ->
            @Suppress("UNUSED_EXPRESSION") ControllerMappings.hotkeyBindTick.value
            val capturing = ControllerMappings.captureHotkey.value == hk
            val code = ControllerMappings.hotkeyCode(hk)
            val unset = code == android.view.KeyEvent.KEYCODE_UNKNOWN
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(30.dp)
                    .background(rowAura())
                    .clickable { ControllerMappings.captureHotkey.value = hk }
                    .controllerFocusable(
                        controllerId = "hotkey:${hk.name}",
                        onConfirm = { ControllerMappings.captureHotkey.value = hk },
                    )
                    .padding(horizontal = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(hk.label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.weight(1f))
                if (!unset && !capturing) {
                    Text(
                        "Clear",
                        color = Color(0xFFFF6B6B),
                        fontSize = 11.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier
                            .clickable {
                                ControllerMappings.clearHotkey(hk)
                                ControllerMappings.hotkeyBindTick.value++
                            }
                            .padding(end = 10.dp),
                    )
                }
                Text(
                    when {
                        capturing -> "Press any button (incl. back)…"
                        unset -> "Not set"
                        else -> ControllerMappings.labelForKey(code)
                    },
                    color = if (capturing) Color(0xFFFFD33A) else Color(0xFFCCCCCC),
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                )
            }
            SettingsDivider()
        }
        @Suppress("UNUSED_EXPRESSION") Box(Modifier.height(6.dp))
    }
}
