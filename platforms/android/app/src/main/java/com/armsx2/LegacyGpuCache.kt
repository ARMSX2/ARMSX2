// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2

/**
 * The GPU cache files in the data root's `cache/` folder that builds from before the stamped cache
 * format wrote. No current build reads them: every cache today lives under `gs_packs/`, in
 * `vulkan_pipelines_<stamp>.bin` or `lsfg_spirv.cache`, and each carries a stamp of what it depends on
 * (driver, compiler, format) and checksums, and throws itself away when that does not match. So a new
 * install deletes only these, and keeps everything else in `cache/`: the current GPU caches, which a
 * wipe would make every game compile again after every app update, and the game list, achievement
 * images and catalog caches beside them.
 */
object LegacyGpuCache {
    private val legacyPrefixes = listOf("vulkan_shaders", "gl_programs")
    private val legacyNames = setOf("vulkan_pipelines.bin", "vulkan_pipelines_debug.bin", "vulkan_pipeline_keys")

    /** The names, of those found directly in `cache/`, that belong to the pre-stamp caches. */
    fun select(names: Collection<String>): List<String> =
        names.filter { name -> name in legacyNames || legacyPrefixes.any { name.startsWith(it) } }.sorted()
}
