package com.armsx2.config

import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONObject

/**
 * Resolved emulator config used to drive a VM launch / live-apply.
 *
 * Field naming convention: each field comments the upstream
 * `<section>/<key>` it maps to (grep against pcsx2 docs / Pcsx2Config.cpp).
 *
 * Settings are pushed via [applyTo] which calls NativeApp.setSetting per
 * field, then a single NativeApp.commitSettings to push the queued writes
 * into the running VM (or persist them for the next launch).
 *
 * Adding a new setting:
 *   1. Add a field with an upstream-matching default,
 *   2. Add a setSetting line in applyTo,
 *   3. Add the JSON mapping in toJson + fromJson + merge,
 *   4. Surface a widget in the appropriate Settings tab.
 */
data class Settings(
    // ---- EmuCore/Speedhacks ----
    /** EmuCore/Speedhacks/EECycleRate — −3..+3 (50%..300%). 0 = nominal. */
    val eeCycleRate: Int = 0,
    /** EmuCore/Speedhacks/EECycleSkip — 0..3. 0 = no skip. */
    val eeCycleSkip: Int = 0,
    /** EE/FPU clamp mode — 0 None / 1 Normal / 2 Extra / 3 Full (PCSX2 default Normal).
     *  Unpacks to EmuCore/CPU/Recompiler fpuOverflow/fpuExtraOverflow/fpuFullMode. */
    val eeClampMode: Int = 1,
    /** VU clamp mode — 0 None / 1 Normal / 2 Extra / 3 Extra+Sign (PCSX2 default Normal).
     *  Unpacks to vu0/vu1 Overflow/ExtraOverflow/SignOverflow. */
    val vuClampMode: Int = 1,
    /** EmuCore/Speedhacks/vuThread — Multi-Threaded VU1 (MTVU).
     *  Kept on by default for the mac ARM64 backend, but persisted normally
     *  so testers can A/B games which dislike MTVU. */
    val mtvu: Boolean = true,
    /** EmuCore/Speedhacks/vu1Instant — completes VU1 in one cycle. */
    val vu1Instant: Boolean = true,
    /** EmuCore/Speedhacks/vuFlagHack — skip VU flag computation when unread. */
    val vuFlagHack: Boolean = true,
    /** EmuCore/Speedhacks/fastCDVD — skip CDVD reads. */
    val fastCDVD: Boolean = false,
    /** EmuCore/Speedhacks/IntcStat — INTC_STAT register read hack. */
    val intcStat: Boolean = true,
    /** EmuCore/Speedhacks/WaitLoop — detect EE wait loops. */
    val waitLoop: Boolean = true,
    /** EmuCore/Speedhacks/vuNeonFusions — ARMSX2-only. Gates the arm64
     *  VU1 JIT NEON peephole fusions (MAC cluster
     *  MULAx+MADDAy+MADDAz+MADDw, OPMULA+OPMSUB cross-product). Default
     *  on — toggle off to A/B whether one of those JIT fusions is
     *  responsible for a per-game regression. */
    val vuNeonFusions: Boolean = true,
    /** EmuCore/Speedhacks/vuDeferredWrites — EXPERIMENTAL. Defers
     *  per-pair VF stores via the NEON cache; flush sites commit later.
     *  Big perf win on transform-heavy code. Known to break SH2 graphics
     *  and other games with cross-pair memory coherence assumptions. */
    val vuDeferredWrites: Boolean = false,
    /** EmuCore/Speedhacks/vuSkipStallSim — AGGRESSIVE. Skips the
     *  vu1_TestPipes_VU1 BL in the JIT — was 19-32% of total CPU on
     *  Futurama/GoW2/Ape Escape 3 per profiling. Breaks any game that
     *  relies on accurate FMAC/FDIV/EFU/IALU pipeline-stall timing. */
    val vuSkipStallSim: Boolean = false,

    // ---- EmuCore/GS — frame limiter ----
    /** EmuCore/GS/FrameLimitEnable. */
    val frameLimitEnable: Boolean = true,
    /** Framerate/NominalScalar expressed as a percent of native speed
     *  (100 = full speed ≈ 60fps NTSC / 50fps PAL). Applies when the Frame
     *  Limiter is on: lower values cap the FPS (50 ≈ 30fps), higher values
     *  fast-forward. Stored as percent; written to emucore as the 0.05..10.0
     *  float scalar. */
    val nominalSpeedPercent: Int = 100,
    /** Deprecated Android-only FPS cap. Kept for JSON compatibility with test
     *  builds, but no longer applied because it skipped GS rendering. */
    val fpsLimit: Int = 0,
    /** Deprecated Android-only frame skip. Kept for JSON compatibility only. */
    val frameSkip: Int = 0,

    // ---- Audio (SPU2/Output) ----
    /** SPU2/Output/StandardVolume — output volume %, 0..200 (100 = full). */
    val audioVolume: Int = 100,
    /** SPU2/Output/OutputMuted — mute audio output. */
    val audioMuted: Boolean = false,
    /** SPU2/Output/SyncMode — TimeStretch keeps pitch stable under load; off
     *  (Disabled) is lower CPU but drifts pitch when frame-time varies. */
    val audioTimeStretch: Boolean = true,
    /** SPU2/Output/BufferMS — audio buffer size (ms). Higher = fewer dropouts,
     *  more latency. 50 = default; raise if audio stutters on low-end devices. */
    val audioBufferMs: Int = 50,
    /** SPU2/Output/OutputLatencyMS — target output latency (ms). 20 = default. */
    val audioOutputLatencyMs: Int = 20,
    /** SPU2/Output/FastForwardVolume — output volume % while fast-forwarding. */
    val audioFastForwardVolume: Int = 100,

    // ---- EmuCore — patches / cheats ----
    /** EmuCore/EnablePatches — game-compatibility patches (default on). */
    val enablePatches: Boolean = true,
    /** EmuCore/EnableCheats — PNACH cheats. */
    val enableCheats: Boolean = false,
    /** EmuCore/EnableWideScreenPatches — 16:9 widescreen patches. */
    val enableWideScreenPatches: Boolean = false,
    /** EmuCore/EnableNoInterlacingPatches — no-interlacing patches. */
    val enableNoInterlacingPatches: Boolean = false,
    /** EmuCore/EnableFastBoot — skip BIOS splash and boot straight to the game. */
    val enableFastBoot: Boolean = false,
    /** EmuCore/EnableGameFixes — master switch for game-specific compatibility hacks. */
    val enableGameFixes: Boolean = false,
    /** EmuCore/Gamefixes/SoftwareRendererFMVHack. */
    val gamefixSoftwareRendererFmv: Boolean = false,
    /** EmuCore/Gamefixes/SkipMPEGHack. */
    val gamefixSkipMpeg: Boolean = false,
    /** EmuCore/Gamefixes/EETimingHack. */
    val gamefixEETiming: Boolean = false,
    /** EmuCore/Gamefixes/InstantDMAHack. */
    val gamefixInstantDma: Boolean = false,
    /** EmuCore/Gamefixes/BlitInternalFPSHack. */
    val gamefixBlitInternalFps: Boolean = false,
    /** EmuCore/Gamefixes/FpuMulHack — Tales of Destiny. */
    val gamefixFpuMul: Boolean = false,
    /** EmuCore/Gamefixes/OPHFlagHack — Bleach Blade Battlers. */
    val gamefixOphFlag: Boolean = false,
    /** EmuCore/Gamefixes/GIFFIFOHack — emulate the GIF FIFO (Test Drive Unlimited). */
    val gamefixGifFifo: Boolean = false,
    /** EmuCore/Gamefixes/DMABusyHack — Mana Khemia 1. */
    val gamefixDmaBusy: Boolean = false,
    /** EmuCore/Gamefixes/VIF1StallHack — delay VIF1 stalls (SOCOM 2 HUD). */
    val gamefixVif1Stall: Boolean = false,
    /** EmuCore/Gamefixes/IbitHack — Scarface, Crash Twinsanity. */
    val gamefixIbit: Boolean = false,
    /** EmuCore/Gamefixes/FullVU0SyncHack — tight VU0 sync on every COP2 op. */
    val gamefixFullVu0Sync: Boolean = false,
    /** EmuCore/Gamefixes/VuAddSubHack — Tri-Ace games. */
    val gamefixVuAddSub: Boolean = false,
    /** EmuCore/Gamefixes/VUOverflowHack — Superman Returns. */
    val gamefixVuOverflow: Boolean = false,
    /** EmuCore/Gamefixes/XgKickHack — extra XGKICK delay (Erementar Gerad). */
    val gamefixXgkick: Boolean = false,
    /** EmuCore/GS/SkipDuplicateFrames — skip presenting unchanged frames. PCSX2 default on. */
    val skipDuplicateFrames: Boolean = true,
    /** EmuCore/CPU/FPU.Roundmode — EE FPU rounding: 0 Nearest / 1 Negative / 2 Positive
     *  / 3 Chop. PS2 EE FPU default is Chop (toward zero). */
    val eeFpuRoundMode: Int = 3,

    /** EmuCore/GS/AspectRatio:
     *  0 Stretch · 1 Auto 4:3/3:2 · 2 4:3 · 3 16:9 · 4 10:7. */
    val aspectRatio: Int = 1,
    /** EmuCore/GS/deinterlace_mode — GSInterlaceMode:
     *  0 Auto · 1 Off · 2/3 Weave · 4/5 Bob · 6/7 Blend · 8/9 Adaptive. */
    val deinterlaceMode: Int = 0,

    // ---- DEV9 — PS2 HDD / Ethernet ----
    /** DEV9/Eth/EthEnable — PS2 network adapter. */
    val dev9EthEnable: Boolean = false,
    /** DEV9/Eth/EthApi — "Sockets" is the usable Android backend. */
    val dev9EthApi: String = "Sockets",
    /** DEV9/Eth/EthDevice — "Auto" lets the sockets backend choose. */
    val dev9EthDevice: String = "Auto",
    /** DEV9/Eth/EthLogDHCP — logs DHCP packets for network debugging. */
    val dev9EthLogDhcp: Boolean = false,
    /** DEV9/Eth/EthLogDNS — logs DNS packets for network debugging. */
    val dev9EthLogDns: Boolean = false,
    /** DEV9/Eth/InterceptDHCP — use PCSX2's internal DHCP replies. */
    val dev9InterceptDhcp: Boolean = false,
    val dev9Ps2Ip: String = "0.0.0.0",
    val dev9Mask: String = "0.0.0.0",
    val dev9Gateway: String = "0.0.0.0",
    val dev9Dns1: String = "0.0.0.0",
    val dev9Dns2: String = "0.0.0.0",
    val dev9AutoMask: Boolean = true,
    val dev9AutoGateway: Boolean = true,
    val dev9ModeDns1: String = "Auto",
    val dev9ModeDns2: String = "Auto",
    /** DEV9/Hdd/HddEnable — virtual PS2 HDD. */
    val dev9HddEnable: Boolean = false,
    /** DEV9/Hdd/HddFile — path/name of the virtual HDD image. */
    val dev9HddFile: String = "DEV9hdd.raw",

    // ---- MemoryCards ----
    val memoryCardSlot1Enabled: Boolean = true,
    val memoryCardSlot1Filename: String = "mcd001.ps2",
    val memoryCardSlot2Enabled: Boolean = true,
    val memoryCardSlot2Filename: String = "mcd002.ps2",

    // ---- EmuCore/CPU/Recompiler — recompiler enables ----
    /** EmuCore/CPU/Recompiler/EnableEE — EE (R5900) recompiler. */
    val recEE: Boolean = true,
    /** EmuCore/CPU/Recompiler/EnableIOP — IOP (R3000) recompiler. */
    val recIOP: Boolean = true,
    /** EmuCore/CPU/Recompiler/EnableVU0 — VU0 recompiler. */
    val recVU0: Boolean = true,
    /** EmuCore/CPU/Recompiler/EnableVU1 — VU1 recompiler. */
    val recVU1: Boolean = true,
    /** EmuCore/CPU/Recompiler/EnableFastmem — fastmem (page-fault backpatch
     *  signal handler). Disabling falls back to the slow VTLB read/write
     *  path on every memory op. */
    val enableFastmem: Boolean = true,

    // ---- macOS/PCSX2 ARM64 backend compatibility flags ----
    // Hidden from UI and forced on. Kept only so older JSON/INI/per-game blobs
    // with UseMac* keys still parse without losing the rest of their settings.
    /** EmuCore/CPU/Recompiler/UseMacEE — legacy, forced on. */
    val useMacEE: Boolean = true,
    /** EmuCore/CPU/Recompiler/UseMacIOP — legacy, forced on. */
    val useMacIOP: Boolean = true,
    /** EmuCore/CPU/Recompiler/UseMacVU0 — legacy, forced on. */
    val useMacVU0: Boolean = true,
    /** EmuCore/CPU/Recompiler/UseMacVU1 — legacy, forced on. */
    val useMacVU1: Boolean = true,

    // ---- microVU-style compile-time pipeline-stall folding ----
    /** EmuCore/CPU/Recompiler/Vu1InlineFmacStall — replace the per-pair
     *  `vu1_TestFMACStallReg / _Reg2` BLs (formerly 17-32% of total CPU per
     *  simpleperf) with an inline `Add VU1_CYCLE_REG, #fmac_stall`. Mirrors
     *  mac's compile-time mVUincCycles + mVUstall fold. Gated by the same
     *  `fmac_carry_safe` (ct_cycle > 3) guarantee that cross-block carry-in
     *  FMAC slots have retired at runtime. */
    val vu1InlineFmacStall: Boolean = false,
    /** EmuCore/CPU/Recompiler/Vu1CrossBlockPState — propagate predecessor's
     *  exit pipeline-state to successor block compile, so CARRY_IN_GATE_*
     *  bounds can shrink (FMAC/IALU=3, FDIV=12, EFU=54). When a predecessor
     *  links to a successor, the successor variant is specialised for that
     *  predecessor's exitState. Mirrors mac's microBlockManager pState match. */
    val vu1CrossBlockPState: Boolean = false,
    /** EmuCore/CPU/Recompiler/Vu1InlineDrainTestPipes — inline-emit the
     *  vu1_TestPipes_VU1 FMAC drain at JIT sites where the pre-walk proves
     *  FDIV/EFU/IALU are empty (skip_info[i].fmacOnlyTestPipes). Saves the BL
     *  + viCacheInvalidateAll + return overhead per call. Mac doesn't need
     *  this because it has no runtime FMAC ring — flag instances are routed
     *  at compile time. */
    val vu1InlineDrainTestPipes: Boolean = false,
    /** EmuCore/CPU/Recompiler/Vu1FmacInstanceRouting — mac-style 4-slot flag-
     *  instance routing. Repurposes VU->fmac[0..3].{mac,status,clip}flag as
     *  instance slots; skips the ring metadata Strs and the FMAC stall BLs.
     *  fmaccount stays 0 so vu1_TestPipes_VU1's FMAC drain early-exits. */
    val vu1FmacInstanceRouting: Boolean = false,

    // ---- EmuCore/GS — renderer accuracy / quality ----
    /** EmuCore/GS/hw_mipmap. */
    val hwMipmap: Boolean = true,
    /** EmuCore/GS/accurate_blending_unit — AccBlendLevel:
     *  0 Min · 1 Basic · 2 Medium · 3 High · 4 Full · 5 Maximum. */
    val accurateBlendingUnit: Int = 1,
    /** EmuCore/GS/filter — BiFiltering:
     *  0 Nearest · 1 Forced (Bilinear) · 2 PS2 · 3 Forced_But_Sprite. */
    val textureFiltering: Int = 2,
    /** EmuCore/GS/texture_preloading — TexturePreloadingLevel:
     *  0 Off · 1 Partial · 2 Full. */
    val texturePreloading: Int = 2,
    /** EmuCore/GS/HWDownloadMode — GSHardwareDownloadMode:
     *  0 Accurate · 1 Force Full · 2 No Readbacks · 3 Unsync · 4 Disabled. */
    val hardwareDownloadMode: Int = 0,
    /** EmuCore/GS/TVShader — CRT / TV shader preset. */
    val tvShader: Int = 0,
    /** EmuCore/GS/ShadeBoost. */
    val shadeBoost: Boolean = false,
    val shadeBoostBrightness: Int = 50,
    val shadeBoostContrast: Int = 50,
    val shadeBoostSaturation: Int = 50,
    val shadeBoostGamma: Int = 50,
    /** EmuCore/GS/LoadTextureReplacements. */
    val loadTextureReplacements: Boolean = false,
    /** EmuCore/GS/LoadTextureReplacementsAsync. */
    val loadTextureReplacementsAsync: Boolean = true,
    /** EmuCore/GS/PrecacheTextureReplacements. */
    val precacheTextureReplacements: Boolean = false,
    /** EmuCore/GS/DumpReplaceableTextures. */
    val dumpReplaceableTextures: Boolean = false,
    /** EmuCore/GS/OsdShowTextureReplacements. */
    val osdShowTextureReplacements: Boolean = false,
    // Performance Overlay element toggles. Default true to mirror native
    // initialize(), which turns every OsdShow* bit on at first boot.
    // Disabling GPU also stops the GPU timing queries (real perf win).
    /** EmuCore/GS/OsdShowFPS. */
    val osdShowFps: Boolean = true,
    /** EmuCore/GS/VsyncEnable — sync presentation to the display refresh (less
     *  tearing/smoother, slightly higher latency). Applies on game restart. */
    val vsyncEnable: Boolean = false,
    /** EmuCore/GS/OsdShowVPS. */
    val osdShowVps: Boolean = true,
    /** EmuCore/GS/OsdShowSpeed. */
    val osdShowSpeed: Boolean = true,
    /** EmuCore/GS/OsdShowCPU. */
    val osdShowCpu: Boolean = true,
    /** EmuCore/GS/OsdShowGPU. */
    val osdShowGpu: Boolean = true,
    /** EmuCore/GS/OsdShowResolution. */
    val osdShowResolution: Boolean = true,
    /** EmuCore/GS/OsdShowGSStats. */
    val osdShowGsStats: Boolean = true,
    /** EmuCore/GS/OsdShowFrameTimes. */
    val osdShowFrameTimes: Boolean = true,
    /** EmuCore/GS/UserHacks_AutoFlushLevel — GSHWAutoFlushLevel:
     *  0 Disabled · 1 SpritesOnly · 2 Enabled. */
    val autoFlush: Int = 0,
    /** EmuCore/GS/UserHacks_HalfPixelOffset — GSHalfPixelOffset:
     *  0 Off · 1 Normal · 2 Special · 3 SpecialAggressive · 4 Native · 5 NativeWTexOffset. */
    val halfPixelOffset: Int = 0,
    /** EmuCore/GS/UserHacks_Limit24BitDepth — 0 Off · 1 Upper · 2 Lower. */
    val limit24BitDepth: Int = 0,
    /** EmuCore/GS/UserHacks — master hardware-fixes toggle. */
    val manualUserHacks: Boolean = false,
    /** EmuCore/GS/UserHacks_TextureInsideRt — texture inside render target. */
    val textureInsideRt: Int = 0,
    /** EmuCore/GS/UserHacks_native_scaling — upscaling fixes/native scaling. */
    val nativeScaling: Int = 0,
    /** EmuCore/GS/UserHacks_round_sprite_offset. */
    val roundSprite: Int = 0,
    /** EmuCore/GS/UserHacks_BilinearHack. */
    val bilinearUpscale: Int = 0,
    /** EmuCore/GS/UserHacks_GPUTargetCLUTMode. */
    val gpuTargetClut: Int = 0,
    /** EmuCore/GS/UserHacks_CPUSpriteRenderBW. */
    val cpuSpriteRenderBw: Int = 0,
    /** EmuCore/GS/UserHacks_CPUSpriteRenderLevel. */
    val cpuSpriteRenderLevel: Int = 0,
    // ---- Additional PCSX2 hardware / upscaling fixes (full parity) ----
    // Upscaling fixes
    /** EmuCore/GS/UserHacks_align_sprite_X — Align Sprite (fixes vertical lines on some 2D upscales). */
    val alignSprite: Boolean = false,
    /** EmuCore/GS/UserHacks_merge_pp_sprite — Merge Sprite (fixes lines between post-process sprites). */
    val mergeSprite: Boolean = false,
    /** EmuCore/GS/UserHacks_ForceEvenSpritePosition — "Wild Arms" hack; forces even sprite/texture positions. */
    val forceEvenSpritePosition: Boolean = false,
    /** EmuCore/GS/UserHacks_NativePaletteDraw — Unscaled Palette Texture Draws. */
    val unscaledPaletteDraw: Boolean = false,
    /** EmuCore/GS/UserHacks_TCOffsetX — texture-coordinate X offset, 0..10000 (= 0..10 px ×1000). */
    val textureOffsetX: Int = 0,
    /** EmuCore/GS/UserHacks_TCOffsetY — texture-coordinate Y offset, 0..10000 (= 0..10 px ×1000). */
    val textureOffsetY: Int = 0,
    // Hardware fixes
    /** EmuCore/GS/paltex — GPU Palette Conversion. */
    val gpuPaletteConversion: Boolean = false,
    /** EmuCore/GS/UserHacks_CPU_FB_Conversion — CPU Framebuffer Conversion. */
    val cpuFramebufferConversion: Boolean = false,
    /** EmuCore/GS/UserHacks_ReadTCOnClose — Read Targets When Closing. */
    val readTargetsWhenClosing: Boolean = false,
    /** EmuCore/GS/UserHacks_DisableDepthSupport — Disable Depth Emulation. */
    val disableDepthEmulation: Boolean = false,
    /** EmuCore/GS/UserHacks_DisablePartialInvalidation — Disable Partial Source Invalidation. */
    val disablePartialInvalidation: Boolean = false,
    /** EmuCore/GS/UserHacks_Disable_Safe_Features — Disable Safe Features. */
    val disableSafeFeatures: Boolean = false,
    /** EmuCore/GS/UserHacks_DisableRenderFixes — Disable Render Fixes. */
    val disableRenderFixes: Boolean = false,
    /** EmuCore/GS/preload_frame_with_gs_data — Preload Frame Data. */
    val preloadFrameData: Boolean = false,
    /** EmuCore/GS/UserHacks_EstimateTextureRegion — Estimate Texture Region. */
    val estimateTextureRegion: Boolean = false,
    /** EmuCore/GS/UserHacks_CPUCLUTRender — CPU CLUT Render: 0 Off · 1 Normal · 2 Aggressive. */
    val cpuClutRender: Int = 0,
    /** EmuCore/GS/TriFilter — TriFiltering: -1 Auto · 0 Off · 1 PS2 · 2 Forced. */
    val triFilter: Int = -1,
    /** EmuCore/GS/MaxAnisotropy — 0 Off, else 2/4/8/16. */
    val maxAnisotropy: Int = 0,
    /** EmuCore/GS/AndroidGpuProfileOverride — 0 Auto · 1 Mali · 2 Adreno · 3 PowerVR.
     *  Stringified to "auto"/"mali"/"adreno"/"powervr" when written to emucore.
     *  Picked up in GSDeviceOGL::CheckFeatures at device init; requires
     *  a renderer restart to take effect. */
    val gpuProfile: Int = 0,
) {
    /** Push every field into emucore via NativeApp.setSetting + commit. */
    fun applyTo() {
        // Speedhacks
        NativeApp.setSetting("EmuCore/Speedhacks", "EECycleRate", "int", eeCycleRate.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "EECycleSkip", "int", eeCycleSkip.toString())
        // EE/FPU + VU clamping (recompiler accuracy). Each mode unpacks to the
        // PCSX2 bit flags below; both VUs get the same mode. Needs a recompiler
        // reset (commitSettings / game restart) to take effect.
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "fpuOverflow", "bool", (eeClampMode >= 1).toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "fpuExtraOverflow", "bool", (eeClampMode >= 2).toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "fpuFullMode", "bool", (eeClampMode >= 3).toString())
        for (vu in arrayOf("vu0", "vu1")) {
            NativeApp.setSetting("EmuCore/CPU/Recompiler", "${vu}Overflow", "bool", (vuClampMode >= 1).toString())
            NativeApp.setSetting("EmuCore/CPU/Recompiler", "${vu}ExtraOverflow", "bool", (vuClampMode >= 2).toString())
            NativeApp.setSetting("EmuCore/CPU/Recompiler", "${vu}SignOverflow", "bool", (vuClampMode >= 3).toString())
        }
        NativeApp.setSetting("EmuCore/Speedhacks", "vuThread", "bool", mtvu.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "vu1Instant", "bool", vu1Instant.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "vuFlagHack", "bool", vuFlagHack.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "fastCDVD", "bool", fastCDVD.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "IntcStat", "bool", intcStat.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "WaitLoop", "bool", waitLoop.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "vuNeonFusions", "bool", vuNeonFusions.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "vuDeferredWrites", "bool", vuDeferredWrites.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "vuSkipStallSim", "bool", vuSkipStallSim.toString())
        // GS frame limit. The setting key is persisted (read by runVMThread
        // after Initialize so cold starts honor the preference) AND the live
        // limiter mode is poked via speedhackLimitermode so toggling in-game
        // takes effect immediately. 0 = Nominal (capped at native rate),
        // 3 = Unlimited.
        NativeApp.setSetting("EmuCore/GS", "FrameLimitEnable", "bool", frameLimitEnable.toString())
        NativeApp.speedhackLimitermode(if (frameLimitEnable) 0 else 3)
        // Framerate/NominalScalar — custom speed / FPS cap as a fraction of
        // native. commitSettings → ApplySettings → CheckForEmulationSpeedConfigChanges
        // → UpdateTargetSpeed picks this up live. Clamp mirrors emucore's
        // EmulationSpeedOptions::SanityCheck (0.05..10.0).
        NativeApp.setSetting("Framerate", "NominalScalar", "float",
            (nominalSpeedPercent.coerceIn(10, 1000) / 100f).toString())
        // Live-apply: the setSetting above only persists; the running frame
        // pacer needs a direct re-pace (mirrors speedhackLimitermode).
        NativeApp.setNominalSpeed(nominalSpeedPercent.coerceIn(10, 1000))
        // Manual frameskip (0..5) — present 1 of every (N+1) frames. Held as a
        // GS-thread global, applied live; no persisted EmuCore key needed.
        NativeApp.setFrameSkip(frameSkip.coerceIn(0, 5))
        // Audio (SPU2). Volume/mute are live native setters; the rest are written
        // to the base layer and applied on commit (SPU2 stream reconfigure).
        NativeApp.setAudioVolume(audioVolume.coerceIn(0, 200))
        NativeApp.setAudioMuted(audioMuted)
        NativeApp.setSetting("SPU2/Output", "SyncMode", "string", if (audioTimeStretch) "TimeStretch" else "Disabled")
        NativeApp.setSetting("SPU2/Output", "BufferMS", "int", audioBufferMs.coerceIn(10, 200).toString())
        NativeApp.setSetting("SPU2/Output", "OutputLatencyMS", "int", audioOutputLatencyMs.coerceIn(5, 200).toString())
        NativeApp.setSetting("SPU2/Output", "FastForwardVolume", "int", audioFastForwardVolume.coerceIn(0, 200).toString())
        // Patches / cheats (EmuCore). Reloaded by ApplySettings →
        // CheckForPatchConfigChanges; widescreen/no-interlacing take effect on
        // the next boot for most games.
        NativeApp.setSetting("EmuCore", "EnablePatches", "bool", enablePatches.toString())
        NativeApp.setSetting("EmuCore", "EnableCheats", "bool", enableCheats.toString())
        NativeApp.setSetting("EmuCore", "EnableWideScreenPatches", "bool", enableWideScreenPatches.toString())
        NativeApp.setSetting("EmuCore", "EnableNoInterlacingPatches", "bool", enableNoInterlacingPatches.toString())
        NativeApp.setSetting("EmuCore", "EnableFastBoot", "bool", enableFastBoot.toString())
        NativeApp.setSetting("EmuCore", "EnableGameFixes", "bool", enableGameFixes.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "SoftwareRendererFMVHack", "bool", gamefixSoftwareRendererFmv.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "SkipMPEGHack", "bool", gamefixSkipMpeg.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "EETimingHack", "bool", gamefixEETiming.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "InstantDMAHack", "bool", gamefixInstantDma.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "BlitInternalFPSHack", "bool", gamefixBlitInternalFps.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "FpuMulHack", "bool", gamefixFpuMul.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "OPHFlagHack", "bool", gamefixOphFlag.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "GIFFIFOHack", "bool", gamefixGifFifo.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "DMABusyHack", "bool", gamefixDmaBusy.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "VIF1StallHack", "bool", gamefixVif1Stall.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "IbitHack", "bool", gamefixIbit.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "FullVU0SyncHack", "bool", gamefixFullVu0Sync.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "VuAddSubHack", "bool", gamefixVuAddSub.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "VUOverflowHack", "bool", gamefixVuOverflow.toString())
        NativeApp.setSetting("EmuCore/Gamefixes", "XgKickHack", "bool", gamefixXgkick.toString())
        NativeApp.setSetting("EmuCore/GS", "SkipDuplicateFrames", "bool", skipDuplicateFrames.toString())
        NativeApp.setSetting("EmuCore/CPU", "FPU.Roundmode", "int", eeFpuRoundMode.coerceIn(0, 3).toString())
        // Display + GS renderer + hardware/upscaling-fix keys are all written
        // together in writeGsToNative() below (shared with applyGsLive()).
        // DEV9. Networking/HDD are initialized with the VM, so changes
        // made from the in-game overlay are persisted for the next boot.
        NativeApp.setSetting("DEV9/Eth", "EthEnable", "bool", dev9EthEnable.toString())
        NativeApp.setSetting("DEV9/Eth", "EthApi", "string", dev9EthApi)
        NativeApp.setSetting("DEV9/Eth", "EthDevice", "string", dev9EthDevice.ifEmpty { "Auto" })
        NativeApp.setSetting("DEV9/Eth", "EthLogDHCP", "bool", dev9EthLogDhcp.toString())
        NativeApp.setSetting("DEV9/Eth", "EthLogDNS", "bool", dev9EthLogDns.toString())
        NativeApp.setSetting("DEV9/Eth", "InterceptDHCP", "bool", dev9InterceptDhcp.toString())
        NativeApp.setSetting("DEV9/Eth", "PS2IP", "string", dev9Ps2Ip.ifEmpty { "0.0.0.0" })
        NativeApp.setSetting("DEV9/Eth", "Mask", "string", dev9Mask.ifEmpty { "0.0.0.0" })
        NativeApp.setSetting("DEV9/Eth", "Gateway", "string", dev9Gateway.ifEmpty { "0.0.0.0" })
        NativeApp.setSetting("DEV9/Eth", "DNS1", "string", dev9Dns1.ifEmpty { "0.0.0.0" })
        NativeApp.setSetting("DEV9/Eth", "DNS2", "string", dev9Dns2.ifEmpty { "0.0.0.0" })
        NativeApp.setSetting("DEV9/Eth", "AutoMask", "bool", dev9AutoMask.toString())
        NativeApp.setSetting("DEV9/Eth", "AutoGateway", "bool", dev9AutoGateway.toString())
        NativeApp.setSetting("DEV9/Eth", "ModeDNS1", "string", dev9ModeDns1.ifEmpty { "Auto" })
        NativeApp.setSetting("DEV9/Eth", "ModeDNS2", "string", dev9ModeDns2.ifEmpty { "Auto" })
        NativeApp.setSetting("DEV9/Hdd", "HddEnable", "bool", dev9HddEnable.toString())
        NativeApp.setSetting("DEV9/Hdd", "HddFile", "string", dev9HddFile.ifEmpty { "DEV9hdd.raw" })
        NativeApp.setSetting("MemoryCards", "Slot1_Enable", "bool", memoryCardSlot1Enabled.toString())
        NativeApp.setSetting("MemoryCards", "Slot1_Filename", "string", memoryCardSlot1Filename.ifEmpty { "mcd001.ps2" })
        NativeApp.setSetting("MemoryCards", "Slot2_Enable", "bool", memoryCardSlot2Enabled.toString())
        NativeApp.setSetting("MemoryCards", "Slot2_Filename", "string", memoryCardSlot2Filename.ifEmpty { "mcd002.ps2" })
        // Recompiler enables. Picked up by VMManager::ApplySettings →
        // SysCpuProviderPack rebind. Toggling these on a running VM swaps
        // the dispatch pointer; existing JIT block caches are flushed by
        // ApplySettings's CpusChanged path.
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableEE", "bool", recEE.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableIOP", "bool", recIOP.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableVU0", "bool", recVU0.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableVU1", "bool", recVU1.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableFastmem", "bool", enableFastmem.toString())
        // Force the single macOS/PCSX2 ARM64 backend. VMManager also ignores
        // stale UseMac* values, but writing true cleans old persisted settings.
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacEE", "bool", "true")
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacIOP", "bool", "true")
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacVU0", "bool", "true")
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacVU1", "bool", "true")
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "Vu1InlineFmacStall", "bool", vu1InlineFmacStall.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "Vu1CrossBlockPState", "bool", vu1CrossBlockPState.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "Vu1InlineDrainTestPipes", "bool", vu1InlineDrainTestPipes.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "Vu1FmacInstanceRouting", "bool", vu1FmacInstanceRouting.toString())
        writeGsToNative()
        // Live convenience pokes. Harmless when the GS is closed; commitSettings()
        // below performs the authoritative apply for a cold start / restart.
        NativeApp.setAspectRatio(aspectRatio.coerceIn(0, 4))
        NativeApp.renderTvShader(tvShader.coerceIn(0, 7))
        NativeApp.renderShadeBoost(
            shadeBoost,
            shadeBoostBrightness.coerceIn(1, 100),
            shadeBoostContrast.coerceIn(1, 100),
            shadeBoostSaturation.coerceIn(1, 100),
            shadeBoostGamma.coerceIn(1, 100),
        )
        NativeApp.osdShowFPS(osdShowFps)
        NativeApp.osdShowVPS(osdShowVps)
        NativeApp.osdShowSpeed(osdShowSpeed)
        NativeApp.osdShowCPU(osdShowCpu)
        NativeApp.osdShowGPU(osdShowGpu)
        NativeApp.osdShowResolution(osdShowResolution)
        NativeApp.osdShowGSStats(osdShowGsStats)
        NativeApp.osdShowFrameTimes(osdShowFrameTimes)
        NativeApp.commitSettings()
    }

    /** Writes every EmuCore/GS key (display + renderer + hardware/upscaling
     *  fixes) into the native BASE settings layer. Pure persistence — no live
     *  pokes, no commit. Shared by [applyTo] (cold start / restart) and
     *  [applyGsLive] (running VM). Keep the key list in sync with
     *  Pcsx2Config::GSOptions::LoadSave. */
    private fun writeGsToNative() {
        val aspectRatioName = when (aspectRatio.coerceIn(0, 4)) {
            0 -> "Stretch"
            2 -> "4:3"
            3 -> "16:9"
            4 -> "10:7"
            else -> "Auto 4:3/3:2"
        }
        NativeApp.setSetting("EmuCore/GS", "AspectRatio", "string", aspectRatioName)
        NativeApp.setSetting("EmuCore/GS", "deinterlace_mode", "int", deinterlaceMode.coerceIn(0, 9).toString())
        NativeApp.setSetting("EmuCore/GS", "hw_mipmap", "bool", hwMipmap.toString())
        NativeApp.setSetting("EmuCore/GS", "accurate_blending_unit", "int", accurateBlendingUnit.toString())
        NativeApp.setSetting("EmuCore/GS", "filter", "int", textureFiltering.toString())
        NativeApp.setSetting("EmuCore/GS", "texture_preloading", "int", texturePreloading.toString())
        NativeApp.setSetting("EmuCore/GS", "HWDownloadMode", "int", hardwareDownloadMode.coerceIn(0, 4).toString())
        NativeApp.setSetting("EmuCore/GS", "TVShader", "int", tvShader.coerceIn(0, 7).toString())
        NativeApp.setSetting("EmuCore/GS", "ShadeBoost", "bool", shadeBoost.toString())
        NativeApp.setSetting("EmuCore/GS", "ShadeBoost_Brightness", "int", shadeBoostBrightness.coerceIn(1, 100).toString())
        NativeApp.setSetting("EmuCore/GS", "ShadeBoost_Contrast", "int", shadeBoostContrast.coerceIn(1, 100).toString())
        NativeApp.setSetting("EmuCore/GS", "ShadeBoost_Saturation", "int", shadeBoostSaturation.coerceIn(1, 100).toString())
        NativeApp.setSetting("EmuCore/GS", "ShadeBoost_Gamma", "int", shadeBoostGamma.coerceIn(1, 100).toString())
        NativeApp.setSetting("EmuCore/GS", "LoadTextureReplacements", "bool", loadTextureReplacements.toString())
        NativeApp.setSetting("EmuCore/GS", "LoadTextureReplacementsAsync", "bool", loadTextureReplacementsAsync.toString())
        NativeApp.setSetting("EmuCore/GS", "PrecacheTextureReplacements", "bool", precacheTextureReplacements.toString())
        NativeApp.setSetting("EmuCore/GS", "DumpReplaceableTextures", "bool", dumpReplaceableTextures.toString())
        NativeApp.setSetting("EmuCore/GS", "OsdShowTextureReplacements", "bool", osdShowTextureReplacements.toString())
        NativeApp.setSetting("EmuCore/GS", "OsdShowFPS", "bool", osdShowFps.toString())
        NativeApp.setSetting("EmuCore/GS", "VsyncEnable", "bool", vsyncEnable.toString())
        NativeApp.setSetting("EmuCore/GS", "OsdShowVPS", "bool", osdShowVps.toString())
        NativeApp.setSetting("EmuCore/GS", "OsdShowSpeed", "bool", osdShowSpeed.toString())
        NativeApp.setSetting("EmuCore/GS", "OsdShowCPU", "bool", osdShowCpu.toString())
        NativeApp.setSetting("EmuCore/GS", "OsdShowGPU", "bool", osdShowGpu.toString())
        NativeApp.setSetting("EmuCore/GS", "OsdShowResolution", "bool", osdShowResolution.toString())
        NativeApp.setSetting("EmuCore/GS", "OsdShowGSStats", "bool", osdShowGsStats.toString())
        NativeApp.setSetting("EmuCore/GS", "OsdShowFrameTimes", "bool", osdShowFrameTimes.toString())
        // Master hardware-fixes toggle. Auto-enables when ANY individual hack is
        // non-default so the user doesn't have to flip it; PCSX2 masks every
        // UserHacks_* key when this is off (GSOptions::MaskUserHacks).
        NativeApp.setSetting("EmuCore/GS", "UserHacks", "bool", anyUserHackEnabled().toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_AutoFlushLevel", "int", autoFlush.coerceIn(0, 2).toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_HalfPixelOffset", "int", halfPixelOffset.coerceIn(0, 5).toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_Limit24BitDepth", "int", limit24BitDepth.coerceIn(0, 2).toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_TextureInsideRt", "int", textureInsideRt.coerceIn(0, 2).toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_native_scaling", "int", nativeScaling.coerceIn(0, 4).toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_round_sprite_offset", "int", roundSprite.coerceIn(0, 2).toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_BilinearHack", "int", bilinearUpscale.coerceIn(0, 3).toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_GPUTargetCLUTMode", "int", gpuTargetClut.coerceIn(0, 2).toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_CPUSpriteRenderBW", "int", cpuSpriteRenderBw.coerceIn(0, 3).toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_CPUSpriteRenderLevel", "int", cpuSpriteRenderLevel.coerceIn(0, 5).toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_CPUCLUTRender", "int", cpuClutRender.coerceIn(0, 2).toString())
        // Upscaling fixes (parity additions)
        NativeApp.setSetting("EmuCore/GS", "UserHacks_align_sprite_X", "bool", alignSprite.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_merge_pp_sprite", "bool", mergeSprite.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_ForceEvenSpritePosition", "bool", forceEvenSpritePosition.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_NativePaletteDraw", "bool", unscaledPaletteDraw.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_TCOffsetX", "int", textureOffsetX.coerceIn(0, 10000).toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_TCOffsetY", "int", textureOffsetY.coerceIn(0, 10000).toString())
        // Hardware fixes (parity additions)
        NativeApp.setSetting("EmuCore/GS", "paltex", "bool", gpuPaletteConversion.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_CPU_FB_Conversion", "bool", cpuFramebufferConversion.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_ReadTCOnClose", "bool", readTargetsWhenClosing.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_DisableDepthSupport", "bool", disableDepthEmulation.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_DisablePartialInvalidation", "bool", disablePartialInvalidation.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_Disable_Safe_Features", "bool", disableSafeFeatures.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_DisableRenderFixes", "bool", disableRenderFixes.toString())
        NativeApp.setSetting("EmuCore/GS", "preload_frame_with_gs_data", "bool", preloadFrameData.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_EstimateTextureRegion", "bool", estimateTextureRegion.toString())
        NativeApp.setSetting("EmuCore/GS", "TriFilter", "int", triFilter.toString())
        NativeApp.setSetting("EmuCore/GS", "MaxAnisotropy", "int", maxAnisotropy.toString())
        val gpuProfileStr = when (gpuProfile) {
            1 -> "mali"
            2 -> "adreno"
            3 -> "powervr"
            else -> "auto"
        }
        NativeApp.setSetting("EmuCore/GS", "AndroidGpuProfileOverride", "string", gpuProfileStr)
    }

    /** True when any hardware/upscaling fix is non-default — used to auto-enable
     *  the UserHacks master so individual hacks aren't silently masked off. */
    private fun anyUserHackEnabled(): Boolean =
        manualUserHacks ||
            autoFlush != 0 || halfPixelOffset != 0 || limit24BitDepth != 0 ||
            textureInsideRt != 0 || nativeScaling != 0 || roundSprite != 0 ||
            bilinearUpscale != 0 || gpuTargetClut != 0 || cpuSpriteRenderBw != 0 ||
            cpuSpriteRenderLevel != 0 || cpuClutRender != 0 ||
            textureOffsetX != 0 || textureOffsetY != 0 ||
            alignSprite || mergeSprite || forceEvenSpritePosition || unscaledPaletteDraw ||
            gpuPaletteConversion || cpuFramebufferConversion || readTargetsWhenClosing ||
            disableDepthEmulation || disablePartialInvalidation || disableSafeFeatures ||
            disableRenderFixes || preloadFrameData || estimateTextureRegion

    /** Live GS-only apply for a running VM: persist all EmuCore/GS keys, then
     *  reconfigure the GS thread without the heavy CPU/JIT rebuild commitSettings()
     *  does. Lets renderer / hardware-fix / upscaling-fix changes apply instantly
     *  mid-game. */
    fun applyGsLive(): Boolean {
        writeGsToNative()
        return NativeApp.applyGSSettingsLive()
    }

    /** True when any field a live GS reconfigure ([applyGsLive]) can pick up
     *  differs from [other]. Lets the in-game delta path skip the GS thread
     *  park when only non-GS settings (audio, frame limit, …) changed.
     *  Excludes display aspect (its own live setter) and gpuProfile (device-init
     *  only — needs a renderer restart). */
    fun gsDiffersFrom(other: Settings): Boolean =
        deinterlaceMode != other.deinterlaceMode ||
            textureFiltering != other.textureFiltering ||
            texturePreloading != other.texturePreloading ||
            hardwareDownloadMode != other.hardwareDownloadMode ||
            tvShader != other.tvShader ||
            shadeBoost != other.shadeBoost ||
            shadeBoostBrightness != other.shadeBoostBrightness ||
            shadeBoostContrast != other.shadeBoostContrast ||
            shadeBoostSaturation != other.shadeBoostSaturation ||
            shadeBoostGamma != other.shadeBoostGamma ||
            accurateBlendingUnit != other.accurateBlendingUnit ||
            hwMipmap != other.hwMipmap ||
            triFilter != other.triFilter ||
            maxAnisotropy != other.maxAnisotropy ||
            manualUserHacks != other.manualUserHacks ||
            autoFlush != other.autoFlush ||
            halfPixelOffset != other.halfPixelOffset ||
            limit24BitDepth != other.limit24BitDepth ||
            textureInsideRt != other.textureInsideRt ||
            nativeScaling != other.nativeScaling ||
            roundSprite != other.roundSprite ||
            bilinearUpscale != other.bilinearUpscale ||
            gpuTargetClut != other.gpuTargetClut ||
            cpuSpriteRenderBw != other.cpuSpriteRenderBw ||
            cpuSpriteRenderLevel != other.cpuSpriteRenderLevel ||
            cpuClutRender != other.cpuClutRender ||
            alignSprite != other.alignSprite ||
            mergeSprite != other.mergeSprite ||
            forceEvenSpritePosition != other.forceEvenSpritePosition ||
            unscaledPaletteDraw != other.unscaledPaletteDraw ||
            textureOffsetX != other.textureOffsetX ||
            textureOffsetY != other.textureOffsetY ||
            gpuPaletteConversion != other.gpuPaletteConversion ||
            cpuFramebufferConversion != other.cpuFramebufferConversion ||
            readTargetsWhenClosing != other.readTargetsWhenClosing ||
            disableDepthEmulation != other.disableDepthEmulation ||
            disablePartialInvalidation != other.disablePartialInvalidation ||
            disableSafeFeatures != other.disableSafeFeatures ||
            disableRenderFixes != other.disableRenderFixes ||
            preloadFrameData != other.preloadFrameData ||
            estimateTextureRegion != other.estimateTextureRegion

    fun toJson(): JSONObject = JSONObject().apply {
        put("eeCycleRate", eeCycleRate)
        put("eeCycleSkip", eeCycleSkip)
        put("eeClampMode", eeClampMode)
        put("vuClampMode", vuClampMode)
        put("mtvu", mtvu)
        put("vu1Instant", vu1Instant)
        put("vuFlagHack", vuFlagHack)
        put("fastCDVD", fastCDVD)
        put("intcStat", intcStat)
        put("waitLoop", waitLoop)
        put("vuNeonFusions", vuNeonFusions)
        put("vuDeferredWrites", vuDeferredWrites)
        put("vuSkipStallSim", vuSkipStallSim)
        put("frameLimitEnable", frameLimitEnable)
        put("nominalSpeedPercent", nominalSpeedPercent)
        put("fpsLimit", fpsLimit)
        put("frameSkip", frameSkip)
        put("audioVolume", audioVolume)
        put("audioMuted", audioMuted)
        put("audioTimeStretch", audioTimeStretch)
        put("audioBufferMs", audioBufferMs)
        put("audioOutputLatencyMs", audioOutputLatencyMs)
        put("audioFastForwardVolume", audioFastForwardVolume)
        put("enablePatches", enablePatches)
        put("enableCheats", enableCheats)
        put("enableWideScreenPatches", enableWideScreenPatches)
        put("enableNoInterlacingPatches", enableNoInterlacingPatches)
        put("enableFastBoot", enableFastBoot)
        put("enableGameFixes", enableGameFixes)
        put("gamefixSoftwareRendererFmv", gamefixSoftwareRendererFmv)
        put("gamefixSkipMpeg", gamefixSkipMpeg)
        put("gamefixEETiming", gamefixEETiming)
        put("gamefixInstantDma", gamefixInstantDma)
        put("gamefixBlitInternalFps", gamefixBlitInternalFps)
        put("gamefixFpuMul", gamefixFpuMul)
        put("gamefixOphFlag", gamefixOphFlag)
        put("gamefixGifFifo", gamefixGifFifo)
        put("gamefixDmaBusy", gamefixDmaBusy)
        put("gamefixVif1Stall", gamefixVif1Stall)
        put("gamefixIbit", gamefixIbit)
        put("gamefixFullVu0Sync", gamefixFullVu0Sync)
        put("gamefixVuAddSub", gamefixVuAddSub)
        put("gamefixVuOverflow", gamefixVuOverflow)
        put("gamefixXgkick", gamefixXgkick)
        put("skipDuplicateFrames", skipDuplicateFrames)
        put("eeFpuRoundMode", eeFpuRoundMode)
        put("aspectRatio", aspectRatio)
        put("deinterlaceMode", deinterlaceMode)
        put("dev9EthEnable", dev9EthEnable)
        put("dev9EthApi", dev9EthApi)
        put("dev9EthDevice", dev9EthDevice)
        put("dev9EthLogDhcp", dev9EthLogDhcp)
        put("dev9EthLogDns", dev9EthLogDns)
        put("dev9InterceptDhcp", dev9InterceptDhcp)
        put("dev9Ps2Ip", dev9Ps2Ip)
        put("dev9Mask", dev9Mask)
        put("dev9Gateway", dev9Gateway)
        put("dev9Dns1", dev9Dns1)
        put("dev9Dns2", dev9Dns2)
        put("dev9AutoMask", dev9AutoMask)
        put("dev9AutoGateway", dev9AutoGateway)
        put("dev9ModeDns1", dev9ModeDns1)
        put("dev9ModeDns2", dev9ModeDns2)
        put("dev9HddEnable", dev9HddEnable)
        put("dev9HddFile", dev9HddFile)
        put("memoryCardSlot1Enabled", memoryCardSlot1Enabled)
        put("memoryCardSlot1Filename", memoryCardSlot1Filename)
        put("memoryCardSlot2Enabled", memoryCardSlot2Enabled)
        put("memoryCardSlot2Filename", memoryCardSlot2Filename)
        put("recEE", recEE)
        put("recIOP", recIOP)
        put("recVU0", recVU0)
        put("recVU1", recVU1)
        put("enableFastmem", enableFastmem)
        put("vu1InlineFmacStall", vu1InlineFmacStall)
        put("vu1CrossBlockPState", vu1CrossBlockPState)
        put("vu1InlineDrainTestPipes", vu1InlineDrainTestPipes)
        put("vu1FmacInstanceRouting", vu1FmacInstanceRouting)
        put("hwMipmap", hwMipmap)
        put("accurateBlendingUnit", accurateBlendingUnit)
        put("textureFiltering", textureFiltering)
        put("texturePreloading", texturePreloading)
        put("hardwareDownloadMode", hardwareDownloadMode)
        put("tvShader", tvShader)
        put("shadeBoost", shadeBoost)
        put("shadeBoostBrightness", shadeBoostBrightness)
        put("shadeBoostContrast", shadeBoostContrast)
        put("shadeBoostSaturation", shadeBoostSaturation)
        put("shadeBoostGamma", shadeBoostGamma)
        put("loadTextureReplacements", loadTextureReplacements)
        put("loadTextureReplacementsAsync", loadTextureReplacementsAsync)
        put("precacheTextureReplacements", precacheTextureReplacements)
        put("dumpReplaceableTextures", dumpReplaceableTextures)
        put("osdShowTextureReplacements", osdShowTextureReplacements)
        put("osdShowFps", osdShowFps)
        put("vsyncEnable", vsyncEnable)
        put("osdShowVps", osdShowVps)
        put("osdShowSpeed", osdShowSpeed)
        put("osdShowCpu", osdShowCpu)
        put("osdShowGpu", osdShowGpu)
        put("osdShowResolution", osdShowResolution)
        put("osdShowGsStats", osdShowGsStats)
        put("osdShowFrameTimes", osdShowFrameTimes)
        put("autoFlush", autoFlush)
        put("halfPixelOffset", halfPixelOffset)
        put("limit24BitDepth", limit24BitDepth)
        put("manualUserHacks", manualUserHacks)
        put("textureInsideRt", textureInsideRt)
        put("nativeScaling", nativeScaling)
        put("roundSprite", roundSprite)
        put("bilinearUpscale", bilinearUpscale)
        put("gpuTargetClut", gpuTargetClut)
        put("cpuSpriteRenderBw", cpuSpriteRenderBw)
        put("cpuSpriteRenderLevel", cpuSpriteRenderLevel)
        put("alignSprite", alignSprite)
        put("mergeSprite", mergeSprite)
        put("forceEvenSpritePosition", forceEvenSpritePosition)
        put("unscaledPaletteDraw", unscaledPaletteDraw)
        put("textureOffsetX", textureOffsetX)
        put("textureOffsetY", textureOffsetY)
        put("gpuPaletteConversion", gpuPaletteConversion)
        put("cpuFramebufferConversion", cpuFramebufferConversion)
        put("readTargetsWhenClosing", readTargetsWhenClosing)
        put("disableDepthEmulation", disableDepthEmulation)
        put("disablePartialInvalidation", disablePartialInvalidation)
        put("disableSafeFeatures", disableSafeFeatures)
        put("disableRenderFixes", disableRenderFixes)
        put("preloadFrameData", preloadFrameData)
        put("estimateTextureRegion", estimateTextureRegion)
        put("cpuClutRender", cpuClutRender)
        put("triFilter", triFilter)
        put("maxAnisotropy", maxAnisotropy)
        put("gpuProfile", gpuProfile)
    }

    companion object {
        /** Lenient parse — missing keys fall back to defaults so old saved
         *  blobs survive when new fields are added. */
        fun fromJson(json: JSONObject): Settings {
            val def = Settings()
            return Settings(
                eeCycleRate = json.optInt("eeCycleRate", def.eeCycleRate),
                eeCycleSkip = json.optInt("eeCycleSkip", def.eeCycleSkip),
                eeClampMode = json.optInt("eeClampMode", def.eeClampMode),
                vuClampMode = json.optInt("vuClampMode", def.vuClampMode),
                mtvu = json.optBoolean("mtvu", def.mtvu),
                vu1Instant = json.optBoolean("vu1Instant", def.vu1Instant),
                vuFlagHack = json.optBoolean("vuFlagHack", def.vuFlagHack),
                fastCDVD = json.optBoolean("fastCDVD", def.fastCDVD),
                intcStat = json.optBoolean("intcStat", def.intcStat),
                waitLoop = json.optBoolean("waitLoop", def.waitLoop),
                vuNeonFusions = json.optBoolean("vuNeonFusions", def.vuNeonFusions),
                vuDeferredWrites = json.optBoolean("vuDeferredWrites", def.vuDeferredWrites),
                vuSkipStallSim = json.optBoolean("vuSkipStallSim", def.vuSkipStallSim),
                frameLimitEnable = json.optBoolean("frameLimitEnable", def.frameLimitEnable),
                nominalSpeedPercent = json.optInt("nominalSpeedPercent", def.nominalSpeedPercent),
                fpsLimit = json.optInt("fpsLimit", def.fpsLimit),
                frameSkip = json.optInt("frameSkip", def.frameSkip),
                audioVolume = json.optInt("audioVolume", def.audioVolume),
                audioMuted = json.optBoolean("audioMuted", def.audioMuted),
                audioTimeStretch = json.optBoolean("audioTimeStretch", def.audioTimeStretch),
                audioBufferMs = json.optInt("audioBufferMs", def.audioBufferMs),
                audioOutputLatencyMs = json.optInt("audioOutputLatencyMs", def.audioOutputLatencyMs),
                audioFastForwardVolume = json.optInt("audioFastForwardVolume", def.audioFastForwardVolume),
                enablePatches = json.optBoolean("enablePatches", def.enablePatches),
                enableCheats = json.optBoolean("enableCheats", def.enableCheats),
                enableWideScreenPatches = json.optBoolean("enableWideScreenPatches", def.enableWideScreenPatches),
                enableNoInterlacingPatches = json.optBoolean("enableNoInterlacingPatches", def.enableNoInterlacingPatches),
                enableFastBoot = json.optBoolean("enableFastBoot", def.enableFastBoot),
                enableGameFixes = json.optBoolean("enableGameFixes", def.enableGameFixes),
                gamefixSoftwareRendererFmv = json.optBoolean("gamefixSoftwareRendererFmv", def.gamefixSoftwareRendererFmv),
                gamefixSkipMpeg = json.optBoolean("gamefixSkipMpeg", def.gamefixSkipMpeg),
                gamefixEETiming = json.optBoolean("gamefixEETiming", def.gamefixEETiming),
                gamefixInstantDma = json.optBoolean("gamefixInstantDma", def.gamefixInstantDma),
                gamefixBlitInternalFps = json.optBoolean("gamefixBlitInternalFps", def.gamefixBlitInternalFps),
                gamefixFpuMul = json.optBoolean("gamefixFpuMul", def.gamefixFpuMul),
                gamefixOphFlag = json.optBoolean("gamefixOphFlag", def.gamefixOphFlag),
                gamefixGifFifo = json.optBoolean("gamefixGifFifo", def.gamefixGifFifo),
                gamefixDmaBusy = json.optBoolean("gamefixDmaBusy", def.gamefixDmaBusy),
                gamefixVif1Stall = json.optBoolean("gamefixVif1Stall", def.gamefixVif1Stall),
                gamefixIbit = json.optBoolean("gamefixIbit", def.gamefixIbit),
                gamefixFullVu0Sync = json.optBoolean("gamefixFullVu0Sync", def.gamefixFullVu0Sync),
                gamefixVuAddSub = json.optBoolean("gamefixVuAddSub", def.gamefixVuAddSub),
                gamefixVuOverflow = json.optBoolean("gamefixVuOverflow", def.gamefixVuOverflow),
                gamefixXgkick = json.optBoolean("gamefixXgkick", def.gamefixXgkick),
                skipDuplicateFrames = json.optBoolean("skipDuplicateFrames", def.skipDuplicateFrames),
                eeFpuRoundMode = json.optInt("eeFpuRoundMode", def.eeFpuRoundMode),
                aspectRatio = json.optInt("aspectRatio", def.aspectRatio),
                deinterlaceMode = json.optInt("deinterlaceMode", def.deinterlaceMode),
                dev9EthEnable = json.optBoolean("dev9EthEnable", def.dev9EthEnable),
                dev9EthApi = json.optString("dev9EthApi", def.dev9EthApi).ifEmpty { def.dev9EthApi },
                dev9EthDevice = json.optString("dev9EthDevice", def.dev9EthDevice).ifEmpty { def.dev9EthDevice },
                dev9EthLogDhcp = json.optBoolean("dev9EthLogDhcp", def.dev9EthLogDhcp),
                dev9EthLogDns = json.optBoolean("dev9EthLogDns", def.dev9EthLogDns),
                dev9InterceptDhcp = json.optBoolean("dev9InterceptDhcp", def.dev9InterceptDhcp),
                dev9Ps2Ip = json.optString("dev9Ps2Ip", def.dev9Ps2Ip).ifEmpty { def.dev9Ps2Ip },
                dev9Mask = json.optString("dev9Mask", def.dev9Mask).ifEmpty { def.dev9Mask },
                dev9Gateway = json.optString("dev9Gateway", def.dev9Gateway).ifEmpty { def.dev9Gateway },
                dev9Dns1 = json.optString("dev9Dns1", def.dev9Dns1).ifEmpty { def.dev9Dns1 },
                dev9Dns2 = json.optString("dev9Dns2", def.dev9Dns2).ifEmpty { def.dev9Dns2 },
                dev9AutoMask = json.optBoolean("dev9AutoMask", def.dev9AutoMask),
                dev9AutoGateway = json.optBoolean("dev9AutoGateway", def.dev9AutoGateway),
                dev9ModeDns1 = json.optString("dev9ModeDns1", def.dev9ModeDns1).ifEmpty { def.dev9ModeDns1 },
                dev9ModeDns2 = json.optString("dev9ModeDns2", def.dev9ModeDns2).ifEmpty { def.dev9ModeDns2 },
                dev9HddEnable = json.optBoolean("dev9HddEnable", def.dev9HddEnable),
                dev9HddFile = json.optString("dev9HddFile", def.dev9HddFile).ifEmpty { def.dev9HddFile },
                memoryCardSlot1Enabled = json.optBoolean("memoryCardSlot1Enabled", def.memoryCardSlot1Enabled),
                memoryCardSlot1Filename = json.optString("memoryCardSlot1Filename", def.memoryCardSlot1Filename).ifEmpty { def.memoryCardSlot1Filename },
                memoryCardSlot2Enabled = json.optBoolean("memoryCardSlot2Enabled", def.memoryCardSlot2Enabled),
                memoryCardSlot2Filename = json.optString("memoryCardSlot2Filename", def.memoryCardSlot2Filename).ifEmpty { def.memoryCardSlot2Filename },
                recEE = json.optBoolean("recEE", def.recEE),
                recIOP = json.optBoolean("recIOP", def.recIOP),
                recVU0 = json.optBoolean("recVU0", def.recVU0),
                recVU1 = json.optBoolean("recVU1", def.recVU1),
                enableFastmem = json.optBoolean("enableFastmem", def.enableFastmem),
                useMacEE = true,
                useMacIOP = true,
                useMacVU0 = true,
                useMacVU1 = true,
                vu1InlineFmacStall = json.optBoolean("vu1InlineFmacStall", def.vu1InlineFmacStall),
                vu1CrossBlockPState = json.optBoolean("vu1CrossBlockPState", def.vu1CrossBlockPState),
                vu1InlineDrainTestPipes = json.optBoolean("vu1InlineDrainTestPipes", def.vu1InlineDrainTestPipes),
                vu1FmacInstanceRouting = json.optBoolean("vu1FmacInstanceRouting", def.vu1FmacInstanceRouting),
                hwMipmap = json.optBoolean("hwMipmap", def.hwMipmap),
                accurateBlendingUnit = json.optInt("accurateBlendingUnit", def.accurateBlendingUnit),
                textureFiltering = json.optInt("textureFiltering", def.textureFiltering),
                texturePreloading = json.optInt("texturePreloading", def.texturePreloading),
                hardwareDownloadMode = json.optInt("hardwareDownloadMode", def.hardwareDownloadMode),
                tvShader = json.optInt("tvShader", def.tvShader),
                shadeBoost = json.optBoolean("shadeBoost", def.shadeBoost),
                shadeBoostBrightness = json.optInt("shadeBoostBrightness", def.shadeBoostBrightness),
                shadeBoostContrast = json.optInt("shadeBoostContrast", def.shadeBoostContrast),
                shadeBoostSaturation = json.optInt("shadeBoostSaturation", def.shadeBoostSaturation),
                shadeBoostGamma = json.optInt("shadeBoostGamma", def.shadeBoostGamma),
                loadTextureReplacements = json.optBoolean("loadTextureReplacements", def.loadTextureReplacements),
                loadTextureReplacementsAsync = json.optBoolean("loadTextureReplacementsAsync", def.loadTextureReplacementsAsync),
                precacheTextureReplacements = json.optBoolean("precacheTextureReplacements", def.precacheTextureReplacements),
                dumpReplaceableTextures = json.optBoolean("dumpReplaceableTextures", def.dumpReplaceableTextures),
                osdShowTextureReplacements = json.optBoolean("osdShowTextureReplacements", def.osdShowTextureReplacements),
                osdShowFps = json.optBoolean("osdShowFps", def.osdShowFps),
                vsyncEnable = json.optBoolean("vsyncEnable", def.vsyncEnable),
                osdShowVps = json.optBoolean("osdShowVps", def.osdShowVps),
                osdShowSpeed = json.optBoolean("osdShowSpeed", def.osdShowSpeed),
                osdShowCpu = json.optBoolean("osdShowCpu", def.osdShowCpu),
                osdShowGpu = json.optBoolean("osdShowGpu", def.osdShowGpu),
                osdShowResolution = json.optBoolean("osdShowResolution", def.osdShowResolution),
                osdShowGsStats = json.optBoolean("osdShowGsStats", def.osdShowGsStats),
                osdShowFrameTimes = json.optBoolean("osdShowFrameTimes", def.osdShowFrameTimes),
                autoFlush = json.optInt("autoFlush", def.autoFlush),
                halfPixelOffset = json.optInt("halfPixelOffset", def.halfPixelOffset),
                limit24BitDepth = json.optInt("limit24BitDepth", def.limit24BitDepth),
                manualUserHacks = json.optBoolean("manualUserHacks", def.manualUserHacks),
                textureInsideRt = json.optInt("textureInsideRt", def.textureInsideRt),
                nativeScaling = json.optInt("nativeScaling", def.nativeScaling),
                roundSprite = json.optInt("roundSprite", def.roundSprite),
                bilinearUpscale = json.optInt("bilinearUpscale", def.bilinearUpscale),
                gpuTargetClut = json.optInt("gpuTargetClut", def.gpuTargetClut),
                cpuSpriteRenderBw = json.optInt("cpuSpriteRenderBw", def.cpuSpriteRenderBw),
                cpuSpriteRenderLevel = json.optInt("cpuSpriteRenderLevel", def.cpuSpriteRenderLevel),
                alignSprite = json.optBoolean("alignSprite", def.alignSprite),
                mergeSprite = json.optBoolean("mergeSprite", def.mergeSprite),
                forceEvenSpritePosition = json.optBoolean("forceEvenSpritePosition", def.forceEvenSpritePosition),
                unscaledPaletteDraw = json.optBoolean("unscaledPaletteDraw", def.unscaledPaletteDraw),
                textureOffsetX = json.optInt("textureOffsetX", def.textureOffsetX),
                textureOffsetY = json.optInt("textureOffsetY", def.textureOffsetY),
                gpuPaletteConversion = json.optBoolean("gpuPaletteConversion", def.gpuPaletteConversion),
                cpuFramebufferConversion = json.optBoolean("cpuFramebufferConversion", def.cpuFramebufferConversion),
                readTargetsWhenClosing = json.optBoolean("readTargetsWhenClosing", def.readTargetsWhenClosing),
                disableDepthEmulation = json.optBoolean("disableDepthEmulation", def.disableDepthEmulation),
                disablePartialInvalidation = json.optBoolean("disablePartialInvalidation", def.disablePartialInvalidation),
                disableSafeFeatures = json.optBoolean("disableSafeFeatures", def.disableSafeFeatures),
                disableRenderFixes = json.optBoolean("disableRenderFixes", def.disableRenderFixes),
                preloadFrameData = json.optBoolean("preloadFrameData", def.preloadFrameData),
                estimateTextureRegion = json.optBoolean("estimateTextureRegion", def.estimateTextureRegion),
                cpuClutRender = json.optInt("cpuClutRender", def.cpuClutRender),
                triFilter = json.optInt("triFilter", def.triFilter),
                maxAnisotropy = json.optInt("maxAnisotropy", def.maxAnisotropy),
                gpuProfile = json.optInt("gpuProfile", def.gpuProfile),
            )
        }

        /** Treat any field present in [overrides] as a delta over [base]. */
        /**
         * Compute the sparse override JSON between two Settings: returns
         * only fields where `current` differs from `base`. Used by the
         * overlay's per-game save path so we only persist what the user
         * actually changed for this title — global tweaks still flow
         * through fields the user hasn't touched. Mirrors the field set
         * of [merge] above (must stay in sync).
         */
        fun diff(base: Settings, current: Settings): JSONObject {
            val j = JSONObject()
            if (current.eeCycleRate         != base.eeCycleRate)         j.put("eeCycleRate", current.eeCycleRate)
            if (current.eeCycleSkip         != base.eeCycleSkip)         j.put("eeCycleSkip", current.eeCycleSkip)
            if (current.eeClampMode         != base.eeClampMode)         j.put("eeClampMode", current.eeClampMode)
            if (current.vuClampMode         != base.vuClampMode)         j.put("vuClampMode", current.vuClampMode)
            if (current.mtvu                != base.mtvu)                j.put("mtvu", current.mtvu)
            if (current.vu1Instant          != base.vu1Instant)          j.put("vu1Instant", current.vu1Instant)
            if (current.vuFlagHack          != base.vuFlagHack)          j.put("vuFlagHack", current.vuFlagHack)
            if (current.fastCDVD            != base.fastCDVD)            j.put("fastCDVD", current.fastCDVD)
            if (current.intcStat            != base.intcStat)            j.put("intcStat", current.intcStat)
            if (current.waitLoop            != base.waitLoop)            j.put("waitLoop", current.waitLoop)
            if (current.vuNeonFusions       != base.vuNeonFusions)       j.put("vuNeonFusions", current.vuNeonFusions)
            if (current.vuDeferredWrites    != base.vuDeferredWrites)    j.put("vuDeferredWrites", current.vuDeferredWrites)
            if (current.vuSkipStallSim      != base.vuSkipStallSim)      j.put("vuSkipStallSim", current.vuSkipStallSim)
            if (current.frameLimitEnable    != base.frameLimitEnable)    j.put("frameLimitEnable", current.frameLimitEnable)
            if (current.nominalSpeedPercent != base.nominalSpeedPercent) j.put("nominalSpeedPercent", current.nominalSpeedPercent)
            if (current.fpsLimit            != base.fpsLimit)            j.put("fpsLimit", current.fpsLimit)
            if (current.frameSkip != base.frameSkip) j.put("frameSkip", current.frameSkip)
            if (current.audioVolume != base.audioVolume) j.put("audioVolume", current.audioVolume)
            if (current.audioMuted != base.audioMuted) j.put("audioMuted", current.audioMuted)
            if (current.audioTimeStretch != base.audioTimeStretch) j.put("audioTimeStretch", current.audioTimeStretch)
            if (current.audioBufferMs != base.audioBufferMs) j.put("audioBufferMs", current.audioBufferMs)
            if (current.audioOutputLatencyMs != base.audioOutputLatencyMs) j.put("audioOutputLatencyMs", current.audioOutputLatencyMs)
            if (current.audioFastForwardVolume != base.audioFastForwardVolume) j.put("audioFastForwardVolume", current.audioFastForwardVolume)
            if (current.enablePatches != base.enablePatches) j.put("enablePatches", current.enablePatches)
            if (current.enableCheats != base.enableCheats) j.put("enableCheats", current.enableCheats)
            if (current.enableWideScreenPatches != base.enableWideScreenPatches) j.put("enableWideScreenPatches", current.enableWideScreenPatches)
            if (current.enableNoInterlacingPatches != base.enableNoInterlacingPatches) j.put("enableNoInterlacingPatches", current.enableNoInterlacingPatches)
            if (current.enableFastBoot != base.enableFastBoot) j.put("enableFastBoot", current.enableFastBoot)
            if (current.enableGameFixes != base.enableGameFixes) j.put("enableGameFixes", current.enableGameFixes)
            if (current.gamefixSoftwareRendererFmv != base.gamefixSoftwareRendererFmv) j.put("gamefixSoftwareRendererFmv", current.gamefixSoftwareRendererFmv)
            if (current.gamefixSkipMpeg != base.gamefixSkipMpeg) j.put("gamefixSkipMpeg", current.gamefixSkipMpeg)
            if (current.gamefixEETiming != base.gamefixEETiming) j.put("gamefixEETiming", current.gamefixEETiming)
            if (current.gamefixInstantDma != base.gamefixInstantDma) j.put("gamefixInstantDma", current.gamefixInstantDma)
            if (current.gamefixBlitInternalFps != base.gamefixBlitInternalFps) j.put("gamefixBlitInternalFps", current.gamefixBlitInternalFps)
            if (current.gamefixFpuMul        != base.gamefixFpuMul)        j.put("gamefixFpuMul", current.gamefixFpuMul)
            if (current.gamefixOphFlag       != base.gamefixOphFlag)       j.put("gamefixOphFlag", current.gamefixOphFlag)
            if (current.gamefixGifFifo       != base.gamefixGifFifo)       j.put("gamefixGifFifo", current.gamefixGifFifo)
            if (current.gamefixDmaBusy       != base.gamefixDmaBusy)       j.put("gamefixDmaBusy", current.gamefixDmaBusy)
            if (current.gamefixVif1Stall     != base.gamefixVif1Stall)     j.put("gamefixVif1Stall", current.gamefixVif1Stall)
            if (current.gamefixIbit          != base.gamefixIbit)          j.put("gamefixIbit", current.gamefixIbit)
            if (current.gamefixFullVu0Sync   != base.gamefixFullVu0Sync)   j.put("gamefixFullVu0Sync", current.gamefixFullVu0Sync)
            if (current.gamefixVuAddSub      != base.gamefixVuAddSub)      j.put("gamefixVuAddSub", current.gamefixVuAddSub)
            if (current.gamefixVuOverflow    != base.gamefixVuOverflow)    j.put("gamefixVuOverflow", current.gamefixVuOverflow)
            if (current.gamefixXgkick        != base.gamefixXgkick)        j.put("gamefixXgkick", current.gamefixXgkick)
            if (current.skipDuplicateFrames  != base.skipDuplicateFrames)  j.put("skipDuplicateFrames", current.skipDuplicateFrames)
            if (current.eeFpuRoundMode       != base.eeFpuRoundMode)       j.put("eeFpuRoundMode", current.eeFpuRoundMode)
            if (current.aspectRatio         != base.aspectRatio)         j.put("aspectRatio", current.aspectRatio)
            if (current.deinterlaceMode     != base.deinterlaceMode)     j.put("deinterlaceMode", current.deinterlaceMode)
            if (current.dev9EthEnable       != base.dev9EthEnable)       j.put("dev9EthEnable", current.dev9EthEnable)
            if (current.dev9EthApi          != base.dev9EthApi)          j.put("dev9EthApi", current.dev9EthApi)
            if (current.dev9EthDevice       != base.dev9EthDevice)       j.put("dev9EthDevice", current.dev9EthDevice)
            if (current.dev9EthLogDhcp      != base.dev9EthLogDhcp)      j.put("dev9EthLogDhcp", current.dev9EthLogDhcp)
            if (current.dev9EthLogDns       != base.dev9EthLogDns)       j.put("dev9EthLogDns", current.dev9EthLogDns)
            if (current.dev9InterceptDhcp   != base.dev9InterceptDhcp)   j.put("dev9InterceptDhcp", current.dev9InterceptDhcp)
            if (current.dev9Ps2Ip           != base.dev9Ps2Ip)           j.put("dev9Ps2Ip", current.dev9Ps2Ip)
            if (current.dev9Mask            != base.dev9Mask)            j.put("dev9Mask", current.dev9Mask)
            if (current.dev9Gateway         != base.dev9Gateway)         j.put("dev9Gateway", current.dev9Gateway)
            if (current.dev9Dns1            != base.dev9Dns1)            j.put("dev9Dns1", current.dev9Dns1)
            if (current.dev9Dns2            != base.dev9Dns2)            j.put("dev9Dns2", current.dev9Dns2)
            if (current.dev9AutoMask        != base.dev9AutoMask)        j.put("dev9AutoMask", current.dev9AutoMask)
            if (current.dev9AutoGateway     != base.dev9AutoGateway)     j.put("dev9AutoGateway", current.dev9AutoGateway)
            if (current.dev9ModeDns1        != base.dev9ModeDns1)        j.put("dev9ModeDns1", current.dev9ModeDns1)
            if (current.dev9ModeDns2        != base.dev9ModeDns2)        j.put("dev9ModeDns2", current.dev9ModeDns2)
            if (current.dev9HddEnable       != base.dev9HddEnable)       j.put("dev9HddEnable", current.dev9HddEnable)
            if (current.dev9HddFile         != base.dev9HddFile)         j.put("dev9HddFile", current.dev9HddFile)
            if (current.memoryCardSlot1Enabled != base.memoryCardSlot1Enabled) j.put("memoryCardSlot1Enabled", current.memoryCardSlot1Enabled)
            if (current.memoryCardSlot1Filename != base.memoryCardSlot1Filename) j.put("memoryCardSlot1Filename", current.memoryCardSlot1Filename)
            if (current.memoryCardSlot2Enabled != base.memoryCardSlot2Enabled) j.put("memoryCardSlot2Enabled", current.memoryCardSlot2Enabled)
            if (current.memoryCardSlot2Filename != base.memoryCardSlot2Filename) j.put("memoryCardSlot2Filename", current.memoryCardSlot2Filename)
            if (current.recEE               != base.recEE)               j.put("recEE", current.recEE)
            if (current.recIOP              != base.recIOP)              j.put("recIOP", current.recIOP)
            if (current.recVU0              != base.recVU0)              j.put("recVU0", current.recVU0)
            if (current.recVU1              != base.recVU1)              j.put("recVU1", current.recVU1)
            if (current.enableFastmem       != base.enableFastmem)       j.put("enableFastmem", current.enableFastmem)
            if (current.vu1InlineFmacStall  != base.vu1InlineFmacStall)  j.put("vu1InlineFmacStall", current.vu1InlineFmacStall)
            if (current.vu1CrossBlockPState != base.vu1CrossBlockPState) j.put("vu1CrossBlockPState", current.vu1CrossBlockPState)
            if (current.vu1InlineDrainTestPipes != base.vu1InlineDrainTestPipes) j.put("vu1InlineDrainTestPipes", current.vu1InlineDrainTestPipes)
            if (current.vu1FmacInstanceRouting != base.vu1FmacInstanceRouting) j.put("vu1FmacInstanceRouting", current.vu1FmacInstanceRouting)
            if (current.hwMipmap            != base.hwMipmap)            j.put("hwMipmap", current.hwMipmap)
            if (current.accurateBlendingUnit!= base.accurateBlendingUnit)j.put("accurateBlendingUnit", current.accurateBlendingUnit)
            if (current.textureFiltering    != base.textureFiltering)    j.put("textureFiltering", current.textureFiltering)
            if (current.texturePreloading   != base.texturePreloading)   j.put("texturePreloading", current.texturePreloading)
            if (current.hardwareDownloadMode!= base.hardwareDownloadMode)j.put("hardwareDownloadMode", current.hardwareDownloadMode)
            if (current.tvShader            != base.tvShader)            j.put("tvShader", current.tvShader)
            if (current.shadeBoost          != base.shadeBoost)          j.put("shadeBoost", current.shadeBoost)
            if (current.shadeBoostBrightness != base.shadeBoostBrightness) j.put("shadeBoostBrightness", current.shadeBoostBrightness)
            if (current.shadeBoostContrast  != base.shadeBoostContrast)  j.put("shadeBoostContrast", current.shadeBoostContrast)
            if (current.shadeBoostSaturation != base.shadeBoostSaturation) j.put("shadeBoostSaturation", current.shadeBoostSaturation)
            if (current.shadeBoostGamma     != base.shadeBoostGamma)     j.put("shadeBoostGamma", current.shadeBoostGamma)
            if (current.loadTextureReplacements != base.loadTextureReplacements) j.put("loadTextureReplacements", current.loadTextureReplacements)
            if (current.loadTextureReplacementsAsync != base.loadTextureReplacementsAsync) j.put("loadTextureReplacementsAsync", current.loadTextureReplacementsAsync)
            if (current.precacheTextureReplacements != base.precacheTextureReplacements) j.put("precacheTextureReplacements", current.precacheTextureReplacements)
            if (current.dumpReplaceableTextures != base.dumpReplaceableTextures) j.put("dumpReplaceableTextures", current.dumpReplaceableTextures)
            if (current.osdShowTextureReplacements != base.osdShowTextureReplacements) j.put("osdShowTextureReplacements", current.osdShowTextureReplacements)
            if (current.osdShowFps != base.osdShowFps) j.put("osdShowFps", current.osdShowFps)
            if (current.vsyncEnable != base.vsyncEnable) j.put("vsyncEnable", current.vsyncEnable)
            if (current.osdShowVps != base.osdShowVps) j.put("osdShowVps", current.osdShowVps)
            if (current.osdShowSpeed != base.osdShowSpeed) j.put("osdShowSpeed", current.osdShowSpeed)
            if (current.osdShowCpu != base.osdShowCpu) j.put("osdShowCpu", current.osdShowCpu)
            if (current.osdShowGpu != base.osdShowGpu) j.put("osdShowGpu", current.osdShowGpu)
            if (current.osdShowResolution != base.osdShowResolution) j.put("osdShowResolution", current.osdShowResolution)
            if (current.osdShowGsStats != base.osdShowGsStats) j.put("osdShowGsStats", current.osdShowGsStats)
            if (current.osdShowFrameTimes != base.osdShowFrameTimes) j.put("osdShowFrameTimes", current.osdShowFrameTimes)
            if (current.autoFlush           != base.autoFlush)           j.put("autoFlush", current.autoFlush)
            if (current.halfPixelOffset     != base.halfPixelOffset)     j.put("halfPixelOffset", current.halfPixelOffset)
            if (current.limit24BitDepth     != base.limit24BitDepth)     j.put("limit24BitDepth", current.limit24BitDepth)
            if (current.manualUserHacks     != base.manualUserHacks)     j.put("manualUserHacks", current.manualUserHacks)
            if (current.textureInsideRt     != base.textureInsideRt)     j.put("textureInsideRt", current.textureInsideRt)
            if (current.nativeScaling       != base.nativeScaling)       j.put("nativeScaling", current.nativeScaling)
            if (current.roundSprite         != base.roundSprite)         j.put("roundSprite", current.roundSprite)
            if (current.bilinearUpscale     != base.bilinearUpscale)     j.put("bilinearUpscale", current.bilinearUpscale)
            if (current.gpuTargetClut       != base.gpuTargetClut)       j.put("gpuTargetClut", current.gpuTargetClut)
            if (current.cpuSpriteRenderBw   != base.cpuSpriteRenderBw)   j.put("cpuSpriteRenderBw", current.cpuSpriteRenderBw)
            if (current.cpuSpriteRenderLevel != base.cpuSpriteRenderLevel) j.put("cpuSpriteRenderLevel", current.cpuSpriteRenderLevel)
            if (current.alignSprite         != base.alignSprite)         j.put("alignSprite", current.alignSprite)
            if (current.mergeSprite         != base.mergeSprite)         j.put("mergeSprite", current.mergeSprite)
            if (current.forceEvenSpritePosition != base.forceEvenSpritePosition) j.put("forceEvenSpritePosition", current.forceEvenSpritePosition)
            if (current.unscaledPaletteDraw != base.unscaledPaletteDraw) j.put("unscaledPaletteDraw", current.unscaledPaletteDraw)
            if (current.textureOffsetX      != base.textureOffsetX)      j.put("textureOffsetX", current.textureOffsetX)
            if (current.textureOffsetY      != base.textureOffsetY)      j.put("textureOffsetY", current.textureOffsetY)
            if (current.gpuPaletteConversion != base.gpuPaletteConversion) j.put("gpuPaletteConversion", current.gpuPaletteConversion)
            if (current.cpuFramebufferConversion != base.cpuFramebufferConversion) j.put("cpuFramebufferConversion", current.cpuFramebufferConversion)
            if (current.readTargetsWhenClosing != base.readTargetsWhenClosing) j.put("readTargetsWhenClosing", current.readTargetsWhenClosing)
            if (current.disableDepthEmulation != base.disableDepthEmulation) j.put("disableDepthEmulation", current.disableDepthEmulation)
            if (current.disablePartialInvalidation != base.disablePartialInvalidation) j.put("disablePartialInvalidation", current.disablePartialInvalidation)
            if (current.disableSafeFeatures != base.disableSafeFeatures) j.put("disableSafeFeatures", current.disableSafeFeatures)
            if (current.disableRenderFixes  != base.disableRenderFixes)  j.put("disableRenderFixes", current.disableRenderFixes)
            if (current.preloadFrameData    != base.preloadFrameData)    j.put("preloadFrameData", current.preloadFrameData)
            if (current.estimateTextureRegion != base.estimateTextureRegion) j.put("estimateTextureRegion", current.estimateTextureRegion)
            if (current.cpuClutRender       != base.cpuClutRender)       j.put("cpuClutRender", current.cpuClutRender)
            if (current.triFilter           != base.triFilter)           j.put("triFilter", current.triFilter)
            if (current.maxAnisotropy       != base.maxAnisotropy)       j.put("maxAnisotropy", current.maxAnisotropy)
            if (current.gpuProfile          != base.gpuProfile)          j.put("gpuProfile", current.gpuProfile)
            return j
        }

        fun merge(base: Settings, overrides: JSONObject): Settings = Settings(
            eeCycleRate = if (overrides.has("eeCycleRate")) overrides.getInt("eeCycleRate") else base.eeCycleRate,
            eeCycleSkip = if (overrides.has("eeCycleSkip")) overrides.getInt("eeCycleSkip") else base.eeCycleSkip,
            eeClampMode = if (overrides.has("eeClampMode")) overrides.getInt("eeClampMode") else base.eeClampMode,
            vuClampMode = if (overrides.has("vuClampMode")) overrides.getInt("vuClampMode") else base.vuClampMode,
            mtvu = if (overrides.has("mtvu")) overrides.getBoolean("mtvu") else base.mtvu,
            vu1Instant = if (overrides.has("vu1Instant")) overrides.getBoolean("vu1Instant") else base.vu1Instant,
            vuFlagHack = if (overrides.has("vuFlagHack")) overrides.getBoolean("vuFlagHack") else base.vuFlagHack,
            fastCDVD = if (overrides.has("fastCDVD")) overrides.getBoolean("fastCDVD") else base.fastCDVD,
            intcStat = if (overrides.has("intcStat")) overrides.getBoolean("intcStat") else base.intcStat,
            waitLoop = if (overrides.has("waitLoop")) overrides.getBoolean("waitLoop") else base.waitLoop,
            vuNeonFusions = if (overrides.has("vuNeonFusions")) overrides.getBoolean("vuNeonFusions") else base.vuNeonFusions,
            vuDeferredWrites = if (overrides.has("vuDeferredWrites")) overrides.getBoolean("vuDeferredWrites") else base.vuDeferredWrites,
            vuSkipStallSim = if (overrides.has("vuSkipStallSim")) overrides.getBoolean("vuSkipStallSim") else base.vuSkipStallSim,
            frameLimitEnable = if (overrides.has("frameLimitEnable")) overrides.getBoolean("frameLimitEnable") else base.frameLimitEnable,
            nominalSpeedPercent = if (overrides.has("nominalSpeedPercent")) overrides.getInt("nominalSpeedPercent") else base.nominalSpeedPercent,
            fpsLimit = if (overrides.has("fpsLimit")) overrides.getInt("fpsLimit") else base.fpsLimit,
            frameSkip = if (overrides.has("frameSkip")) overrides.getInt("frameSkip") else base.frameSkip,
            audioVolume = if (overrides.has("audioVolume")) overrides.getInt("audioVolume") else base.audioVolume,
            audioMuted = if (overrides.has("audioMuted")) overrides.getBoolean("audioMuted") else base.audioMuted,
            audioTimeStretch = if (overrides.has("audioTimeStretch")) overrides.getBoolean("audioTimeStretch") else base.audioTimeStretch,
            audioBufferMs = if (overrides.has("audioBufferMs")) overrides.getInt("audioBufferMs") else base.audioBufferMs,
            audioOutputLatencyMs = if (overrides.has("audioOutputLatencyMs")) overrides.getInt("audioOutputLatencyMs") else base.audioOutputLatencyMs,
            audioFastForwardVolume = if (overrides.has("audioFastForwardVolume")) overrides.getInt("audioFastForwardVolume") else base.audioFastForwardVolume,
            enablePatches = if (overrides.has("enablePatches")) overrides.getBoolean("enablePatches") else base.enablePatches,
            enableCheats = if (overrides.has("enableCheats")) overrides.getBoolean("enableCheats") else base.enableCheats,
            enableWideScreenPatches = if (overrides.has("enableWideScreenPatches")) overrides.getBoolean("enableWideScreenPatches") else base.enableWideScreenPatches,
            enableNoInterlacingPatches = if (overrides.has("enableNoInterlacingPatches")) overrides.getBoolean("enableNoInterlacingPatches") else base.enableNoInterlacingPatches,
            enableFastBoot = if (overrides.has("enableFastBoot")) overrides.getBoolean("enableFastBoot") else base.enableFastBoot,
            enableGameFixes = if (overrides.has("enableGameFixes")) overrides.getBoolean("enableGameFixes") else base.enableGameFixes,
            gamefixSoftwareRendererFmv = if (overrides.has("gamefixSoftwareRendererFmv")) overrides.getBoolean("gamefixSoftwareRendererFmv") else base.gamefixSoftwareRendererFmv,
            gamefixSkipMpeg = if (overrides.has("gamefixSkipMpeg")) overrides.getBoolean("gamefixSkipMpeg") else base.gamefixSkipMpeg,
            gamefixEETiming = if (overrides.has("gamefixEETiming")) overrides.getBoolean("gamefixEETiming") else base.gamefixEETiming,
            gamefixInstantDma = if (overrides.has("gamefixInstantDma")) overrides.getBoolean("gamefixInstantDma") else base.gamefixInstantDma,
            gamefixBlitInternalFps = if (overrides.has("gamefixBlitInternalFps")) overrides.getBoolean("gamefixBlitInternalFps") else base.gamefixBlitInternalFps,
            gamefixFpuMul = if (overrides.has("gamefixFpuMul")) overrides.getBoolean("gamefixFpuMul") else base.gamefixFpuMul,
            gamefixOphFlag = if (overrides.has("gamefixOphFlag")) overrides.getBoolean("gamefixOphFlag") else base.gamefixOphFlag,
            gamefixGifFifo = if (overrides.has("gamefixGifFifo")) overrides.getBoolean("gamefixGifFifo") else base.gamefixGifFifo,
            gamefixDmaBusy = if (overrides.has("gamefixDmaBusy")) overrides.getBoolean("gamefixDmaBusy") else base.gamefixDmaBusy,
            gamefixVif1Stall = if (overrides.has("gamefixVif1Stall")) overrides.getBoolean("gamefixVif1Stall") else base.gamefixVif1Stall,
            gamefixIbit = if (overrides.has("gamefixIbit")) overrides.getBoolean("gamefixIbit") else base.gamefixIbit,
            gamefixFullVu0Sync = if (overrides.has("gamefixFullVu0Sync")) overrides.getBoolean("gamefixFullVu0Sync") else base.gamefixFullVu0Sync,
            gamefixVuAddSub = if (overrides.has("gamefixVuAddSub")) overrides.getBoolean("gamefixVuAddSub") else base.gamefixVuAddSub,
            gamefixVuOverflow = if (overrides.has("gamefixVuOverflow")) overrides.getBoolean("gamefixVuOverflow") else base.gamefixVuOverflow,
            gamefixXgkick = if (overrides.has("gamefixXgkick")) overrides.getBoolean("gamefixXgkick") else base.gamefixXgkick,
            skipDuplicateFrames = if (overrides.has("skipDuplicateFrames")) overrides.getBoolean("skipDuplicateFrames") else base.skipDuplicateFrames,
            eeFpuRoundMode = if (overrides.has("eeFpuRoundMode")) overrides.getInt("eeFpuRoundMode") else base.eeFpuRoundMode,
            aspectRatio = if (overrides.has("aspectRatio")) overrides.getInt("aspectRatio") else base.aspectRatio,
            deinterlaceMode = if (overrides.has("deinterlaceMode")) overrides.getInt("deinterlaceMode") else base.deinterlaceMode,
            dev9EthEnable = if (overrides.has("dev9EthEnable")) overrides.getBoolean("dev9EthEnable") else base.dev9EthEnable,
            dev9EthApi = if (overrides.has("dev9EthApi")) overrides.getString("dev9EthApi").ifEmpty { base.dev9EthApi } else base.dev9EthApi,
            dev9EthDevice = if (overrides.has("dev9EthDevice")) overrides.getString("dev9EthDevice").ifEmpty { base.dev9EthDevice } else base.dev9EthDevice,
            dev9EthLogDhcp = if (overrides.has("dev9EthLogDhcp")) overrides.getBoolean("dev9EthLogDhcp") else base.dev9EthLogDhcp,
            dev9EthLogDns = if (overrides.has("dev9EthLogDns")) overrides.getBoolean("dev9EthLogDns") else base.dev9EthLogDns,
            dev9InterceptDhcp = if (overrides.has("dev9InterceptDhcp")) overrides.getBoolean("dev9InterceptDhcp") else base.dev9InterceptDhcp,
            dev9Ps2Ip = if (overrides.has("dev9Ps2Ip")) overrides.getString("dev9Ps2Ip").ifEmpty { base.dev9Ps2Ip } else base.dev9Ps2Ip,
            dev9Mask = if (overrides.has("dev9Mask")) overrides.getString("dev9Mask").ifEmpty { base.dev9Mask } else base.dev9Mask,
            dev9Gateway = if (overrides.has("dev9Gateway")) overrides.getString("dev9Gateway").ifEmpty { base.dev9Gateway } else base.dev9Gateway,
            dev9Dns1 = if (overrides.has("dev9Dns1")) overrides.getString("dev9Dns1").ifEmpty { base.dev9Dns1 } else base.dev9Dns1,
            dev9Dns2 = if (overrides.has("dev9Dns2")) overrides.getString("dev9Dns2").ifEmpty { base.dev9Dns2 } else base.dev9Dns2,
            dev9AutoMask = if (overrides.has("dev9AutoMask")) overrides.getBoolean("dev9AutoMask") else base.dev9AutoMask,
            dev9AutoGateway = if (overrides.has("dev9AutoGateway")) overrides.getBoolean("dev9AutoGateway") else base.dev9AutoGateway,
            dev9ModeDns1 = if (overrides.has("dev9ModeDns1")) overrides.getString("dev9ModeDns1").ifEmpty { base.dev9ModeDns1 } else base.dev9ModeDns1,
            dev9ModeDns2 = if (overrides.has("dev9ModeDns2")) overrides.getString("dev9ModeDns2").ifEmpty { base.dev9ModeDns2 } else base.dev9ModeDns2,
            dev9HddEnable = if (overrides.has("dev9HddEnable")) overrides.getBoolean("dev9HddEnable") else base.dev9HddEnable,
            dev9HddFile = if (overrides.has("dev9HddFile")) overrides.getString("dev9HddFile").ifEmpty { base.dev9HddFile } else base.dev9HddFile,
            memoryCardSlot1Enabled = if (overrides.has("memoryCardSlot1Enabled")) overrides.getBoolean("memoryCardSlot1Enabled") else base.memoryCardSlot1Enabled,
            memoryCardSlot1Filename = if (overrides.has("memoryCardSlot1Filename")) overrides.getString("memoryCardSlot1Filename").ifEmpty { base.memoryCardSlot1Filename } else base.memoryCardSlot1Filename,
            memoryCardSlot2Enabled = if (overrides.has("memoryCardSlot2Enabled")) overrides.getBoolean("memoryCardSlot2Enabled") else base.memoryCardSlot2Enabled,
            memoryCardSlot2Filename = if (overrides.has("memoryCardSlot2Filename")) overrides.getString("memoryCardSlot2Filename").ifEmpty { base.memoryCardSlot2Filename } else base.memoryCardSlot2Filename,
            recEE = if (overrides.has("recEE")) overrides.getBoolean("recEE") else base.recEE,
            recIOP = if (overrides.has("recIOP")) overrides.getBoolean("recIOP") else base.recIOP,
            recVU0 = if (overrides.has("recVU0")) overrides.getBoolean("recVU0") else base.recVU0,
            recVU1 = if (overrides.has("recVU1")) overrides.getBoolean("recVU1") else base.recVU1,
            enableFastmem = if (overrides.has("enableFastmem")) overrides.getBoolean("enableFastmem") else base.enableFastmem,
            useMacEE = true,
            useMacIOP = true,
            useMacVU0 = true,
            useMacVU1 = true,
            vu1InlineFmacStall = if (overrides.has("vu1InlineFmacStall")) overrides.getBoolean("vu1InlineFmacStall") else base.vu1InlineFmacStall,
            vu1CrossBlockPState = if (overrides.has("vu1CrossBlockPState")) overrides.getBoolean("vu1CrossBlockPState") else base.vu1CrossBlockPState,
            vu1InlineDrainTestPipes = if (overrides.has("vu1InlineDrainTestPipes")) overrides.getBoolean("vu1InlineDrainTestPipes") else base.vu1InlineDrainTestPipes,
            vu1FmacInstanceRouting = if (overrides.has("vu1FmacInstanceRouting")) overrides.getBoolean("vu1FmacInstanceRouting") else base.vu1FmacInstanceRouting,
            hwMipmap = if (overrides.has("hwMipmap")) overrides.getBoolean("hwMipmap") else base.hwMipmap,
            accurateBlendingUnit = if (overrides.has("accurateBlendingUnit")) overrides.getInt("accurateBlendingUnit") else base.accurateBlendingUnit,
            textureFiltering = if (overrides.has("textureFiltering")) overrides.getInt("textureFiltering") else base.textureFiltering,
            texturePreloading = if (overrides.has("texturePreloading")) overrides.getInt("texturePreloading") else base.texturePreloading,
            hardwareDownloadMode = if (overrides.has("hardwareDownloadMode")) overrides.getInt("hardwareDownloadMode") else base.hardwareDownloadMode,
            tvShader = if (overrides.has("tvShader")) overrides.getInt("tvShader") else base.tvShader,
            shadeBoost = if (overrides.has("shadeBoost")) overrides.getBoolean("shadeBoost") else base.shadeBoost,
            shadeBoostBrightness = if (overrides.has("shadeBoostBrightness")) overrides.getInt("shadeBoostBrightness") else base.shadeBoostBrightness,
            shadeBoostContrast = if (overrides.has("shadeBoostContrast")) overrides.getInt("shadeBoostContrast") else base.shadeBoostContrast,
            shadeBoostSaturation = if (overrides.has("shadeBoostSaturation")) overrides.getInt("shadeBoostSaturation") else base.shadeBoostSaturation,
            shadeBoostGamma = if (overrides.has("shadeBoostGamma")) overrides.getInt("shadeBoostGamma") else base.shadeBoostGamma,
            loadTextureReplacements = if (overrides.has("loadTextureReplacements")) overrides.getBoolean("loadTextureReplacements") else base.loadTextureReplacements,
            loadTextureReplacementsAsync = if (overrides.has("loadTextureReplacementsAsync")) overrides.getBoolean("loadTextureReplacementsAsync") else base.loadTextureReplacementsAsync,
            precacheTextureReplacements = if (overrides.has("precacheTextureReplacements")) overrides.getBoolean("precacheTextureReplacements") else base.precacheTextureReplacements,
            dumpReplaceableTextures = if (overrides.has("dumpReplaceableTextures")) overrides.getBoolean("dumpReplaceableTextures") else base.dumpReplaceableTextures,
            osdShowTextureReplacements = if (overrides.has("osdShowTextureReplacements")) overrides.getBoolean("osdShowTextureReplacements") else base.osdShowTextureReplacements,
            osdShowFps = if (overrides.has("osdShowFps")) overrides.getBoolean("osdShowFps") else base.osdShowFps,
            vsyncEnable = if (overrides.has("vsyncEnable")) overrides.getBoolean("vsyncEnable") else base.vsyncEnable,
            osdShowVps = if (overrides.has("osdShowVps")) overrides.getBoolean("osdShowVps") else base.osdShowVps,
            osdShowSpeed = if (overrides.has("osdShowSpeed")) overrides.getBoolean("osdShowSpeed") else base.osdShowSpeed,
            osdShowCpu = if (overrides.has("osdShowCpu")) overrides.getBoolean("osdShowCpu") else base.osdShowCpu,
            osdShowGpu = if (overrides.has("osdShowGpu")) overrides.getBoolean("osdShowGpu") else base.osdShowGpu,
            osdShowResolution = if (overrides.has("osdShowResolution")) overrides.getBoolean("osdShowResolution") else base.osdShowResolution,
            osdShowGsStats = if (overrides.has("osdShowGsStats")) overrides.getBoolean("osdShowGsStats") else base.osdShowGsStats,
            osdShowFrameTimes = if (overrides.has("osdShowFrameTimes")) overrides.getBoolean("osdShowFrameTimes") else base.osdShowFrameTimes,
            autoFlush = if (overrides.has("autoFlush")) overrides.getInt("autoFlush") else base.autoFlush,
            halfPixelOffset = if (overrides.has("halfPixelOffset")) overrides.getInt("halfPixelOffset") else base.halfPixelOffset,
            limit24BitDepth = if (overrides.has("limit24BitDepth")) overrides.getInt("limit24BitDepth") else base.limit24BitDepth,
            manualUserHacks = if (overrides.has("manualUserHacks")) overrides.getBoolean("manualUserHacks") else base.manualUserHacks,
            textureInsideRt = if (overrides.has("textureInsideRt")) overrides.getInt("textureInsideRt") else base.textureInsideRt,
            nativeScaling = if (overrides.has("nativeScaling")) overrides.getInt("nativeScaling") else base.nativeScaling,
            roundSprite = if (overrides.has("roundSprite")) overrides.getInt("roundSprite") else base.roundSprite,
            bilinearUpscale = if (overrides.has("bilinearUpscale")) overrides.getInt("bilinearUpscale") else base.bilinearUpscale,
            gpuTargetClut = if (overrides.has("gpuTargetClut")) overrides.getInt("gpuTargetClut") else base.gpuTargetClut,
            cpuSpriteRenderBw = if (overrides.has("cpuSpriteRenderBw")) overrides.getInt("cpuSpriteRenderBw") else base.cpuSpriteRenderBw,
            cpuSpriteRenderLevel = if (overrides.has("cpuSpriteRenderLevel")) overrides.getInt("cpuSpriteRenderLevel") else base.cpuSpriteRenderLevel,
            alignSprite = if (overrides.has("alignSprite")) overrides.getBoolean("alignSprite") else base.alignSprite,
            mergeSprite = if (overrides.has("mergeSprite")) overrides.getBoolean("mergeSprite") else base.mergeSprite,
            forceEvenSpritePosition = if (overrides.has("forceEvenSpritePosition")) overrides.getBoolean("forceEvenSpritePosition") else base.forceEvenSpritePosition,
            unscaledPaletteDraw = if (overrides.has("unscaledPaletteDraw")) overrides.getBoolean("unscaledPaletteDraw") else base.unscaledPaletteDraw,
            textureOffsetX = if (overrides.has("textureOffsetX")) overrides.getInt("textureOffsetX") else base.textureOffsetX,
            textureOffsetY = if (overrides.has("textureOffsetY")) overrides.getInt("textureOffsetY") else base.textureOffsetY,
            gpuPaletteConversion = if (overrides.has("gpuPaletteConversion")) overrides.getBoolean("gpuPaletteConversion") else base.gpuPaletteConversion,
            cpuFramebufferConversion = if (overrides.has("cpuFramebufferConversion")) overrides.getBoolean("cpuFramebufferConversion") else base.cpuFramebufferConversion,
            readTargetsWhenClosing = if (overrides.has("readTargetsWhenClosing")) overrides.getBoolean("readTargetsWhenClosing") else base.readTargetsWhenClosing,
            disableDepthEmulation = if (overrides.has("disableDepthEmulation")) overrides.getBoolean("disableDepthEmulation") else base.disableDepthEmulation,
            disablePartialInvalidation = if (overrides.has("disablePartialInvalidation")) overrides.getBoolean("disablePartialInvalidation") else base.disablePartialInvalidation,
            disableSafeFeatures = if (overrides.has("disableSafeFeatures")) overrides.getBoolean("disableSafeFeatures") else base.disableSafeFeatures,
            disableRenderFixes = if (overrides.has("disableRenderFixes")) overrides.getBoolean("disableRenderFixes") else base.disableRenderFixes,
            preloadFrameData = if (overrides.has("preloadFrameData")) overrides.getBoolean("preloadFrameData") else base.preloadFrameData,
            estimateTextureRegion = if (overrides.has("estimateTextureRegion")) overrides.getBoolean("estimateTextureRegion") else base.estimateTextureRegion,
            cpuClutRender = if (overrides.has("cpuClutRender")) overrides.getInt("cpuClutRender") else base.cpuClutRender,
            triFilter = if (overrides.has("triFilter")) overrides.getInt("triFilter") else base.triFilter,
            maxAnisotropy = if (overrides.has("maxAnisotropy")) overrides.getInt("maxAnisotropy") else base.maxAnisotropy,
            gpuProfile = if (overrides.has("gpuProfile")) overrides.getInt("gpuProfile") else base.gpuProfile,
        )
    }
}
