package com.armsx2.ui.settingshub

import com.armsx2.navigation.SettingsCategory

/** Frame-generation search rows. The play source set has an empty list of the same name. */
internal val LSFG_SEARCH_INDEX: List<SettingsSearchEntry> = listOf(
    SettingsSearchEntry("perf.lsfg.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.lsfg.multiplier.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.lsfg.performance.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.lsfg.adaptive.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.lsfg.flowScale.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.lsfg.dll.label", true, SettingsCategory.Performance),
)
