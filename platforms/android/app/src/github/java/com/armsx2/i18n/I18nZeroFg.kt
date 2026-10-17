package com.armsx2.i18n

/**
 * ZeroFG strings, github flavour only.
 *
 * The play source set has an empty map of the same name, so these are absent from the Play build
 * rather than present-but-unreachable. [EN] merges whichever one is in scope.
 */
internal val ZEROFG_EN: Map<String, String> = mapOf(
    "perf.zerofg.label" to "Frame Generation (ZeroFG)",
    "perf.zerofg.description" to "ZeroFG frame generation, on its own Vulkan device beside the renderer: one generated frame between every two of the game's, presented on the screen's own refresh. Needs a Vulkan 1.3 driver and a screen at twice the game's frame rate (120 Hz for a 60 fps game). Switches while the game runs.",
    "perf.zerofg.mode.label" to "ZeroFG engine",
    "perf.zerofg.mode.description" to "Zero: the full engine. ReallyZero: its economy tier, two fine refinement passes instead of four, lighter on the GPU. Switches while the game runs.",
)
