package com.armsx2.enumsx2

import com.armsx2.config.Settings

/**
 * Pure ENUMSX2 target/profile logic.
 * Kept free of android.os.Build so it can be unit-tested on the JVM.
 */
object Enumsx2MobileProfile {
    private fun norm(value: String?): String =
        value.orEmpty().lowercase().replace("-", "").replace("_", "").replace(" ", "")

    fun matches(values: Iterable<String?>): Boolean {
        val joined = values.map(::norm).joinToString("|")
        return joined.contains("a67l") ||
            (joined.contains("blu") && (joined.contains("viewultra") || joined.contains("ultra5g"))) ||
            joined.contains("sc9863")
    }

    fun apply(base: Settings): Settings = base.copy(
        cpu = base.cpu.copy(
            eeCycleRate = 0,
            eeCycleSkip = 0,
            mtvu = true,
            vuAot = true,
            recEE = true,
            recIOP = true,
            recVU0 = true,
            recVU1 = true,
            enableFastmem = true,
            vuNeonFusions = true,
            vuDeferredWrites = false,
            vuSkipStallSim = false,
        ),
        output = base.output.copy(
            renderer = "vulkan",
            upscaleFloat = 1.0f,
            hwScaler = 1,
            screenResOverride = "1280x720",
        ),
        graphics = base.graphics.copy(
            accurateBlendingUnit = 1,
            texturePreloading = 1,
            maxAnisotropy = 0,
            fxaa = false,
            shaderChainEnabled = false,
            disableShaderCache = false,
            gpuProfile = 3,
        ),
        display = base.display.copy(
            hwRov = false,
            coalesceRenderPasses = true,
        ),
        hwFixes = base.hwFixes.copy(vsyncQueueSize = 2),
    )
}
