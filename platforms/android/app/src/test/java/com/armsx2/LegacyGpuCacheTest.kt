package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Test

class LegacyGpuCacheTest {
    @Test
    fun onlyThePreStampFilesGo() {
        val found = listOf(
            "gs_packs", "vulkan_pipelines_1a2b3c4d.bin", "vulkan_pipelines_debug_0badf00d.bin", "lsfg_spirv.cache",
            "gamelist.cache", "achievement_images", "covers", "driver-packs", "texture-catalog", ".install",
            "vulkan_shaders.idx", "vulkan_shaders.bin", "vulkan_shaders_debug.idx", "gl_programs.idx", "gl_programs.bin",
            "vulkan_pipelines.bin", "vulkan_pipelines_debug.bin", "vulkan_pipeline_keys",
        )
        assertEquals(
            listOf(
                "gl_programs.bin", "gl_programs.idx", "vulkan_pipeline_keys", "vulkan_pipelines.bin",
                "vulkan_pipelines_debug.bin", "vulkan_shaders.bin", "vulkan_shaders.idx", "vulkan_shaders_debug.idx",
            ),
            LegacyGpuCache.select(found),
        )
    }
}
