package com.armsx2

import android.content.Context
import android.os.Build
import com.armsx2.config.Settings

/** ENUMSX2 device-specific first-install defaults.
 *
 * This intentionally runs only while ConfigStore is seeding a genuinely fresh install.
 * It never overwrites settings after the user has a persisted global configuration.
 */
object EnumSX2DeviceProfile {
    enum class Target { GENERIC, A67L_GEN2, BLU_VIEW_ULTRA_5G }

    private fun norm(value: String?): String = value.orEmpty().trim().lowercase()

    fun target(): Target {
        val model = norm(Build.MODEL)
        val device = norm(Build.DEVICE)
        val product = norm(Build.PRODUCT)
        val hardware = norm(Build.HARDWARE)
        val board = norm(Build.BOARD)
        val all = listOf(model, device, product, hardware, board).joinToString(" ")

        return when {
            "a67l" in all || "a67l gen2" in all -> Target.A67L_GEN2
            ("blu" in all && "view" in all && "ultra" in all) || "view ultra 5g" in all -> Target.BLU_VIEW_ULTRA_5G
            else -> Target.GENERIC
        }
    }

    fun isPrimaryTarget(): Boolean = target() != Target.GENERIC

    /** Apply conservative defaults using fields already consumed by ARMSX2's settings layer. */
    fun applyFreshInstallDefaults(context: Context, base: Settings): Settings {
        if (!isPrimaryTarget()) return base

        // Keep native resolution and Vulkan when ConfigStore already determined Vulkan 1.1 is usable.
        // PowerVR is explicitly supported by the existing AndroidGpuProfileOverride mapping.
        return base.copy(
            cpu = base.cpu.copy(
                eeCycleRate = 0,
                eeCycleSkip = 0,
                dynamicEeCycleRate = false,
                mtvu = DeviceTier.mtvuDefault(),
                vuAot = true,
                recEE = true,
                recIOP = true,
                recVU0 = true,
                recVU1 = true,
                enableFastmem = true,
                extraMemory = false,
            ),
            output = base.output.copy(
                upscaleFloat = 1.0f,
            ),
            graphics = base.graphics.copy(
                accurateBlendingUnit = 0,
                textureFiltering = 2,
                texturePreloading = 1,
                hardwareDownloadMode = 0,
                fxaa = false,
                shaderChainEnabled = false,
                maxAnisotropy = 0,
                gpuProfile = 3,
                disableShaderCache = false,
                vsyncEnable = false,
            ),
        )
    }
}
