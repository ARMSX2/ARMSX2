package com.armsx2.enumsx2

import android.os.Build
import com.armsx2.config.Settings

/**
 * ENUMSX2 Patch 5: phone-friendly A67L/BLU optimized defaults.
 *
 * This object only transforms a Settings value passed to it. It does not persist,
 * overwrite, or repeatedly force settings by itself.
 */
object Enumsx2A67lDefaults {
    private fun norm(value: String?): String =
        value.orEmpty().lowercase().replace("-", "").replace("_", "").replace(" ", "")

    fun isTargetDevice(): Boolean {
        val values = listOf(
            Build.MODEL,
            Build.DEVICE,
            Build.PRODUCT,
            Build.HARDWARE,
            Build.MANUFACTURER,
            if (Build.VERSION.SDK_INT >= 31) Build.SOC_MODEL else ""
        ).map(::norm)

        val joined = values.joinToString("|")
        val a67l = joined.contains("a67l")
        val bluUltra = joined.contains("blu") &&
            (joined.contains("viewultra") || joined.contains("ultra5g"))
        val sc9863 = joined.contains("sc9863")

        return a67l || bluUltra || sc9863
    }

    /**
     * Conservative defaults for the target devices.
     * Keeps compatibility-oriented CPU timing while reducing mobile GPU/RAM pressure.
     */
    fun apply(base: Settings): Settings {
        if (!isTargetDevice()) return base

        return base.copy(
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
            hwFixes = base.hwFixes.copy(
                vsyncQueueSize = 2,
            ),
        )
    }
}
