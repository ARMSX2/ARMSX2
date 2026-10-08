package com.armsx2.config

/** How a game database entry sets a [Settings] field: to a value, or as a floor or a cap on the player's. */
internal sealed class DbSets {
    abstract val field: String

    data class Exact(override val field: String) : DbSets()
    data class AtLeast(override val field: String) : DbSets()
    data class AtMost(override val field: String) : DbSets()
}

/**
 * Which [Settings] field each game database entry sets, so a settings screen can show what the
 * game really runs rather than what the player has stored.
 *
 * Keyed by the entry's own name (what the log prints and what [GameDbOverrides.Entry.name]
 * carries), valued by the key [Settings.toJson] uses. The database's value is in the field's own
 * units for every entry here, which is what the pickers on the settings screens already assume
 * when they name an entry's value.
 *
 * Left out on purpose, so the row shows the player's value as before: entries with no setting in
 * this app (EE division rounding, FPU multiply, VIF FIFO, interlaced field shift, large-ST
 * rewrite), the blending cap that only applies where reading the frame being drawn is slow, which
 * depends on the device, two whose values are not in the same units as their setting
 * (trilinear filtering, CPU sprite size), and two the core applies only while the player's own
 * value is still the default (deinterlacing, hardware download mode).
 */
internal object GameDbFields {
    private fun exact(vararg pairs: Pair<String, String>): Map<String, DbSets> =
        pairs.associate { (entry, field) -> entry to DbSets.Exact(field) }

    val sets: Map<String, DbSets> = exact(
        // Rounding and clamping.
        "eeRoundMode" to "eeFpuRoundMode",
        "vu0RoundMode" to "vu0RoundMode",
        "vu1RoundMode" to "vu1RoundMode",
        "eeClampMode" to "eeClampMode",
        "vu0ClampMode" to "vuClampMode",
        "vu1ClampMode" to "vu1ClampMode",
        // Speed hacks.
        "mvuFlag" to "vuFlagHack",
        "instantVU1" to "vu1Instant",
        "mtvu" to "mtvu",
        "eeCycleRate" to "eeCycleRate",
        // Game fixes.
        "GoemonTlb" to "gamefixGoemonTlb",
        "SoftwareRendererFMV" to "gamefixSoftwareRendererFmv",
        "SkipMPEG" to "gamefixSkipMpeg",
        "OPHFlag" to "gamefixOphFlag",
        "EETiming" to "gamefixEETiming",
        "InstantDMA" to "gamefixInstantDma",
        "DMABusy" to "gamefixDmaBusy",
        "GIFFIFO" to "gamefixGifFifo",
        "VIF1Stall" to "gamefixVif1Stall",
        "VuAddSub" to "gamefixVuAddSub",
        "Ibit" to "gamefixIbit",
        "VUSync" to "gamefixVuSync",
        "VUOverflow" to "gamefixVuOverflow",
        "XGKick" to "gamefixXgkick",
        "BlitInternalFPS" to "gamefixBlitInternalFps",
        "FullVU0Sync" to "gamefixFullVu0Sync",
        // GS hardware fixes.
        "autoFlush" to "autoFlush",
        "cpuFramebufferConversion" to "cpuFramebufferConversion",
        "readTCOnClose" to "readTargetsWhenClosing",
        "disableDepthSupport" to "disableDepthEmulation",
        "preloadFrameData" to "preloadFrameData",
        "disablePartialInvalidation" to "disablePartialInvalidation",
        "textureInsideRT" to "textureInsideRt",
        "limit24BitDepth" to "limit24BitDepth",
        "alignSprite" to "alignSprite",
        "mergeSprite" to "mergeSprite",
        "mipmap" to "hwMipmap",
        "accurateAlphaTest" to "hwAccurateAlphaTest",
        "forceEvenSpritePosition" to "forceEvenSpritePosition",
        "bilinearUpscale" to "bilinearUpscale",
        "nativePaletteDraw" to "unscaledPaletteDraw",
        "estimateTextureRegion" to "estimateTextureRegion",
        "drawBuffering" to "drawBuffering",
        "PCRTCOffsets" to "screenOffsets",
        "PCRTCOverscan" to "showOverscan",
        "coalesceRenderPasses" to "coalesceRenderPasses",
        "skipDrawStart" to "skipDrawStart",
        "skipDrawEnd" to "skipDrawEnd",
        "halfPixelOffset" to "halfPixelOffset",
        "roundSprite" to "roundSprite",
        "nativeScaling" to "nativeScaling",
        "cpuSpriteRenderLevel" to "cpuSpriteRenderLevel",
        "cpuCLUTRender" to "cpuClutRender",
        "gpuTargetCLUT" to "gpuTargetClut",
        "gpuPaletteConversion" to "gpuPaletteConversion",
    ) + mapOf(
        "minimumBlendingLevel" to DbSets.AtLeast("accurateBlendingUnit"),
        "maximumBlendingLevel" to DbSets.AtMost("accurateBlendingUnit"),
        // The core takes the lower of the player's and the database's.
        "texturePreloading" to DbSets.AtMost("texturePreloading"),
    )

    /**
     * What the game runs for a field whose stored value is [player] when an entry of kind [how] sets
     * [db]: the database's value, or the player's pulled up to a floor or down to a cap. Null for a
     * field that is not a flag or a whole number. A flag is on for any non-zero database value.
     */
    fun effective(how: DbSets, player: Any?, db: Int): Any? = when (player) {
        is Boolean -> if (how is DbSets.Exact) db != 0 else null
        is Int -> when (how) {
            is DbSets.Exact -> db
            is DbSets.AtLeast -> maxOf(player, db)
            is DbSets.AtMost -> minOf(player, db)
        }
        else -> null
    }

    /** Whether [value], what a settings row now holds, is exactly the database's [db] for an entry of kind [how]. */
    fun isDatabaseValue(how: DbSets, value: Any?, db: Int): Boolean = when (value) {
        is Boolean -> how is DbSets.Exact && value == (db != 0)
        is Int -> value == db
        else -> false
    }
}
