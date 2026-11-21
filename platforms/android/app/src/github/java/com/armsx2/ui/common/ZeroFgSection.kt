package com.armsx2.ui.common

import androidx.compose.runtime.Composable
import com.armsx2.BuildConfig
import com.armsx2.i18n.str
import com.armsx2.ui.settings.SegmentedRow
import com.armsx2.ui.settings.SettingsDivider
import com.armsx2.ui.settings.ToggleRow

/**
 * ZeroFG frame-generation rows: the master toggle and, while it is on, the mode and the multiplier.
 *
 * Presentation only, like [LsfgSection]: the caller wires [onChange] to its own settings tier.
 * ZeroFG switches in game, so it can be compared with LSFG on the same scene, and so can its modes.
 *
 * Github flavour only, beside LSFG so the two can be compared in the same build; the play source
 * set has a stub of the same name.
 */
/** The saved ZeroFG mode of each option the settings offer, in their order: ZeroFG 1, ZeroGS 7. */
private val ZEROFG_MODES = intArrayOf(1, 7)

/** The multipliers the settings offer, in their order. */
private val ZEROFG_MULTIPLIERS = intArrayOf(2, 3, 4)

@Composable
fun ZeroFgSection(
    enabled: Boolean,
    mode: Int,
    multiplier: Int,
    onChange: (enabled: Boolean, mode: Int, multiplier: Int) -> Unit,
) {
    if (!BuildConfig.ZEROFG) return

    ToggleRow(
        label = str("perf.zerofg.label"),
        value = enabled,
        description = str("perf.zerofg.description"),
    ) { on -> onChange(on, mode, multiplier) }

    if (enabled) {
        SettingsDivider()
        // Product names, not translated. The saved mode is EmuCore/GS/ZeroFGMode: ZeroFG 1, ZeroGS 7; the 1.1 modes still saved
        // show (and run) as their 1.2 one: 0 as ZeroFG, 6 as ZeroGS.
        SegmentedRow(
            label = str("perf.zerofg.mode.label"),
            options = listOf("ZeroFG", "ZeroGS"),
            selectedIndex = if (mode >= 6) 1 else 0,
            description = str("perf.zerofg.mode.description"),
        ) { index -> onChange(enabled, ZEROFG_MODES[index], multiplier) }
        SettingsDivider()
        SegmentedRow(
            label = str("perf.zerofg.multiplier.label"),
            options = listOf("x2", "x3", "x4"),
            selectedIndex = (multiplier.coerceIn(2, 4) - 2),
            description = str("perf.zerofg.multiplier.description"),
        ) { index -> onChange(enabled, mode, ZEROFG_MULTIPLIERS[index]) }
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
    multiplier: Int,
    onChange: (enabled: Boolean, mode: Int, multiplier: Int) -> Unit,
) {
    if (!BuildConfig.ZEROFG) return
    com.armsx2.ui.emulation.SectionCard(str("perf.zerofg.label")) {
        ZeroFgSection(enabled, mode, multiplier, onChange)
    }
}
