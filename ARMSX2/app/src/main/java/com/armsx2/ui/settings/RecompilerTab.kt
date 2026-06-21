package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.material3.Text
import com.armsx2.config.Settings
import com.armsx2.ui.InGameOverlay

/**
 * Recompiler section of the in-game settings overlay.
 *
 * Disables here drop the corresponding CPU/COP onto its interpreter
 * fallback. Android refresh uses the macOS/PCSX2 ARM64 backend as the single
 * JIT path; there is no per-CPU original-vs-mac A/B surface here anymore.
 * Toggling these on a running VM swaps rec vs interpreter via
 * VMManager::ApplySettings's CpusChanged path, so treat this tab as a debug
 * tool.
 */
@Composable
fun RecompilerTab(state: MutableState<Settings>) {
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
            "Disabling a recompiler drops to interpreter — debug only, expect a heavy slowdown.",
            color = Color(0xFFB0B0B0),
            fontSize = 11.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )
        // 4-wide toggle grid, matching the Playing-Now + Performance look.
        // Row 1: the four CPU/COP recompilers (mac ARM64 backend vs interpreter).
        // Row 2: Fastmem + spacers.
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            BubbleGridRow {
                ToggleBubble("EE (R5900)", s.recEE, modifier = Modifier.weight(1f)) {
                    apply(s.copy(recEE = it))
                }
                ToggleBubble("IOP (R3000)", s.recIOP, modifier = Modifier.weight(1f)) {
                    apply(s.copy(recIOP = it))
                }
                ToggleBubble("VU0", s.recVU0, modifier = Modifier.weight(1f)) {
                    apply(s.copy(recVU0 = it))
                }
                ToggleBubble("VU1", s.recVU1, modifier = Modifier.weight(1f)) {
                    apply(s.copy(recVU1 = it))
                }
            }
            BubbleGridRow {
                ToggleBubble("Fastmem", s.enableFastmem, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableFastmem = it))
                }
                Spacer(Modifier.weight(1f))
                Spacer(Modifier.weight(1f))
                Spacer(Modifier.weight(1f))
            }
        }
    }
}
