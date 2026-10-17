package com.armsx2.ui.common

import androidx.compose.runtime.Composable
import com.armsx2.BuildConfig
import com.armsx2.i18n.str
import com.armsx2.ui.settings.SegmentedRow
import com.armsx2.ui.settings.SettingsDivider
import com.armsx2.ui.settings.ToggleRow

/**
 * ZeroFG frame-generation rows: the master toggle and, while it is on, the engine.
 *
 * Presentation only, like [LsfgSection]: the caller wires [onChange] to its own settings tier.
 * ZeroFG switches in game, so the two can be compared on the same scene, and so can its engines.
 *
 * Github flavour only, beside LSFG so the two can be compared in the same build; the play source
 * set has a stub of the same name.
 */
@Composable
fun ZeroFgSection(
    enabled: Boolean,
    mode: Int,
    onChange: (enabled: Boolean, mode: Int) -> Unit,
) {
    if (!BuildConfig.ZEROFG) return

    ToggleRow(
        label = str("perf.zerofg.label"),
        value = enabled,
        description = str("perf.zerofg.description"),
    ) { on -> onChange(on, mode) }

    if (enabled) {
        SettingsDivider()
        // Product names, not translated.
        SegmentedRow(
            label = str("perf.zerofg.mode.label"),
            options = listOf("Zero", "ReallyZero"),
            selectedIndex = mode.coerceIn(0, 1),
            description = str("perf.zerofg.mode.description"),
        ) { index -> onChange(enabled, index) }
    }
}

/**
 * The same rows as a card of the in-game menu, beside [LsfgEmulationCard]: ZeroFG switches while
 * the game runs, so it can be turned off and on (or traded for LSFG) on the same scene.
 */
@Composable
fun ZeroFgEmulationCard(
    enabled: Boolean,
    mode: Int,
    onChange: (enabled: Boolean, mode: Int) -> Unit,
) {
    if (!BuildConfig.ZEROFG) return
    com.armsx2.ui.emulation.SectionCard(str("perf.zerofg.label")) {
        ZeroFgSection(enabled, mode, onChange)
    }
}
