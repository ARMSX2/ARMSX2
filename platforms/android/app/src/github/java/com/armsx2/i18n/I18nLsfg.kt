package com.armsx2.i18n

/**
 * Frame-generation strings, github flavour only.
 *
 * The play source set has an empty map of the same name, so these are absent from the Play build
 * rather than present-but-unreachable. [EN] merges whichever one is in scope.
 */
internal val LSFG_EN: Map<String, String> = mapOf(
    "perf.lsfg.label" to "Frame Generation (LSFG)",
    "perf.lsfg.description" to "Insert interpolated frames between rendered ones. Needs Vulkan, an Adreno 7xx or newer GPU (or a 6xx running a Turnip driver), and your own copy of Lossless Scaling.",
    "perf.lsfg.multiplier.label" to "Generated Frames",
    "perf.lsfg.multiplier.description" to "Frames displayed for each frame the emulator renders. Higher is smoother but adds latency and GPU load.",
    "perf.lsfg.performance.label" to "Performance Mode",
    "perf.lsfg.performance.description" to "Use the lighter 3.1p interpolation shaders. Cheaper on the GPU, slightly softer around fast motion. Ignored if your Lossless.dll is too old to include them.",
    "perf.lsfg.fp16.label" to "Half Precision",
    "perf.lsfg.fp16.description" to "Use the half-precision (fp16) shaders, which phone GPUs often run much faster. Some drivers handle fp16 badly: if generated frames look wrong, turn this off. Turning it on takes effect the next time a game starts, and the performance overlay shows fp16 while it is in use.",
    "perf.lsfg.adaptive.label" to "Adaptive frame pacing",
    "perf.lsfg.adaptive.description" to "Vary how many frames are generated to hold a steady on-screen rate, instead of always multiplying by the same amount. Helps most in games that swing between 60 and 30fps, where a fixed multiplier makes every transition visible. Targets your display\u2019s refresh rate.",
    "perf.lsfg.flowScale.label" to "Motion Detail",
    "perf.lsfg.flowScale.description" to "Resolution of the motion analysis. At Auto it follows the resolution the game renders at, so none of the game's own detail is lost. Lower is much cheaper and blurs fine detail in the generated frames.",
    "perf.lsfg.flowScale.auto" to "Auto",
    "perf.lsfg.dll.label" to "Lossless.dll",
    "perf.lsfg.dll.none" to "Not selected. Tap to choose your Lossless.dll",
    "perf.lsfg.dll.selected" to "Selected. Tap to choose a different file",
    "perf.lsfg.dll.importFailed" to "Could not read the selected file.",
    "perf.lsfg.dll.notADll" to "That file is not a readable Lossless.dll.",
    "perf.lsfg.requirements.title" to "Frame Generation requirements",
    "perf.lsfg.requirements.body" to "Frame generation uses Lossless Scaling\u2019s own interpolation shaders. ARMSX2 does not include them: you must own Lossless Scaling and supply its Lossless.dll yourself.\n\nIt also requires the Vulkan renderer and an Adreno 7xx or newer GPU, or an Adreno 6xx running a Turnip driver. On anything else it stays off.",
    "perf.lsfg.requirements.accept" to "I understand",
    "perf.lsfg.unavailable.notBuilt" to "Frame generation is not included in this build.",
    "perf.lsfg.unavailable.notVulkan" to "Frame generation requires the Vulkan renderer.",
    "perf.lsfg.unavailable.gpu" to "Frame generation requires an Adreno 7xx or newer GPU, or an Adreno 6xx running a Turnip driver.",
    "perf.lsfg.unavailable.noDll" to "Select your Lossless.dll to enable frame generation.",
    "perf.lsfg.unavailable.badDll" to "The selected file is not a readable Lossless.dll.",
    "perf.lsfg.unavailable.initFailed" to "Frame generation could not start on this device. Check the log for details.",
)
