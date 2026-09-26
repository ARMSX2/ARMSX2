package com.armsx2.config

import com.armsx2.ShaderParams
import com.armsx2.config.Settings.Companion.emitSink
import com.armsx2.config.Settings.Companion.merge
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONArray
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
 * Settings holds one nested data class per section (CpuSettings, GraphicsSettings and so on),
 * because a Kotlin data class stops working on Android at around 245 fields; SettingsSizeTest
 * says why and fails before any class gets there. The JSON keys stay flat ("renderer", not
 * "graphics.renderer"), so the nesting is invisible to stored settings and per-game overrides.
 *
 * Adding a new setting:
 *   1. Add a field, with an upstream-matching default, to its section's class,
 *   2. Add a setSetting line in applyTo,
 *   3. Add the JSON mapping in toJson + fromJson + diff + merge,
 *   4. Surface a widget in the appropriate Settings tab.
 */
/** One DEV9 internal-DNS host override: [url] resolves to [ip] when DNS mode = Internal.
 *  Used for private/fan servers (e.g. obsrv for RE Outbreak) that redirect specific hostnames. */
data class Dev9HostMapping(
    val url: String = "",
    val ip: String = "0.0.0.0",
    val enabled: Boolean = true,
)

/** RetroAchievements' options, the [Achievements] section the RetroAchievements screen and the
 *  in-game menu's 🏆 tab edit. Standard settings, so what is chosen for a game applies to that
 *  game and outranks the global value; they used to be written straight into the global native
 *  config from wherever they were changed. Defaults are Pcsx2Config::AchievementsOptions'.
 *
 *  Nested one level further, in [EmuCoreSettings]. The JSON keys keep their "achievements"
 *  prefix ("achievementsHardcore" and so on), as they had before these moved here. */
data class AchievementsSettings(
    /** Achievements/Enabled. Can differ per game: off globally and on for the games you want it
     *  in, or the other way round. The core starts and stops RetroAchievements itself when a
     *  game's value differs (Achievements::UpdateSettings). On by default, matching the native
     *  first-run seed and the login, which both switch it on. */
    val enabled: Boolean = true,
    /** Achievements/ChallengeMode. Only engages from a clean boot (Achievements::UpdateSettings
     *  defers it), which the hardcore switch handles. */
    val hardcore: Boolean = false,
    val notifications: Boolean = true,
    val leaderboardNotifications: Boolean = true,
    val overlays: Boolean = true,
    val lbOverlays: Boolean = true,
    val soundEffects: Boolean = true,
    val encoreMode: Boolean = false,
    val spectatorMode: Boolean = false,
    val unofficialTestMode: Boolean = false,
    val notificationsDuration: Int = 5,
    val leaderboardsDuration: Int = 10,
    /** The positions are the core's enum values: OsdOverlayPos (TopLeft = 1) for notifications,
     *  AchievementOverlayPosition (TopLeft = 0) for the overlay. */
    val notificationPosition: Int = 1,
    val overlayPosition: Int = 8,
    /** Achievements/NotificationScale — size of the achievement popups and in-game indicators, as a
     *  percentage of the stock layout (50..250). The stock size was hard to read on a handheld. */
    val notificationScale: Int = 100,
)

/** EE/VU clamping, speed hacks, recompiler enables and the arm64 JIT options
 *  (EmuCore/Speedhacks, EmuCore/CPU). Part of [Settings]. */
data class CpuSettings(
    // ---- EmuCore/Speedhacks ----
    /** EmuCore/Speedhacks/EECycleRate — −3..+3 (50%..300%). 0 = nominal. */
    val eeCycleRate: Int = 0,
    /** EmuCore/Speedhacks/EECycleSkip — 0..3. 0 = no skip. */
    val eeCycleSkip: Int = 0,
    /** EE/FPU clamp mode — 0 None / 1 Normal / 2 Extra / 3 Full / 4 Exact
     *  (PCSX2 default Normal). Unpacks to EmuCore/CPU/Recompiler
     *  fpuOverflow/fpuExtraOverflow/fpuFullMode/fpuExactMode.
     *  4 is Full plus the rest of the EE multiplier's one-ULP deficit and a
     *  divide/sqrt/rsqrt that runs the unit's own recurrence out of line, so it
     *  costs a call per divide. A title's own GameDB eeClampMode entry applies
     *  only while game fixes are on and the mode was not set for that game —
     *  a per-game setting outranks the database, and a GameDB entry below 4
     *  clears the exact bit. Keep any bound in the pickers in sync with this
     *  list. */
    val eeClampMode: Int = 1,
    /** VU clamp mode — 0 None / 1 Normal / 2 Extra / 3 Extra+Sign / 4 Exact
     *  (PCSX2 default Normal). Unpacks to vu0 Overflow/ExtraOverflow/
     *  SignOverflow/ExactMode, and to the vu1 four unless [vu1ClampMode]
     *  overrides them. 4 is Extra+Sign plus the VU's own arithmetic and
     *  status flags: the adder's guard mask, the divide unit's recurrence and
     *  the EFU's series, the multiplier's one-ULP deficit, and the FMAC's
     *  saturation ceiling with its MAC U and MAC O. A title's own GameDB
     *  vu0/vu1/vuClampMode entry applies only while game fixes are on and the
     *  mode was not set for that game — a per-game setting outranks the
     *  database, and a GameDB entry below 4 clears the exact bit. Keep any
     *  bound in the pickers in sync with this list. */
    val vuClampMode: Int = 1,
    /** VU1 clamp override — -1 follows [vuClampMode], else 0..4 on the same
     *  ladder but for the vu1 keys alone. Keep any bound in the pickers in
     *  sync with this list. */
    val vu1ClampMode: Int = -1,
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
)

/** Frame limiter and speed target (EmuCore/GS frame limiter, Framerate). Part of [Settings]. */
data class FrameLimitSettings(

    // ---- EmuCore/GS — frame limiter ----
    /** EmuCore/GS/FrameLimitEnable. */
    val frameLimitEnable: Boolean = true,
    /** Framerate/NominalScalar expressed as a percent of native speed
     *  (100 = full speed ≈ 60fps NTSC / 50fps PAL). Applies when the Frame
     *  Limiter is on: lower values cap the FPS (50 ≈ 30fps), higher values
     *  fast-forward. Stored as percent; written to emucore as the 0.05..10.0
     *  float scalar. */
    val nominalSpeedPercent: Int = 100,
    /** Max presented-FPS cap, independent of [nominalSpeedPercent] and the Speed
     *  Limit %. 0 = off. When > 0 the native side caps the DISPLAY frame rate by
     *  dropping presents on the GS thread while emulation keeps running full
     *  speed — it does NOT slow the game. Adaptive: a game already at/below the
     *  target is unaffected (no over-skip). */
    val fpsLimit: Int = 0,
    /** Deprecated Android-only frame skip. Kept for JSON compatibility only. */
    val frameSkip: Int = 0,
)

/** Audio output (SPU2). Part of [Settings]. */
data class AudioSettings(

    // ---- Audio (SPU2/Output) ----
    /** SPU2/Output/StandardVolume — output volume %, 0..200 (100 = full). */
    val audioVolume: Int = 100,
    /** SPU2/Output/OutputMuted — mute audio output. */
    val audioMuted: Boolean = false,
    /** SPU2/Output/SwapChannels — swap final stereo output L<->R (flipped-speaker
     *  devices forced into reverse-landscape, e.g. the Clamp gamepad). */
    val audioSwapChannels: Boolean = false,
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
    /** SPU2/NeonReverbSIMD — opt-in NEON reverb FIR on ARM64. Frees CPU on
     *  CPU-bound devices; default off uses the scalar reference (unchanged
     *  audio). Applied on the next game boot/reset. */
    val spu2NeonReverb: Boolean = false,
    /** SPU2/Output/AndroidOpenSLES — opt-in legacy OpenSL ES audio path (Oboe)
     *  instead of AAudio. Slightly higher latency, but Android doesn't reclaim
     *  the idle stream, so pause/resume (and fast-forward toggling through the
     *  menu) never triggers the ~1s stream rebuild. Applies live (stream
     *  reconfigures). Default off = AAudio low-latency. */
    val audioOpenSLES: Boolean = false,
    /** SPU2/Output/LightweightMode — low-end audio lever: skip the SPU2 reverb
     *  pipeline (all echo/spatial reverb) in the mixer. Frees CPU on devices that
     *  can't keep up even with NEON reverb; default off = full reverb. Applies
     *  live (read per-sample in MixCore). */
    val spu2LightweightMix: Boolean = false,
)

/** Patches, cheats, boot options, RetroAchievements and the other EmuCore toggles. Part of [Settings]. */
data class EmuCoreSettings(

    // ---- EmuCore — patches / cheats ----
    /** EmuCore/EnablePatches — game-compatibility patches (default on). */
    val enablePatches: Boolean = true,
    /** EmuCore/EnableCheats — PNACH cheats. */
    val enableCheats: Boolean = false,
    /** EmuCore/EnableWideScreenPatches — 16:9 widescreen patches. */
    val enableWideScreenPatches: Boolean = false,
    /** EmuCore/EnableNoInterlacingPatches — no-interlacing patches. */
    val enableNoInterlacingPatches: Boolean = false,
    /** EmuCore/EnableFastBoot — skip BIOS splash and boot straight to the game.
     *
     *  Default ON: "how do I skip the boot animation" is one of the most-asked questions in the
     *  Discord, and desktop PCSX2 fast-boots by default too. Only fresh installs are affected —
     *  the saved JSON always carries this key, so anyone who already has a value keeps it rather
     *  than having their boot behaviour changed under them by an update. */
    val enableFastBoot: Boolean = true,
    /** EmuCore/HostFs — host: filesystem access in the VM, for ELF/homebrew and mods
     *  (e.g. modded Persona 3 FES). Per-game capable; applies on the next game boot. */
    val hostFs: Boolean = false,
    /** RetroAchievements' options, the [Achievements] INI section. */
    val achievements: AchievementsSettings = AchievementsSettings(),
    /** EmuCore/EnablePINE — the IPC server external tools drive the emulator through
     *  (read/write guest memory, savestates, GS dumps). On Android it listens on loopback
     *  TCP, so it is reachable from a workstation only after `adb forward`; nothing outside
     *  the device can see it. Off by default: it is a debugging tool, and a listening socket
     *  a player did not ask for should not exist. */
    val pineEnabled: Boolean = false,
    /** EmuCore/PINESlot — the port [pineEnabled] listens on. Deliberately has no UI row: the
     *  only reason to move it is running two emulators at once, which does not happen on a
     *  handheld, and a free-entry port field is a support burden for a knob nobody turns.
     *  Kept in the model anyway so the toggle's description can state the real port rather
     *  than assuming the default. Editable in the INI for the rare case that needs it. */
    val pineSlot: Int = 28011,
    /** EmuCore/EnableGameFixes — master switch that lets the GameDB apply each game's
     *  curated compatibility gamefixes (e.g. VuAddSubHack, SkipMPEGHack). Defaults TRUE
     *  to match upstream PCSX2 (Pcsx2Config.cpp EnableGameFixes = true) and trak's Mac:
     *  Android was the outlier defaulting it false, which silently skipped every GameDB
     *  CPU gamefix — that's what broke Valkyrie Profile 2 (needs VuAddSubHack; without it
     *  the first VU0 program diverges and the EE derails to PC=0) and made Skip MPEG inert.
     *  GameDB gamefixes are per-game curated, so on-by-default only helps compatibility. */
    val enableGameFixes: Boolean = true,
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
    /** EmuCore/Gamefixes/GoemonTlbHack — preload TLB for Goemon games. Restart to apply. */
    val gamefixGoemonTlb: Boolean = false,
    /** EmuCore/Gamefixes/VUSyncHack — run microVU behind the EE (M-bit games). Restart to apply. */
    val gamefixVuSync: Boolean = false,
    /** EmuCore/GS/SkipDuplicateFrames — skip presenting unchanged frames. PCSX2 default on. */
    val skipDuplicateFrames: Boolean = true,
    /** EmuCore/CPU/FPU.Roundmode — EE FPU rounding: 0 Nearest / 1 Negative / 2 Positive
     *  / 3 Chop. PS2 EE FPU default is Chop (toward zero). */
    val eeFpuRoundMode: Int = 3,
    /** EmuCore/CPU/VU0.Roundmode — VU0 rounding: 0 Nearest / 1 Neg / 2 Pos / 3 Chop. Default Chop. */
    val vu0RoundMode: Int = 3,
    /** EmuCore/CPU/VU1.Roundmode — VU1 rounding: 0 Nearest / 1 Neg / 2 Pos / 3 Chop. Default Chop. */
    val vu1RoundMode: Int = 3,
)

/** Display, PCRTC and presentation options (EmuCore/GS). Part of [Settings]. */
data class DisplaySettings(

    // ---- EmuCore/GS — display / PCRTC fixes ----
    /** EmuCore/GS/pcrtc_offsets — apply PCRTC screen offsets. PCSX2 default off. */
    val screenOffsets: Boolean = false,
    /** EmuCore/GS/pcrtc_overscan — show overscan area. PCSX2 default off. */
    val showOverscan: Boolean = false,
    /** EmuCore/GS/pcrtc_antiblur — anti-blur. PCSX2 default ON. */
    val antiBlur: Boolean = true,
    /** EmuCore/GS/disable_interlace_offset — disable interlace offset. Default off. */
    val disableInterlaceOffset: Boolean = false,
    /** EmuCore/GS/SyncToHostRefreshRate — pace emulation to the host refresh. Default off. */
    val syncToHostRefresh: Boolean = false,
    /** EmuCore/GS/DisableFramebufferFetch — disable the framebuffer-fetch path. Default off. */
    val disableFramebufferFetch: Boolean = false,
    /** EmuCore/GS/HWROV — Rasterizer Order Views (accurate blending via fragment-shader
     *  interlock; Vulkan only). Default OFF on mobile: it's a perf loss on tilers and is
     *  inert on Turnip/Adreno (no VK_EXT_fragment_shader_interlock), so on-by-default just
     *  costs frames for no gain. Upstream PCSX2 defaults it true (desktop); we override to
     *  false for Android. Users can still enable it in Renderer for benchmarking. */
    val hwRov: Boolean = false,
    /** EmuCore/GS/HWAA1 — hardware PS2 AA1 edge anti-aliasing. Default off. Applies on game restart. */
    val hwAa1: Boolean = false,
    /** EmuCore/GS/HWAccurateAlphaTest — accurate alpha test for the HW renderer (pairs with ROV). Default off. */
    val hwAat: Boolean = false,
    /** EmuCore/GS/CoalesceRenderPasses — group consecutive draws to the same target into a
     * single render pass. Aimed squarely at tiling GPUs (every Android GPU), where each pass
     * boundary costs a full tile load and store; rendering output is unchanged. Default off,
     * matching upstream, because it is new. bmd only wired this into the desktop UI, so
     * without this it would be unreachable on the platform it was written for. */
    val coalesceRenderPasses: Boolean = false,
    /** EmuCore/GS/ForceMaliFramebufferFetch — re-enable the Vulkan framebuffer-fetch
     * (ROAA) path on MediaTek Mali / Mali-G57, where it is force-disabled because those
     * drivers return zero/stale destination colour through ROAA (black or missing
     * textures). Mali exposes no hardware dual-source blend, so with fetch off the HW
     * renderer SW-blends via a per-primitive texture barrier — very slow in blend-heavy
     * games (issue #339: Shadow of the Colossus on Dimensity 8350 + Mali-G615). This lets
     * such a user test whether their driver is actually affected. Default OFF. Inert on
     * other GPUs/renderers. Applies on game restart. */
    val forceMaliFbFetch: Boolean = false,
    /** EmuCore/GS/AndroidUseAngleOpenGL — run the OpenGL renderer through ANGLE's
     *  GLES-on-Vulkan translation (bundled libEGL_angle.so / libGLESv2_angle.so).
     *  Useful on devices with a broken native GLES driver (e.g. some MediaTek Mali).
     *  Only takes effect when the renderer is OpenGL; MainActivityRuntime.applyAngleEnv
     *  turns it into the ARMSX2_ANGLE_EGL_LIBRARY env var that GLContextEGL reads.
     *  Applies on game restart. Default off. */
    val useAngleOpenGL: Boolean = false,
    /** EmuCore/GS/OverrideTextureBarriers — -1 Auto / 0 Off / 1 On. */
    val overrideTextureBarriers: Int = -1,
    /** EmuCore/GS/GSBackThreadMode — GV7 GS front/back thread split.
     * 0 Off (single-threaded), 1 Inline, 2 Lockstep, 3 Pipelined (fastest).
     * Defaults to Off (opt-in); a per-game override can raise it. Restart-required. */
    val gsBackThreadMode: Int = 0,
    /** EmuCore/GS/DisableVertexShaderExpand — force CPU vertex expansion. Renderer-init; restart to apply. */
    val disableVertexShaderExpand: Boolean = false,
    /** EmuCore/GS/UseBlitSwapChain — blit present model instead of flip. Renderer-init; restart to apply. */
    val useBlitSwapChain: Boolean = false,
    /** EmuCore/GS/DisableShaderCache — don't cache compiled shaders to disk. Renderer-init; restart to apply. */
    val disableShaderCache: Boolean = false,
    /** EmuCore/GS/HWAccurateAlphaTest — accurate hardware alpha test. PCSX2 default off. */
    val hwAccurateAlphaTest: Boolean = false,
    /** EmuCore/GS/VsyncEnable — sync presentation to the display refresh (less
     *  tearing/smoother, slightly higher latency). Applies on game restart. */
    val vsyncEnable: Boolean = false,
)

/** Hardware and software renderer fixes, including the upscaling fixes (EmuCore/GS). Part of [Settings]. */
data class HwFixesSettings(

    // ---- EmuCore/GS — hardware / software renderer fixes ----
    /** EmuCore/GS/UserHacks_SkipDraw_Start — first draw to skip. 0 = off. */
    val skipDrawStart: Int = 0,
    /** EmuCore/GS/UserHacks_SkipDraw_End — last draw to skip. 0 = off. */
    val skipDrawEnd: Int = 0,
    /** EmuCore/GS/HWSpinGPUForReadbacks — busy-wait the GPU on readbacks. Default off. */
    val spinGpuReadbacks: Boolean = false,
    /** EmuCore/GS/HWSpinCPUForReadbacks — busy-wait the CPU on readbacks. Default off. */
    val spinCpuReadbacks: Boolean = false,
    /** EmuCore/GS/IntegerScaling — integer pixel scaling for the presented image. Default off. */
    val integerScaling: Boolean = false,
    /** EmuCore/GS/CropLeft|Top|Right|Bottom — overscan crop in native PS2 pixels, trimmed
     *  from the presented image before aspect/integer scaling. Many PS2 titles render
     *  garbage or a black band in the overscan area that a TV would have hidden; the core
     *  has always supported this (GSRenderer.cpp) but Android never exposed it (issue #293). */
    val cropLeft: Int = 0,
    val cropTop: Int = 0,
    val cropRight: Int = 0,
    val cropBottom: Int = 0,
    /** Display zoom, 100-150% (#383). An AetherSX2-style single "zoom" slider: rather than the
     *  four fiddly per-edge crops (which distort when set unevenly), this trims all four edges by
     *  the SAME fraction, so the image scales up into the frame without changing aspect. App-side
     *  only (no native key) — it's converted to symmetric CropLeft/Top/Right/Bottom in writeIni,
     *  overriding the manual crops while > 100. */
    val displayZoom: Int = 100,
    /** EmuCore/GS/dithering_ps2 — 0 Off / 1 Scaled / 2 Unscaled / 3 Force 32bit. PCSX2 default Unscaled. */
    val dithering: Int = 2,
    /** EmuCore/GS/VsyncQueueSize — frames the GS thread may queue (0-3). PCSX2 default 2. */
    val vsyncQueueSize: Int = 2,
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
    /** EmuCore/GS/UserHacks_DrawBuffering — buffer draws (UserHack). */
    val drawBuffering: Boolean = false,
    /** EmuCore/GS/UserHacks_CPUCLUTRender — CPU CLUT Render: 0 Off · 1 Normal · 2 Aggressive. */
    val cpuClutRender: Int = 0,
    /** EmuCore/GS/TriFilter — TriFiltering: -1 Auto · 0 Off · 1 PS2 · 2 Forced. */
    val triFilter: Int = -1,
    /** EmuCore/GS/MaxAnisotropy — 0 Off, else 2/4/8/16. */
    val maxAnisotropy: Int = 0,
    /** EmuCore/GS/AndroidGpuProfileOverride — 0 Auto · 1 Mali · 2 Adreno · 3 PowerVR · 4 Xclipse.
     *  Stringified to "auto"/"mali"/"adreno"/"powervr"/"xclipse" when written to emucore.
     *  Picked up in GSDeviceOGL::CheckFeatures at device init; requires
     *  a renderer restart to take effect. */
    val gpuProfile: Int = 0,
)

/** Output-surface scaling and the app-side display layout; per-game scoped. Part of [Settings]. */
data class OutputSettings(
    // Output-surface scaling. App-side (no EmuCore key) but PER-GAME scoped: a heavy
    // game can render its output smaller while the library and lighter games stay
    // sharp. Were global-only prefs until #-Duda reported that changing them in Game
    // scope also moved Global — there was no per-game copy to write.
    val hwScaler: Int = 0,                       // 0 = screen, else 448*n short side
    val screenResOverride: String = "auto",      // "auto" | "2560x1440" | "1920x1080" | "1280x720"
    /** EmuCore/GS/autoflush_sw — software-renderer auto-flush. PCSX2 default on. */
    val autoFlushSw: Boolean = true,
    /** EmuCore/GS/mipmap — software-renderer mipmapping. PCSX2 default on. */
    val mipmapSw: Boolean = true,
    /** EmuCore/GS/extrathreads — extra software-renderer threads (0-10). PCSX2 default 4. */
    val swThreads: Int = 4,
    /** EmuCore/GS/extrathreads_height — SW-renderer tile height per thread (0-8). PCSX2 default 4. Restart to apply. */
    val swThreadsHeight: Int = 4,

    /** EmuCore/GS/AspectRatio:
     *  0 Stretch · 1 Auto 4:3/3:2 · 2 4:3 · 3 16:9 · 4 10:7 · 5 21:9 · 6 20:9 · 7 19.5:9 · 8 Custom.
     *  Indices are persisted, so append new ratios — never insert. */
    val aspectRatio: Int = 1,
    /** EmuCore/GS/FMVAspectRatioSwitch — aspect ratio used ONLY while an FMV/MPEG is
     *  playing (restores [aspectRatio] when it ends). 0 Off (no override) · 1 Auto
     *  4:3/3:2 · 2 4:3 · 3 16:9 · 4 10:7 · 5 21:9 · 6 20:9 · 7 19.5:9 · 8 Custom. Default Off. */
    val fmvAspectRatio: Int = 0,
    /** EmuCore/GS/CustomAspectRatio — width/height used when [aspectRatio] is 8 (Custom).
     *  A ratio rather than separate W/H so any value is expressible; clamped 0.5..5.0 natively. */
    val customAspectRatio: Float = 16f / 9f,
    /** Host graphics API: "auto" / "opengl" / "vulkan" / "software". Applied via
     *  the renderer JNI helpers on (re)launch; per-game so each title can pick its
     *  own backend. Seeded from the legacy global "renderer" pref on first load. */
    val renderer: String = "auto",
    /** Internal resolution multiplier (0.25..5.0; 1.0 = native). Applied live via
     *  the GS upscale helper; per-game so each title keeps its own. Seeded from the
     *  legacy global "upscaleFloat" pref on first load. */
    val upscaleFloat: Float = 1.0f,
    /** Installed custom Vulkan GPU driver id to pin (e.g. a Turnip build). "" = system
     *  driver. Applied at (re)launch via CustomDriver.applyToNative in
     *  MainActivityRuntime.applyRendererPrefs; per-game so a title can pin the driver it
     *  needs. Seeded from the legacy global "customDriverId" pref on first load. */
    val customDriverId: String = "",
    /** Android activity screen orientation: 0 Use Device Setting · 1 Landscape · 2 Portrait
     *  · 3 Auto-Rotate. Applied via MainActivityRuntime.applyEmulationOrientation, resolved
     *  per-game at game boot (global in the library/menus). Seeded from the legacy global
     *  "ui.orientation" pref on first load. */
    val orientation: Int = 0,
    /** GitHub #375: in PORTRAIT, top-align the render (true, default) instead of vertical-
     *  centering (false), so the bottom is free for touch controls. Applied live via
     *  NativeApp.setPortraitRenderTop; only affects a portrait window. */
    val portraitRenderTop: Boolean = true,
    /** In LANDSCAPE, top-align the render instead of vertical-centering (default). Foldables and
     *  clamshell controllers (Backbone-style) open the screen downward, so a centred image sits
     *  too low. Applied live via NativeApp.setLandscapeRenderTop; only affects a landscape window. */
    val landscapeRenderTop: Boolean = false,
    /** Auto Progressive Scan: hold Triangle+Cross on port 1 through the boot sequence, which is
     *  the real-console combo a number of PS2 titles probe to offer 480p progressive output
     *  (Tekken 4, several Criterion games). Purely a synthetic pad hold — no core setting — so it
     *  only does anything on games that implement the prompt. Per-game because the same combo is
     *  a normal input elsewhere, and titles that ignore the prompt gain nothing from holding it. */
    val autoProgressiveScan: Boolean = false,
    /** Affinity Control Mode (default 7 = Performance Cores). 0 Disabled · 1 EE>VU>GS ·
     *  2 EE>GS>VU · 3 VU>EE>GS · 4 VU>GS>EE · 5 GS>EE>VU · 6 GS>VU>EE · 7 Performance Cores.
     *  Pushed to native via NativeApp.setAffinityMode before runVMThread and consumed by
     *  VMManager::SetEmuThreadAffinities, so it applies on the next boot. Per-game because the
     *  best placement is workload-dependent: GS-bound titles want the GS thread on the prime
     *  core, VU-bound ones want VU left free to float there.
     *
     *  Modes 1-6 hand out INDIVIDUAL cores and remain experimental — pinning VU to a mid-tier
     *  big core measured ~1.4x slower than letting it float to the prime. Mode 7 is a different
     *  thing: it confines the emu threads to the big/prime TIER and leaves EAS free to place
     *  them within it, and it self-disables (unpinned) on any device where that tier can't be
     *  read or is too narrow to hold them. That safety is why it can be the default. */
    val affinityMode: Int = 7,
    /** EmuCore/GS FramerateNTSC — the emulated PS2 vsync rate for NTSC games
     *  (PCSX2 default 59.94). Lowering it slows the game's target rate; raising it
     *  speeds it up. Mirrors NetherSX2's "Framerate For NTSC". */
    val framerateNtsc: Float = 59.94f,
    /** EmuCore/GS FrameratePAL — emulated PS2 vsync rate for PAL games (default 50.00). */
    val frameratePal: Float = 50.00f,
    /** EmuCore/GS/deinterlace_mode — GSInterlaceMode:
     *  0 Auto · 1 Off · 2/3 Weave · 4/5 Bob · 6/7 Blend · 8/9 Adaptive. */
    val deinterlaceMode: Int = 0,
)

/** DEV9: PS2 HDD and Ethernet, including Local Link. Part of [Settings]. */
data class NetworkSettings(

    // ---- DEV9 — PS2 HDD / Ethernet ----
    /** DEV9/Eth/EthEnable — PS2 network adapter. */
    val dev9EthEnable: Boolean = false,
    /** DEV9/Eth/EthApi — "Sockets" for internet play, "Local Link" for device-to-device LAN. */
    val dev9EthApi: String = "Sockets",
    // ---- Local Link (EthApi = "Local Link") -------------------------------------------------
    // Bridges the emulated PS2 Ethernet frames between devices over UDP on the local network, so
    // games with native System Link / LAN support see each other as if on one switch. Each device
    // runs its own VM — this is NOT netplay, and it does nothing for online-only or i.Link titles.
    /** DEV9/Eth/LocalLinkHost — true = this device relays for the session; false = it joins one. */
    val localLinkHost: Boolean = false,
    /** DEV9/Eth/LocalLinkAddress — the host's LAN IPv4, entered on joining devices only. */
    val localLinkAddress: String = "",
    /** DEV9/Eth/LocalLinkPort — UDP port; must match on every device (no negotiation). */
    val localLinkPort: Int = 19072,
    /** DEV9/Eth/LocalLinkPeerId — 1 for the host, 2+ for each guest. Must be unique per device;
     *  duplicate ids collide because the peer id is what derives the emulated MAC and IP. */
    val localLinkPeerId: Int = 1,
    /** DEV9/Eth/LocalLinkRoomCode — shared 4-12 char code that keys the packet authentication.
     *  Prevents crosstalk between sessions on the same Wi-Fi; it is NOT strong security. */
    val localLinkRoomCode: String = "",
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
    /** DEV9/Eth/Hosts — hostname->IP overrides consulted by the INTERNAL DNS server
     *  (DNS mode = Internal). For private/fan servers that redirect specific hostnames. */
    val dev9EthHosts: List<Dev9HostMapping> = emptyList(),
    /** DEV9/Hdd/HddEnable — virtual PS2 HDD. */
    val dev9HddEnable: Boolean = false,
    /** DEV9/Hdd/HddFile — path/name of the virtual HDD image. */
    val dev9HddFile: String = "DEV9hdd.raw",
)

/** Memory cards, the per-game BIOS and USB devices. Part of [Settings]. */
data class SystemSettings(

    // ---- MemoryCards ----
    val memoryCardSlot1Enabled: Boolean = true,
    val memoryCardSlot1Filename: String = "mcd001.ps2",
    // Per-game BIOS override (e.g. an EU disc vs a US disc wanting its region's BIOS).
    // Empty = use the global BIOS picked in the BIOS manager. Just the filename; the file
    // lives in the app-private BIOS dir like every installed BIOS. Applied at boot in
    // MainActivityRuntime.applyRendererPrefs (resolved per-game, with global fallback).
    val biosFilename: String = "",
    val memoryCardSlot2Enabled: Boolean = true,
    val memoryCardSlot2Filename: String = "mcd002.ps2",

    // ---- USB ----
    /** USB1/Type = hidkbd — attach an emulated USB HID keyboard on USB port 1.
     *  Needed by games that require a real USB keyboard (EverQuest Online
     *  Adventures, Konami-keyboard titles). A physical/Bluetooth keyboard's key
     *  events are forwarded to it (see MainActivityRuntime.dispatchKeyEvent → NativeApp.usbKeyboardKey).
     *  Default off. */
    val usbKeyboard: Boolean = false,
)

/** Renderer accuracy, quality, post-processing and texture replacement (EmuCore/GS). Part of [Settings]. */
data class GraphicsSettings(

    // ---- EmuCore/GS — renderer accuracy / quality ----
    /** EmuCore/GS/hw_mipmap. */
    val hwMipmap: Boolean = true,
    /** EmuCore/GS/accurate_blending_unit — AccBlendLevel:
     *  0 Min · 1 Basic · 2 Medium · 3 High · 4 Full · 5 Maximum. */
    val accurateBlendingUnit: Int = 1,
    /** EmuCore/GS/filter — BiFiltering:
     *  0 Nearest · 1 Forced (Bilinear) · 2 PS2 · 3 Forced_But_Sprite. */
    val textureFiltering: Int = 2,
    /** EmuCore/GS/linear_present_mode — GSPostBilinearMode:
     *  0 Off (nearest) · 1 Smooth · 2 Sharp. Display output (scan-out) bilinear filter. */
    val displayBilinear: Int = 1,
    /** EmuCore/GS/texture_preloading — TexturePreloadingLevel:
     *  0 Off · 1 Partial · 2 Full. */
    val texturePreloading: Int = 2,
    /** EmuCore/GS/HWDownloadMode — GSHardwareDownloadMode:
     *  0 Accurate · 1 Force Full · 2 No Readbacks · 3 Unsync · 4 Disabled · 5 Asynchronous.
     *  ★ 5 (Asynchronous) is EXPERIMENTAL: a non-blocking GPU→CPU readback pipeline, so the EE
     *  thread never stalls on the GS thread. Note the enum stops being ordered at 5 — never write
     *  `mode > n` comparisons against it. Keep the clamp in applyTo in sync with this list. */
    val hardwareDownloadMode: Int = 0,
    /** EmuCore/GS/TVShader — CRT / TV shader preset. */
    val tvShader: Int = 0,
    /** EmuCore/GS/ShadeBoost. */
    val shadeBoost: Boolean = false,
    val shadeBoostBrightness: Int = 50,
    val shadeBoostContrast: Int = 50,
    val shadeBoostSaturation: Int = 50,
    val shadeBoostGamma: Int = 50,
    /** EmuCore/GS/fxaa — FXAA post-process anti-aliasing. */
    val fxaa: Boolean = false,
    /** EmuCore/GS/ShaderChainEnabled + /ShaderChainPreset — RetroArch (.slangp) shader
     *  chain run at present via librashader, after ShadeBoost/FXAA. An empty preset means
     *  off regardless of the flag, and it's a no-op if this build has no librashader. */
    val shaderChainEnabled: Boolean = false,
    val shaderChainPreset: String = "",
    /** EmuCore/GS/LsfgEnabled + /LsfgMultiplier + /LsfgDllPath — LSFG frame generation,
     *  inserted into the Vulkan present path. Off unless the user both enables it AND
     *  supplies their own Lossless.dll: the interpolation shaders are read out of that
     *  file at runtime and nothing about them ships with ARMSX2. Vulkan + Adreno 7xx and
     *  newer only, and absent entirely from the Play build. [lsfgMultiplier] is frames
     *  DISPLAYED per frame rendered, so 2 means one interpolated frame between each pair. */
    val lsfgEnabled: Boolean = false,
    val lsfgMultiplier: Int = 2,
    val lsfgDllPath: String = "",
    /** EmuCore/GS/LsfgPerformance — LSFG 3.1p, a lighter shader family than 3.1. On by default:
     *  this runs on a phone GPU that is already busy presenting the game, and the cheaper
     *  pipeline is what makes frame generation pay for itself there. Falls back to 3.1 by itself
     *  when the user's Lossless.dll predates 3.1p. */
    val lsfgPerformance: Boolean = true,
    /** EmuCore/GS/LsfgFlowScale — optical-flow resolution, as a PERCENTAGE of the presented
     *  image (25..100). Lower is cheaper and blurrier. The native side inverts it: the library
     *  takes a divisor, so 25% becomes 4.0. See GSLsfg.cpp. */
    val lsfgFlowScale: Int = 100,
    /** EmuCore/GS/LsfgTargetRate — target OUTPUT rate in Hz for the adaptive pacer; 0 holds
     *  [lsfgMultiplier] fixed.
     *
     *  A fixed multiplier is the wrong shape for a game that oscillates between 60 and 30fps on
     *  a 60Hz panel: at x2 it presents 120 then 60, and every transition reads as judder. Given
     *  a target the pacer varies the generation count instead — two interpolated frames while
     *  the game runs at 30, one while it runs at 60 — so the presented rate stays put while the
     *  rendered rate moves underneath it.
     *
     *  Stored as a concrete Hz rather than an on/off flag because the native pacer needs a
     *  number; the UI writes the panel's refresh rate when the user turns it on. */
    val lsfgTargetRate: Int = 0,
    /** Tweaked shader parameters, as `preset path -> (parameter name -> value)`.
     *
     *  Sparse: a parameter the user hasn't touched is simply absent, and the author's own
     *  initial applies. That is what keeps this from bloating — a preset can declare ~900
     *  parameters, and storing all of them per game would dwarf the rest of the config.
     *
     *  Keyed by preset rather than holding one flat map because parameter names collide
     *  freely across packs ("gamma" means something different in every one of them), and
     *  because it lets a user flip between two tweaked presets without losing either set.
     *  No EmuCore key mirrors this — see applyTo. */
    val shaderChainParams: Map<String, Map<String, Float>> = emptyMap(),
    /** EmuCore/GS/CASMode — GSCASMode: 0 Off / 1 Sharpen Only / 2 Sharpen + Resize. */
    val casMode: Int = 0,
    /** EmuCore/GS/CASSharpness — sharpening strength 0..100 (%). */
    val casSharpness: Int = 50,
    /** EmuCore/GS/Upscaler — GSUpscaler: 0 Off / 1 MetalFX (Apple only) / 2 FSR1 / 3 SGSR /
     *  4 SGSR edge-direction.
     *  1 is unreachable from this UI; the values are the core enum's, and it is persisted
     *  as an integer, so they must not be renumbered to close the gap. */
    val upscaler: Int = 0,
    /** EmuCore/GS/FSRSharpness — FSR1 RCAS strength 0..100 (%). Separate from casSharpness:
     *  RCAS runs on a different curve, so the two sliders are not interchangeable. */
    val fsrSharpness: Int = 50,
    /** EmuCore/GS/SGSRSharpness — SGSR edge sharpness 0..200 (%), 100 = Qualcomm's default.
     *  Deliberately NOT shared with fsrSharpness: FSR1's is natively 0..100, so one field would
     *  either redefine existing FSR configurations or force FSR's range to change meaning. */
    val sgsrSharpness: Int = 100,
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
)

/** On-screen display: the performance overlay and its elements. Part of [Settings]. */
data class OsdSettings(
    // Performance Overlay element toggles. Default true to mirror native
    // initialize(), which turns every OsdShow* bit on at first boot.
    // Disabling GPU also stops the GPU timing queries (real perf win).
    /** EmuCore/GS/OsdShowFPS. */
    val osdShowFps: Boolean = false,
    /** EmuCore/GS/OsdScale — size of on-screen messages/stats, percent (25–500, 100 = PCSX2's
     *  normal). Defaults to 65: at 100 the stats block dominates a handheld screen, and 65 matches
     *  the size NetherSX2 ships. Saves still on the old 100 default are migrated once (ConfigStore). */
    val osdScale: Int = 65,
    /** EmuCore/GS/OsdColor — OSD text colour as 0xRRGGBB. 0 = default white. */
    val osdColor: Int = 0,
    /** EmuCore/GS/OsdPerformancePos — which corner the perf stats block sits in.
     *
     *  Stored as PCSX2's own OsdOverlayPos ordinal (1 TopLeft, 3 TopRight, 7 BottomLeft,
     *  9 BottomRight) rather than a 0..3 index of our own, so the value written here is the
     *  value the core reads — no translation table to keep in sync, and an INI hand-edited to
     *  one of the five positions we don't offer (the centres) still round-trips. Default 3 is
     *  PCSX2's own DEFAULT_OSD_PERFORMANCE_POS, so nobody's overlay moves on update. */
    val osdPosition: Int = 3,
    /** EmuCore/GS/OsdShowVPS. */
    val osdShowVps: Boolean = false,
    /** EmuCore/GS/OsdShowSpeed. */
    val osdShowSpeed: Boolean = false,
    /** EmuCore/GS/OsdShowCPU. */
    val osdShowCpu: Boolean = false,
    /** EmuCore/GS/OsdShowGPU. */
    val osdShowGpu: Boolean = false,
    /** EmuCore/GS/OsdShowResolution. */
    val osdShowResolution: Boolean = false,
    /** EmuCore/GS/OsdShowGSStats. */
    val osdShowGsStats: Boolean = false,
    /** EmuCore/GS/OsdShowFrameTimes. */
    val osdShowFrameTimes: Boolean = false,
    /** EmuCore/GS/OsdShowHardwareInfo — the CPU/GPU model info line. */
    val osdShowHardwareInfo: Boolean = false,
    /** EmuCore/GS/OsdMessagesPos — transient OSD notifications (shader-compile
     *  popups, "settings applied", save-state, etc.). true = shown (TopLeft),
     *  false = hidden (None). Achievement popups are separate & unaffected. */
    val osdShowMessages: Boolean = true,
    /** EmuCore/GS/OsdShowGPUStats — GPU pipeline stats (VSI/PSI). Vulkan-only
     *  (GLES has no pipeline_statistics_query); default off since it's a niche
     *  diagnostic that adds per-frame query overhead. */
    val osdShowGpuStats: Boolean = false,
    /** EmuCore/GS/OsdShowVersion — the emulator version line. */
    val osdShowVersion: Boolean = false,
    /** EmuCore/GS/OsdShowSettings — the settings summary (bottom-left). */
    val osdShowSettings: Boolean = false,
    /** EmuCore/GS/OsdShowInputs — the control inputs (bottom-right). */
    val osdShowInputs: Boolean = false,
)


/** `{"<preset path>": {"<parameter name>": value}}` — the wire form of
 *  [Settings.shaderChainParams]. Spelled once and shared by all four places the map has to
 *  cross a boundary (the JSON store, the per-game override diff, the override merge and the
 *  INI seed), because four hand-rolled copies of the same nesting is four chances for one
 *  of them to drift. */
private fun shaderChainParamsToJson(value: Map<String, Map<String, Float>>): JSONObject =
    JSONObject().apply {
        value.forEach { (preset, params) ->
            if (preset.isNotEmpty() && params.isNotEmpty()) {
                put(preset, JSONObject().apply {
                    params.forEach { (name, v) -> put(name, v.toDouble()) }
                })
            }
        }
    }

private fun shaderChainParamsFromJson(json: JSONObject?): Map<String, Map<String, Float>> {
    if (json == null) return emptyMap()
    return buildMap {
        json.keys().forEach { preset ->
            val params = json.optJSONObject(preset) ?: return@forEach
            val values = buildMap<String, Float> {
                params.keys().forEach { name -> put(name, params.optDouble(name, 0.0).toFloat()) }
            }
            // Drop presets whose overrides all went away rather than persisting an empty
            // object that would read back as "this preset is tweaked" forever.
            if (values.isNotEmpty()) put(preset, values)
        }
    }
}

data class Settings(
    val cpu: CpuSettings = CpuSettings(),
    val frameLimit: FrameLimitSettings = FrameLimitSettings(),
    val audio: AudioSettings = AudioSettings(),
    val emuCore: EmuCoreSettings = EmuCoreSettings(),
    val display: DisplaySettings = DisplaySettings(),
    val hwFixes: HwFixesSettings = HwFixesSettings(),
    val output: OutputSettings = OutputSettings(),
    val network: NetworkSettings = NetworkSettings(),
    val system: SystemSettings = SystemSettings(),
    val graphics: GraphicsSettings = GraphicsSettings(),
    val osd: OsdSettings = OsdSettings(),
) {
    val effectiveVu1ClampMode: Int get() = if (cpu.vu1ClampMode < 0) cpu.vuClampMode else cpu.vu1ClampMode

    /** Routes a persisted-key write to the native base layer, or to
     *  [emitSink] when a per-game INI export is capturing the key set (see
     *  [writeGameSettingsIni]). Replaces the direct NativeApp.setSetting calls
     *  so applyTo/writeGsToNative can be reused as the single source of the
     *  field→EmuCore-key mapping for the export — no duplicated key list. */
    private fun put(section: String, key: String, type: String, value: String) {
        val sink = emitSink
        if (sink != null) sink(section, key, type, value)
        else NativeApp.setSetting(section, key, type, value)
    }

    /** Push every field into emucore via NativeApp.setSetting + commit. */
    fun applyTo() {
        // Speedhacks
        put("EmuCore/Speedhacks", "EECycleRate", "int", cpu.eeCycleRate.toString())
        put("EmuCore/Speedhacks", "EECycleSkip", "int", cpu.eeCycleSkip.toString())
        // EE/FPU + VU clamping (recompiler accuracy). Each mode unpacks to the
        // PCSX2 bit flags below. commitSettings resets the recompiler cache
        // in-game, so a game restart is not required.
        put("EmuCore/CPU/Recompiler", "fpuOverflow", "bool", (cpu.eeClampMode >= 1).toString())
        put("EmuCore/CPU/Recompiler", "fpuExtraOverflow", "bool", (cpu.eeClampMode >= 2).toString())
        put("EmuCore/CPU/Recompiler", "fpuFullMode", "bool", (cpu.eeClampMode >= 3).toString())
        // The four are cumulative and emucore validates them as such: an
        // inconsistent set is silently reset to defaults on load rather than
        // rejected, so all four go out together or none of them mean anything.
        put("EmuCore/CPU/Recompiler", "fpuExactMode", "bool", (cpu.eeClampMode >= 4).toString())
        for ((vu, mode) in arrayOf("vu0" to cpu.vuClampMode, "vu1" to effectiveVu1ClampMode)) {
            put("EmuCore/CPU/Recompiler", "${vu}Overflow", "bool", (mode >= 1).toString())
            put("EmuCore/CPU/Recompiler", "${vu}ExtraOverflow", "bool", (mode >= 2).toString())
            put("EmuCore/CPU/Recompiler", "${vu}SignOverflow", "bool", (mode >= 3).toString())
            // Cumulative like the FPU ladder above: emucore resets an
            // ExactMode without SignOverflow back to defaults on load.
            put("EmuCore/CPU/Recompiler", "${vu}ExactMode", "bool", (mode >= 4).toString())
        }
        put("EmuCore/Speedhacks", "vuThread", "bool", cpu.mtvu.toString())
        put("EmuCore/Speedhacks", "vu1Instant", "bool", cpu.vu1Instant.toString())
        put("EmuCore/Speedhacks", "vuFlagHack", "bool", cpu.vuFlagHack.toString())
        put("EmuCore/Speedhacks", "fastCDVD", "bool", cpu.fastCDVD.toString())
        put("EmuCore/Speedhacks", "IntcStat", "bool", cpu.intcStat.toString())
        put("EmuCore/Speedhacks", "WaitLoop", "bool", cpu.waitLoop.toString())
        put("EmuCore/Speedhacks", "vuNeonFusions", "bool", cpu.vuNeonFusions.toString())
        put("EmuCore/Speedhacks", "vuDeferredWrites", "bool", cpu.vuDeferredWrites.toString())
        put("EmuCore/Speedhacks", "vuSkipStallSim", "bool", cpu.vuSkipStallSim.toString())
        // GS frame limit. The setting key is persisted (read by runVMThread
        // after Initialize so cold starts honor the preference) AND the live
        // limiter mode is poked via speedhackLimitermode so toggling in-game
        // takes effect immediately. 0 = Nominal (capped at native rate),
        // 3 = Unlimited.
        put("EmuCore/GS", "FrameLimitEnable", "bool", frameLimit.frameLimitEnable.toString())
        // Preserve an active fast-forward / slow-down latch, exactly as the in-game overlay's
        // own frame-limit path does (MainActivityRuntime). Forcing 0/3 unconditionally here
        // clobbered Turbo on ANY settings apply while fast-forward was engaged — and since
        // fastForwardToggleActive stayed true, the UI kept reporting "Fast Forward ON" with
        // the emulator back at nominal speed. Frame-limit-off masked the bug: that path IS
        // mode 3, so re-asserting it changed nothing, which is why users saw "frame limit off
        // fast-forwards but fast-forward doesn't".
        if (emitSink == null) {
            NativeApp.speedhackLimitermode(
                when {
                    MainActivityRuntime.fastForwardToggleActive -> MainActivityRuntime.ffLimiterMode()
                    MainActivityRuntime.slowDownToggleActive -> 2
                    else -> if (frameLimit.frameLimitEnable) 0 else 3
                }
            )
        }
        // Framerate/NominalScalar — custom speed / FPS cap as a fraction of
        // native. commitSettings → ApplySettings → CheckForEmulationSpeedConfigChanges
        // → UpdateTargetSpeed picks this up live. Clamp mirrors emucore's
        // EmulationSpeedOptions::SanityCheck (0.05..10.0).
        put("Framerate", "NominalScalar", "float",
            (frameLimit.nominalSpeedPercent.coerceIn(10, 1000) / 100f).toString())
        // Live-apply: the setSetting above only persists; the running frame
        // pacer needs a direct re-pace (mirrors speedhackLimitermode).
        if (emitSink == null) NativeApp.setNominalSpeed(frameLimit.nominalSpeedPercent.coerceIn(10, 1000))
        // Max presented-FPS cap — independent of the Speed Limit % above. Caps
        // the display rate by dropping presents on the GS thread (emulation keeps
        // full speed, NominalScalar untouched); 0 = off. See GSRenderer::VSync.
        if (emitSink == null) NativeApp.setFpsCap(frameLimit.fpsLimit.coerceIn(0, 1000))
        // Manual frameskip (0..5) — present 1 of every (N+1) frames. Held as a
        // GS-thread global, applied live; no persisted EmuCore key needed.
        if (emitSink == null) NativeApp.setFrameSkip(frameLimit.frameSkip.coerceIn(0, 5))
        if (emitSink == null) NativeApp.setPortraitRenderTop(output.portraitRenderTop)
        if (emitSink == null) NativeApp.setLandscapeRenderTop(output.landscapeRenderTop)
        // Audio (SPU2). Volume/mute are live native setters; the rest are written
        // to the base layer and applied on commit (SPU2 stream reconfigure).
        if (emitSink == null) NativeApp.setAudioVolume(audio.audioVolume.coerceIn(0, 200))
        if (emitSink == null) NativeApp.setAudioMuted(audio.audioMuted)
        if (emitSink == null) NativeApp.setAudioSwapChannels(audio.audioSwapChannels)
        put("SPU2/Output", "SyncMode", "string", if (audio.audioTimeStretch) "TimeStretch" else "Disabled")
        put("SPU2/Output", "BufferMS", "int", audio.audioBufferMs.coerceIn(10, 200).toString())
        put("SPU2/Output", "OutputLatencyMS", "int", audio.audioOutputLatencyMs.coerceIn(5, 200).toString())
        put("SPU2/Output", "FastForwardVolume", "int", audio.audioFastForwardVolume.coerceIn(0, 200).toString())
        // Opt-in NEON reverb FIR (ARM64). Read by SPU2::InternalReset on the
        // next game boot; default off = scalar reference (unchanged audio).
        put("SPU2", "NeonReverbSIMD", "bool", audio.spu2NeonReverb.toString())
        // Opt-in OpenSL ES output (Oboe). Lives in the SPU2/Output StreamParameters,
        // so ApplySettings → CheckForConfigChanges recreates the stream on toggle.
        put("SPU2/Output", "AndroidOpenSLES", "bool", audio.audioOpenSLES.toString())
        // Lightweight mix (skip reverb) — read live in MixCore via EmuConfig.SPU2.
        put("SPU2/Output", "LightweightMode", "bool", audio.spu2LightweightMix.toString())
        // Patches / cheats (EmuCore). Reloaded by ApplySettings →
        // CheckForPatchConfigChanges; widescreen/no-interlacing take effect on
        // the next boot for most games.
        put("EmuCore", "EnablePatches", "bool", emuCore.enablePatches.toString())
        put("EmuCore", "EnableCheats", "bool", emuCore.enableCheats.toString())
        put("EmuCore", "EnableWideScreenPatches", "bool", emuCore.enableWideScreenPatches.toString())
        put("EmuCore", "EnableNoInterlacingPatches", "bool", emuCore.enableNoInterlacingPatches.toString())
        put("EmuCore", "EnableFastBoot", "bool", emuCore.enableFastBoot.toString())
        put("EmuCore", "HostFs", "bool", emuCore.hostFs.toString())
        put("Achievements", "Enabled", "bool", emuCore.achievements.enabled.toString())
        put("Achievements", "ChallengeMode", "bool", emuCore.achievements.hardcore.toString())
        put("Achievements", "Notifications", "bool", emuCore.achievements.notifications.toString())
        put("Achievements", "LeaderboardNotifications", "bool", emuCore.achievements.leaderboardNotifications.toString())
        put("Achievements", "Overlays", "bool", emuCore.achievements.overlays.toString())
        put("Achievements", "LBOverlays", "bool", emuCore.achievements.lbOverlays.toString())
        put("Achievements", "SoundEffects", "bool", emuCore.achievements.soundEffects.toString())
        put("Achievements", "EncoreMode", "bool", emuCore.achievements.encoreMode.toString())
        put("Achievements", "SpectatorMode", "bool", emuCore.achievements.spectatorMode.toString())
        put("Achievements", "UnofficialTestMode", "bool", emuCore.achievements.unofficialTestMode.toString())
        put("Achievements", "NotificationsDuration", "int", emuCore.achievements.notificationsDuration.coerceIn(3, 30).toString())
        put("Achievements", "LeaderboardsDuration", "int", emuCore.achievements.leaderboardsDuration.coerceIn(3, 30).toString())
        put("Achievements", "NotificationPosition", "int", emuCore.achievements.notificationPosition.toString())
        put("Achievements", "OverlayPosition", "int", emuCore.achievements.overlayPosition.toString())
        put("Achievements", "NotificationScale", "int", emuCore.achievements.notificationScale.coerceIn(50, 250).toString())
        // VMManager::ReloadPINE compares these against the live server and starts, stops or
        // rebinds it, so a commit is enough — no game restart.
        put("EmuCore", "EnablePINE", "bool", emuCore.pineEnabled.toString())
        put("EmuCore", "PINESlot", "int", emuCore.pineSlot.toString())
        put("EmuCore", "EnableGameFixes", "bool", emuCore.enableGameFixes.toString())
        put("EmuCore/Gamefixes", "SoftwareRendererFMVHack", "bool", emuCore.gamefixSoftwareRendererFmv.toString())
        put("EmuCore/Gamefixes", "SkipMPEGHack", "bool", emuCore.gamefixSkipMpeg.toString())
        put("EmuCore/Gamefixes", "EETimingHack", "bool", emuCore.gamefixEETiming.toString())
        put("EmuCore/Gamefixes", "InstantDMAHack", "bool", emuCore.gamefixInstantDma.toString())
        put("EmuCore/Gamefixes", "BlitInternalFPSHack", "bool", emuCore.gamefixBlitInternalFps.toString())
        put("EmuCore/Gamefixes", "OPHFlagHack", "bool", emuCore.gamefixOphFlag.toString())
        put("EmuCore/Gamefixes", "GIFFIFOHack", "bool", emuCore.gamefixGifFifo.toString())
        put("EmuCore/Gamefixes", "DMABusyHack", "bool", emuCore.gamefixDmaBusy.toString())
        put("EmuCore/Gamefixes", "VIF1StallHack", "bool", emuCore.gamefixVif1Stall.toString())
        put("EmuCore/Gamefixes", "IbitHack", "bool", emuCore.gamefixIbit.toString())
        put("EmuCore/Gamefixes", "FullVU0SyncHack", "bool", emuCore.gamefixFullVu0Sync.toString())
        put("EmuCore/Gamefixes", "VuAddSubHack", "bool", emuCore.gamefixVuAddSub.toString())
        put("EmuCore/Gamefixes", "VUOverflowHack", "bool", emuCore.gamefixVuOverflow.toString())
        put("EmuCore/Gamefixes", "XgKickHack", "bool", emuCore.gamefixXgkick.toString())
        put("EmuCore/Gamefixes", "GoemonTlbHack", "bool", emuCore.gamefixGoemonTlb.toString())
        put("EmuCore/Gamefixes", "VUSyncHack", "bool", emuCore.gamefixVuSync.toString())
        put("EmuCore/GS", "SkipDuplicateFrames", "bool", emuCore.skipDuplicateFrames.toString())
        put("EmuCore/CPU", "FPU.Roundmode", "int", emuCore.eeFpuRoundMode.coerceIn(0, 3).toString())
        put("EmuCore/CPU", "VU0.Roundmode", "int", emuCore.vu0RoundMode.coerceIn(0, 3).toString())
        put("EmuCore/CPU", "VU1.Roundmode", "int", emuCore.vu1RoundMode.coerceIn(0, 3).toString())
        // Display + GS renderer + hardware/upscaling-fix keys are all written
        // together in writeGsToNative() below (shared with applyGsLive()).
        // DEV9. Networking/HDD are initialized with the VM, so changes
        // made from the in-game overlay are persisted for the next boot.
        put("DEV9/Eth", "EthEnable", "bool", network.dev9EthEnable.toString())
        put("DEV9/Eth", "EthApi", "string", network.dev9EthApi)
        put("DEV9/Eth", "LocalLinkHost", "bool", network.localLinkHost.toString())
        put("DEV9/Eth", "LocalLinkAddress", "string", network.localLinkAddress)
        put("DEV9/Eth", "LocalLinkPort", "int", network.localLinkPort.coerceIn(1, 65535).toString())
        put("DEV9/Eth", "LocalLinkPeerId", "int", network.localLinkPeerId.coerceIn(1, 65533).toString())
        put("DEV9/Eth", "LocalLinkRoomCode", "string", network.localLinkRoomCode)
        put("DEV9/Eth", "EthDevice", "string", network.dev9EthDevice.ifEmpty { "Auto" })
        put("DEV9/Eth", "EthLogDHCP", "bool", network.dev9EthLogDhcp.toString())
        put("DEV9/Eth", "EthLogDNS", "bool", network.dev9EthLogDns.toString())
        put("DEV9/Eth", "InterceptDHCP", "bool", network.dev9InterceptDhcp.toString())
        put("DEV9/Eth", "PS2IP", "string", network.dev9Ps2Ip.ifEmpty { "0.0.0.0" })
        put("DEV9/Eth", "Mask", "string", network.dev9Mask.ifEmpty { "0.0.0.0" })
        put("DEV9/Eth", "Gateway", "string", network.dev9Gateway.ifEmpty { "0.0.0.0" })
        put("DEV9/Eth", "DNS1", "string", network.dev9Dns1.ifEmpty { "0.0.0.0" })
        put("DEV9/Eth", "DNS2", "string", network.dev9Dns2.ifEmpty { "0.0.0.0" })
        put("DEV9/Eth", "AutoMask", "bool", network.dev9AutoMask.toString())
        put("DEV9/Eth", "AutoGateway", "bool", network.dev9AutoGateway.toString())
        put("DEV9/Eth", "ModeDNS1", "string", network.dev9ModeDns1.ifEmpty { "Auto" })
        put("DEV9/Eth", "ModeDNS2", "string", network.dev9ModeDns2.ifEmpty { "Auto" })
        // Internal-DNS host overrides. Count gates how many Host{i} sections the core reads.
        put("DEV9/Eth/Hosts", "Count", "int", network.dev9EthHosts.size.toString())
        network.dev9EthHosts.forEachIndexed { i, h ->
            put("DEV9/Eth/Hosts/Host$i", "Url", "string", h.url)
            put("DEV9/Eth/Hosts/Host$i", "Desc", "string", "ARMSX2")
            put("DEV9/Eth/Hosts/Host$i", "Address", "string", h.ip.ifEmpty { "0.0.0.0" })
            put("DEV9/Eth/Hosts/Host$i", "Enabled", "bool", h.enabled.toString())
        }
        put("DEV9/Hdd", "HddEnable", "bool", network.dev9HddEnable.toString())
        put("DEV9/Hdd", "HddFile", "string", network.dev9HddFile.ifEmpty { "DEV9hdd.raw" })
        put("MemoryCards", "Slot1_Enable", "bool", system.memoryCardSlot1Enabled.toString())
        put("MemoryCards", "Slot1_Filename", "string", system.memoryCardSlot1Filename.ifEmpty { "mcd001.ps2" })
        put("MemoryCards", "Slot2_Enable", "bool", system.memoryCardSlot2Enabled.toString())
        put("MemoryCards", "Slot2_Filename", "string", system.memoryCardSlot2Filename.ifEmpty { "mcd002.ps2" })
        // USB keyboard (#254). Persist [USB1] Type so USBOptions::LoadSave attaches
        // the emulated HID keyboard on the next boot. The live attach/detach on a
        // running VM is NativeApp.usbApplyPorts below.
        //
        // Port 1 is shared with the USB device picker (UsbDevices), so the switch owns it
        // only while it is on. Off, the port carries what the picker put there. This used
        // to write "None" instead, at every settings apply and every game launch, which
        // unplugged a GunCon 2 or Buzz set on Port 1 before the game ever saw it.
        put("USB1", "Type", "string", if (system.usbKeyboard) "hidkbd" else com.armsx2.input.UsbDevices.storedType(0))
        // Recompiler enables. Picked up by VMManager::ApplySettings →
        // SysCpuProviderPack rebind. Toggling these on a running VM swaps
        // the dispatch pointer; existing JIT block caches are flushed by
        // ApplySettings's CpusChanged path.
        put("EmuCore/CPU/Recompiler", "EnableEE", "bool", cpu.recEE.toString())
        put("EmuCore/CPU/Recompiler", "EnableIOP", "bool", cpu.recIOP.toString())
        put("EmuCore/CPU/Recompiler", "EnableVU0", "bool", cpu.recVU0.toString())
        put("EmuCore/CPU/Recompiler", "EnableVU1", "bool", cpu.recVU1.toString())
        put("EmuCore/CPU/Recompiler", "EnableFastmem", "bool", cpu.enableFastmem.toString())
        // Force the single macOS/PCSX2 ARM64 backend. VMManager also ignores
        // stale UseMac* values, but writing true cleans old persisted settings.
        put("EmuCore/CPU/Recompiler", "UseMacEE", "bool", "true")
        put("EmuCore/CPU/Recompiler", "UseMacIOP", "bool", "true")
        put("EmuCore/CPU/Recompiler", "UseMacVU0", "bool", "true")
        put("EmuCore/CPU/Recompiler", "UseMacVU1", "bool", "true")
        put("EmuCore/CPU/Recompiler", "Vu1InlineFmacStall", "bool", cpu.vu1InlineFmacStall.toString())
        put("EmuCore/CPU/Recompiler", "Vu1CrossBlockPState", "bool", cpu.vu1CrossBlockPState.toString())
        put("EmuCore/CPU/Recompiler", "Vu1InlineDrainTestPipes", "bool", cpu.vu1InlineDrainTestPipes.toString())
        put("EmuCore/CPU/Recompiler", "Vu1FmacInstanceRouting", "bool", cpu.vu1FmacInstanceRouting.toString())
        writeGsToNative()
        // Per-game INI export is capturing the key set only — writeGsToNative()
        // above was the last persisted emit, so stop before the live pokes /
        // commit (they'd re-poke the VM and double-park it; the export must not).
        if (emitSink != null) return
        // Live convenience pokes. Harmless when the GS is closed; commitSettings()
        // below performs the authoritative apply for a cold start / restart.
        // 0..5 — the upper bound is the LAST aspect index, so adding a ratio means widening this
        // too. Left at 4 it silently sent 10:7 to the core no matter what the picker showed, which
        // is the worst version of this bug: the UI looks correct and nothing happens.
        NativeApp.setAspectRatio(output.aspectRatio.coerceIn(0, 8))
        NativeApp.setFmvAspectRatio(output.fmvAspectRatio.coerceIn(0, 8))
        NativeApp.renderTvShader(graphics.tvShader.coerceIn(0, 7))
        NativeApp.renderShadeBoost(
            graphics.shadeBoost,
            graphics.shadeBoostBrightness.coerceIn(1, 100),
            graphics.shadeBoostContrast.coerceIn(1, 100),
            graphics.shadeBoostSaturation.coerceIn(1, 100),
            graphics.shadeBoostGamma.coerceIn(1, 100),
        )
        NativeApp.osdShowFPS(osd.osdShowFps)
        NativeApp.osdSetScale(osd.osdScale.toFloat())
        NativeApp.osdSetColor(osd.osdColor)
        NativeApp.osdShowVPS(osd.osdShowVps)
        NativeApp.osdShowSpeed(osd.osdShowSpeed)
        NativeApp.osdShowCPU(osd.osdShowCpu)
        NativeApp.osdShowGPU(osd.osdShowGpu)
        NativeApp.osdShowResolution(osd.osdShowResolution)
        NativeApp.osdShowGSStats(osd.osdShowGsStats)
        NativeApp.osdShowFrameTimes(osd.osdShowFrameTimes)
        NativeApp.osdShowHardwareInfo(osd.osdShowHardwareInfo)
        NativeApp.osdShowMessages(osd.osdShowMessages)
        NativeApp.osdShowGpuStats(osd.osdShowGpuStats)
        NativeApp.osdShowVersion(osd.osdShowVersion)
        NativeApp.osdShowSettings(osd.osdShowSettings)
        NativeApp.osdShowInputs(osd.osdShowInputs)
        // ★ The OSD MODE overrides every flag just written. Full / Minimal / Off are a separate
        // choice from the per-stat selection above, and this function runs on every settings
        // change — so without this line, changing any unrelated setting quietly swaps an active
        // mode for the Custom flags, which reads to the user as "the OSD disappeared when I
        // changed a setting". Custom is already correct and is left alone.
        com.armsx2.ui.InGameOverlay.reassertOsdModeAfterSettingsApply()
        // USB keyboard (#254): live attach/detach on the running VM, of whatever the
        // [USB1] Type above now names. No-op before the VM exists: the persisted Type
        // handles the cold boot.
        NativeApp.usbApplyPorts()
        NativeApp.commitSettings()
    }

    /** Reverse of [applyTo]: rebuild a Settings from a parsed PCSX2-Android.ini map
     *  (keys "Section/Key" -> raw string value). Any key absent from the map keeps this
     *  Settings' current value (call on Settings() to default-fill). Used to recover an
     *  existing native config when the new UI has no stored config.global (fresh install
     *  over a reused data folder). Mirror applyTo's field->(section,key) mapping EXACTLY.
     *
     *  Only keys applyTo/writeGsToNative actually persist via [put] are inverted here.
     *  Live-only pokes (fpsLimit, frameSkip, audioVolume/Muted/SwapChannels) and the
     *  launch-time renderer/upscaleFloat helpers write no base-layer key, so those fields
     *  keep their current value. UseMac* are legacy/forced-on (applyTo always writes
     *  "true"), so they are forced true here to match fromJson. */
    fun readFromIni(ini: Map<String, String>): Settings {
        // Typed lookups: null when the key is absent (or unparseable) so callers
        // fall back to `this.<field>` via ?:.
        fun boolAt(key: String): Boolean? = ini[key]?.let { it == "true" || it == "1" }
        fun intAt(key: String): Int? = ini[key]?.toIntOrNull()
        fun floatAt(key: String): Float? = ini[key]?.toFloatOrNull()
        fun strAt(key: String): String? = ini[key]

        // EE/FPU clamp (0 None / 1 Normal / 2 Extra / 3 Full / 4 Exact) is packed by
        // applyTo into four cumulative bool keys (fpuOverflow>=1, fpuExtraOverflow>=2,
        // fpuFullMode>=3, fpuExactMode>=4). Read them back highest-first, and treat the
        // exact key's absence as an older core rather than as mode 3 — a build without it
        // never wrote the key, and inferring 3 there would silently demote the setting.
        val eeClamp = run {
            val fo = boolAt("EmuCore/CPU/Recompiler/fpuOverflow")
            val fe = boolAt("EmuCore/CPU/Recompiler/fpuExtraOverflow")
            val ff = boolAt("EmuCore/CPU/Recompiler/fpuFullMode")
            val fx = boolAt("EmuCore/CPU/Recompiler/fpuExactMode")
            if (fo == null && fe == null && ff == null && fx == null) this.cpu.eeClampMode
            else if (fx == true) 4 else if (ff == true) 3 else if (fe == true) 2 else if (fo == true) 1 else 0
        }
        // VU clamp (0 None / 1 Normal / 2 Extra / 3 Extra+Sign / 4 Exact) — the same packing,
        // once per VU. Treat the exact key's absence as an older core rather than as mode 3 — a
        // build without it never wrote the key, and inferring 3 there would silently demote the
        // setting.
        fun vuClampAt(vu: String, fallback: Int): Int {
            val o = boolAt("EmuCore/CPU/Recompiler/${vu}Overflow")
            val e = boolAt("EmuCore/CPU/Recompiler/${vu}ExtraOverflow")
            val sgn = boolAt("EmuCore/CPU/Recompiler/${vu}SignOverflow")
            val fx = boolAt("EmuCore/CPU/Recompiler/${vu}ExactMode")
            return if (o == null && e == null && sgn == null && fx == null) fallback
            else if (fx == true) 4 else if (sgn == true) 3 else if (e == true) 2 else if (o == true) 1 else 0
        }
        val vuClamp = vuClampAt("vu0", this.cpu.vuClampMode)
        val vu1Clamp = vuClampAt("vu1", this.effectiveVu1ClampMode).let { if (it == vuClamp) -1 else it }

        // renderer + upscale aren't written by applyTo's put() — the core / renderUpscalemultiplier
        // persist them to the base layer directly — so recover them from the native keys.
        // GSRendererType: Auto=-1, OGL=12, SW=13, VK=14.
        val recoveredRenderer = when (intAt("EmuCore/GS/Renderer")) {
            -1 -> "auto"
            12 -> "opengl"
            13 -> "software"
            14 -> "vulkan"
            else -> this.output.renderer
        }

        return this.copy(
            cpu = this.cpu.copy(
                // ---- EmuCore/Speedhacks ----
                eeCycleRate = intAt("EmuCore/Speedhacks/EECycleRate") ?: this.cpu.eeCycleRate,
                eeCycleSkip = intAt("EmuCore/Speedhacks/EECycleSkip") ?: this.cpu.eeCycleSkip,
                eeClampMode = eeClamp,
                vuClampMode = vuClamp,
                vu1ClampMode = vu1Clamp,
                mtvu = boolAt("EmuCore/Speedhacks/vuThread") ?: this.cpu.mtvu,
                vu1Instant = boolAt("EmuCore/Speedhacks/vu1Instant") ?: this.cpu.vu1Instant,
                vuFlagHack = boolAt("EmuCore/Speedhacks/vuFlagHack") ?: this.cpu.vuFlagHack,
                fastCDVD = boolAt("EmuCore/Speedhacks/fastCDVD") ?: this.cpu.fastCDVD,
                intcStat = boolAt("EmuCore/Speedhacks/IntcStat") ?: this.cpu.intcStat,
                waitLoop = boolAt("EmuCore/Speedhacks/WaitLoop") ?: this.cpu.waitLoop,
                vuNeonFusions = boolAt("EmuCore/Speedhacks/vuNeonFusions") ?: this.cpu.vuNeonFusions,
                vuDeferredWrites = boolAt("EmuCore/Speedhacks/vuDeferredWrites") ?: this.cpu.vuDeferredWrites,
                vuSkipStallSim = boolAt("EmuCore/Speedhacks/vuSkipStallSim") ?: this.cpu.vuSkipStallSim,
                // ---- EmuCore/CPU/Recompiler enables ----
                recEE = boolAt("EmuCore/CPU/Recompiler/EnableEE") ?: this.cpu.recEE,
                recIOP = boolAt("EmuCore/CPU/Recompiler/EnableIOP") ?: this.cpu.recIOP,
                recVU0 = boolAt("EmuCore/CPU/Recompiler/EnableVU0") ?: this.cpu.recVU0,
                recVU1 = boolAt("EmuCore/CPU/Recompiler/EnableVU1") ?: this.cpu.recVU1,
                enableFastmem = boolAt("EmuCore/CPU/Recompiler/EnableFastmem") ?: this.cpu.enableFastmem,
                // Legacy/forced-on ARM64 backend flags — always "true" in the INI (mirror fromJson).
                useMacEE = true,
                useMacIOP = true,
                useMacVU0 = true,
                useMacVU1 = true,
                vu1InlineFmacStall = boolAt("EmuCore/CPU/Recompiler/Vu1InlineFmacStall") ?: this.cpu.vu1InlineFmacStall,
                vu1CrossBlockPState = boolAt("EmuCore/CPU/Recompiler/Vu1CrossBlockPState") ?: this.cpu.vu1CrossBlockPState,
                vu1InlineDrainTestPipes = boolAt("EmuCore/CPU/Recompiler/Vu1InlineDrainTestPipes") ?: this.cpu.vu1InlineDrainTestPipes,
                vu1FmacInstanceRouting = boolAt("EmuCore/CPU/Recompiler/Vu1FmacInstanceRouting") ?: this.cpu.vu1FmacInstanceRouting,
            ),
            frameLimit = this.frameLimit.copy(
                // ---- Frame limiter (nominalSpeedPercent stored as the 0.10..10.0 scalar) ----
                frameLimitEnable = boolAt("EmuCore/GS/FrameLimitEnable") ?: this.frameLimit.frameLimitEnable,
                nominalSpeedPercent = floatAt("Framerate/NominalScalar")?.let { Math.round(it * 100f) }
                ?: this.frameLimit.nominalSpeedPercent,
            ),
            audio = this.audio.copy(
                // ---- Audio (SPU2/Output) — SyncMode is TimeStretch/Disabled ----
                audioTimeStretch = strAt("SPU2/Output/SyncMode")?.let { it == "TimeStretch" } ?: this.audio.audioTimeStretch,
                audioBufferMs = intAt("SPU2/Output/BufferMS") ?: this.audio.audioBufferMs,
                audioOutputLatencyMs = intAt("SPU2/Output/OutputLatencyMS") ?: this.audio.audioOutputLatencyMs,
                audioFastForwardVolume = intAt("SPU2/Output/FastForwardVolume") ?: this.audio.audioFastForwardVolume,
                spu2NeonReverb = boolAt("SPU2/NeonReverbSIMD") ?: this.audio.spu2NeonReverb,
                audioOpenSLES = boolAt("SPU2/Output/AndroidOpenSLES") ?: this.audio.audioOpenSLES,
                spu2LightweightMix = boolAt("SPU2/Output/LightweightMode") ?: this.audio.spu2LightweightMix,
            ),
            emuCore = this.emuCore.copy(
                // ---- EmuCore patches / cheats ----
                enablePatches = boolAt("EmuCore/EnablePatches") ?: this.emuCore.enablePatches,
                enableCheats = boolAt("EmuCore/EnableCheats") ?: this.emuCore.enableCheats,
                enableWideScreenPatches = boolAt("EmuCore/EnableWideScreenPatches") ?: this.emuCore.enableWideScreenPatches,
                enableNoInterlacingPatches = boolAt("EmuCore/EnableNoInterlacingPatches") ?: this.emuCore.enableNoInterlacingPatches,
                enableFastBoot = boolAt("EmuCore/EnableFastBoot") ?: this.emuCore.enableFastBoot,
                hostFs = boolAt("EmuCore/HostFs") ?: this.emuCore.hostFs,
                achievements = AchievementsSettings(
                enabled = boolAt("Achievements/Enabled") ?: this.emuCore.achievements.enabled,
                hardcore = boolAt("Achievements/ChallengeMode") ?: this.emuCore.achievements.hardcore,
                notifications = boolAt("Achievements/Notifications") ?: this.emuCore.achievements.notifications,
                leaderboardNotifications = boolAt("Achievements/LeaderboardNotifications") ?: this.emuCore.achievements.leaderboardNotifications,
                overlays = boolAt("Achievements/Overlays") ?: this.emuCore.achievements.overlays,
                lbOverlays = boolAt("Achievements/LBOverlays") ?: this.emuCore.achievements.lbOverlays,
                soundEffects = boolAt("Achievements/SoundEffects") ?: this.emuCore.achievements.soundEffects,
                encoreMode = boolAt("Achievements/EncoreMode") ?: this.emuCore.achievements.encoreMode,
                spectatorMode = boolAt("Achievements/SpectatorMode") ?: this.emuCore.achievements.spectatorMode,
                unofficialTestMode = boolAt("Achievements/UnofficialTestMode") ?: this.emuCore.achievements.unofficialTestMode,
                notificationsDuration = intAt("Achievements/NotificationsDuration") ?: this.emuCore.achievements.notificationsDuration,
                leaderboardsDuration = intAt("Achievements/LeaderboardsDuration") ?: this.emuCore.achievements.leaderboardsDuration,
                notificationPosition = intAt("Achievements/NotificationPosition") ?: this.emuCore.achievements.notificationPosition,
                overlayPosition = intAt("Achievements/OverlayPosition") ?: this.emuCore.achievements.overlayPosition,
                notificationScale = intAt("Achievements/NotificationScale") ?: this.emuCore.achievements.notificationScale,
            ),
                pineEnabled = boolAt("EmuCore/EnablePINE") ?: this.emuCore.pineEnabled,
                pineSlot = intAt("EmuCore/PINESlot") ?: this.emuCore.pineSlot,
                enableGameFixes = boolAt("EmuCore/EnableGameFixes") ?: this.emuCore.enableGameFixes,
                // ---- EmuCore/Gamefixes ----
                gamefixSoftwareRendererFmv = boolAt("EmuCore/Gamefixes/SoftwareRendererFMVHack") ?: this.emuCore.gamefixSoftwareRendererFmv,
                gamefixSkipMpeg = boolAt("EmuCore/Gamefixes/SkipMPEGHack") ?: this.emuCore.gamefixSkipMpeg,
                gamefixEETiming = boolAt("EmuCore/Gamefixes/EETimingHack") ?: this.emuCore.gamefixEETiming,
                gamefixInstantDma = boolAt("EmuCore/Gamefixes/InstantDMAHack") ?: this.emuCore.gamefixInstantDma,
                gamefixBlitInternalFps = boolAt("EmuCore/Gamefixes/BlitInternalFPSHack") ?: this.emuCore.gamefixBlitInternalFps,
                gamefixOphFlag = boolAt("EmuCore/Gamefixes/OPHFlagHack") ?: this.emuCore.gamefixOphFlag,
                gamefixGifFifo = boolAt("EmuCore/Gamefixes/GIFFIFOHack") ?: this.emuCore.gamefixGifFifo,
                gamefixDmaBusy = boolAt("EmuCore/Gamefixes/DMABusyHack") ?: this.emuCore.gamefixDmaBusy,
                gamefixVif1Stall = boolAt("EmuCore/Gamefixes/VIF1StallHack") ?: this.emuCore.gamefixVif1Stall,
                gamefixIbit = boolAt("EmuCore/Gamefixes/IbitHack") ?: this.emuCore.gamefixIbit,
                gamefixFullVu0Sync = boolAt("EmuCore/Gamefixes/FullVU0SyncHack") ?: this.emuCore.gamefixFullVu0Sync,
                gamefixVuAddSub = boolAt("EmuCore/Gamefixes/VuAddSubHack") ?: this.emuCore.gamefixVuAddSub,
                gamefixVuOverflow = boolAt("EmuCore/Gamefixes/VUOverflowHack") ?: this.emuCore.gamefixVuOverflow,
                gamefixXgkick = boolAt("EmuCore/Gamefixes/XgKickHack") ?: this.emuCore.gamefixXgkick,
                gamefixGoemonTlb = boolAt("EmuCore/Gamefixes/GoemonTlbHack") ?: this.emuCore.gamefixGoemonTlb,
                gamefixVuSync = boolAt("EmuCore/Gamefixes/VUSyncHack") ?: this.emuCore.gamefixVuSync,
                skipDuplicateFrames = boolAt("EmuCore/GS/SkipDuplicateFrames") ?: this.emuCore.skipDuplicateFrames,
                eeFpuRoundMode = intAt("EmuCore/CPU/FPU.Roundmode") ?: this.emuCore.eeFpuRoundMode,
                vu0RoundMode = intAt("EmuCore/CPU/VU0.Roundmode") ?: this.emuCore.vu0RoundMode,
                vu1RoundMode = intAt("EmuCore/CPU/VU1.Roundmode") ?: this.emuCore.vu1RoundMode,
            ),
            display = this.display.copy(
                vsyncEnable = boolAt("EmuCore/GS/VsyncEnable") ?: this.display.vsyncEnable,
                screenOffsets = boolAt("EmuCore/GS/pcrtc_offsets") ?: this.display.screenOffsets,
                showOverscan = boolAt("EmuCore/GS/pcrtc_overscan") ?: this.display.showOverscan,
                antiBlur = boolAt("EmuCore/GS/pcrtc_antiblur") ?: this.display.antiBlur,
                disableInterlaceOffset = boolAt("EmuCore/GS/disable_interlace_offset") ?: this.display.disableInterlaceOffset,
                syncToHostRefresh = boolAt("EmuCore/GS/SyncToHostRefreshRate") ?: this.display.syncToHostRefresh,
                disableFramebufferFetch = boolAt("EmuCore/GS/DisableFramebufferFetch") ?: this.display.disableFramebufferFetch,
                hwRov = boolAt("EmuCore/GS/HWROV") ?: this.display.hwRov,
                hwAa1 = boolAt("EmuCore/GS/HWAA1") ?: this.display.hwAa1,
                coalesceRenderPasses = boolAt("EmuCore/GS/CoalesceRenderPasses") ?: this.display.coalesceRenderPasses,
                forceMaliFbFetch = boolAt("EmuCore/GS/ForceMaliFramebufferFetch") ?: this.display.forceMaliFbFetch,
                useAngleOpenGL = boolAt("EmuCore/GS/AndroidUseAngleOpenGL") ?: this.display.useAngleOpenGL,
                overrideTextureBarriers = intAt("EmuCore/GS/OverrideTextureBarriers") ?: this.display.overrideTextureBarriers,
                gsBackThreadMode = intAt("EmuCore/GS/GSBackThreadMode") ?: this.display.gsBackThreadMode,
                disableVertexShaderExpand = boolAt("EmuCore/GS/DisableVertexShaderExpand") ?: this.display.disableVertexShaderExpand,
                useBlitSwapChain = boolAt("EmuCore/GS/UseBlitSwapChain") ?: this.display.useBlitSwapChain,
                disableShaderCache = boolAt("EmuCore/GS/DisableShaderCache") ?: this.display.disableShaderCache,
                hwAccurateAlphaTest = boolAt("EmuCore/GS/HWAccurateAlphaTest") ?: this.display.hwAccurateAlphaTest,
            ),
            hwFixes = this.hwFixes.copy(
                drawBuffering = boolAt("EmuCore/GS/UserHacks_DrawBuffering") ?: this.hwFixes.drawBuffering,
                spinGpuReadbacks = boolAt("EmuCore/GS/HWSpinGPUForReadbacks") ?: this.hwFixes.spinGpuReadbacks,
                spinCpuReadbacks = boolAt("EmuCore/GS/HWSpinCPUForReadbacks") ?: this.hwFixes.spinCpuReadbacks,
                integerScaling = boolAt("EmuCore/GS/IntegerScaling") ?: this.hwFixes.integerScaling,
                cropLeft = intAt("EmuCore/GS/CropLeft") ?: this.hwFixes.cropLeft,
                cropTop = intAt("EmuCore/GS/CropTop") ?: this.hwFixes.cropTop,
                cropRight = intAt("EmuCore/GS/CropRight") ?: this.hwFixes.cropRight,
                cropBottom = intAt("EmuCore/GS/CropBottom") ?: this.hwFixes.cropBottom,
                dithering = intAt("EmuCore/GS/dithering_ps2") ?: this.hwFixes.dithering,
                vsyncQueueSize = intAt("EmuCore/GS/VsyncQueueSize") ?: this.hwFixes.vsyncQueueSize,
                skipDrawStart = intAt("EmuCore/GS/UserHacks_SkipDraw_Start") ?: this.hwFixes.skipDrawStart,
                skipDrawEnd = intAt("EmuCore/GS/UserHacks_SkipDraw_End") ?: this.hwFixes.skipDrawEnd,
                // "UserHacks" is applyTo's derived master (manualUserHacks OR any hack set);
                // recovering it into manualUserHacks is idempotent — the individual hacks below
                // re-derive it when re-applied, and it preserves a master-on-with-no-hacks state.
                manualUserHacks = boolAt("EmuCore/GS/UserHacks") ?: this.hwFixes.manualUserHacks,
                autoFlush = intAt("EmuCore/GS/UserHacks_AutoFlushLevel") ?: this.hwFixes.autoFlush,
                halfPixelOffset = intAt("EmuCore/GS/UserHacks_HalfPixelOffset") ?: this.hwFixes.halfPixelOffset,
                limit24BitDepth = intAt("EmuCore/GS/UserHacks_Limit24BitDepth") ?: this.hwFixes.limit24BitDepth,
                textureInsideRt = intAt("EmuCore/GS/UserHacks_TextureInsideRt") ?: this.hwFixes.textureInsideRt,
                nativeScaling = intAt("EmuCore/GS/UserHacks_native_scaling") ?: this.hwFixes.nativeScaling,
                roundSprite = intAt("EmuCore/GS/UserHacks_round_sprite_offset") ?: this.hwFixes.roundSprite,
                bilinearUpscale = intAt("EmuCore/GS/UserHacks_BilinearHack") ?: this.hwFixes.bilinearUpscale,
                gpuTargetClut = intAt("EmuCore/GS/UserHacks_GPUTargetCLUTMode") ?: this.hwFixes.gpuTargetClut,
                cpuSpriteRenderBw = intAt("EmuCore/GS/UserHacks_CPUSpriteRenderBW") ?: this.hwFixes.cpuSpriteRenderBw,
                cpuSpriteRenderLevel = intAt("EmuCore/GS/UserHacks_CPUSpriteRenderLevel") ?: this.hwFixes.cpuSpriteRenderLevel,
                cpuClutRender = intAt("EmuCore/GS/UserHacks_CPUCLUTRender") ?: this.hwFixes.cpuClutRender,
                alignSprite = boolAt("EmuCore/GS/UserHacks_align_sprite_X") ?: this.hwFixes.alignSprite,
                mergeSprite = boolAt("EmuCore/GS/UserHacks_merge_pp_sprite") ?: this.hwFixes.mergeSprite,
                forceEvenSpritePosition = boolAt("EmuCore/GS/UserHacks_ForceEvenSpritePosition") ?: this.hwFixes.forceEvenSpritePosition,
                unscaledPaletteDraw = boolAt("EmuCore/GS/UserHacks_NativePaletteDraw") ?: this.hwFixes.unscaledPaletteDraw,
                textureOffsetX = intAt("EmuCore/GS/UserHacks_TCOffsetX") ?: this.hwFixes.textureOffsetX,
                textureOffsetY = intAt("EmuCore/GS/UserHacks_TCOffsetY") ?: this.hwFixes.textureOffsetY,
                gpuPaletteConversion = boolAt("EmuCore/GS/paltex") ?: this.hwFixes.gpuPaletteConversion,
                cpuFramebufferConversion = boolAt("EmuCore/GS/UserHacks_CPU_FB_Conversion") ?: this.hwFixes.cpuFramebufferConversion,
                readTargetsWhenClosing = boolAt("EmuCore/GS/UserHacks_ReadTCOnClose") ?: this.hwFixes.readTargetsWhenClosing,
                disableDepthEmulation = boolAt("EmuCore/GS/UserHacks_DisableDepthSupport") ?: this.hwFixes.disableDepthEmulation,
                disablePartialInvalidation = boolAt("EmuCore/GS/UserHacks_DisablePartialInvalidation") ?: this.hwFixes.disablePartialInvalidation,
                disableSafeFeatures = boolAt("EmuCore/GS/UserHacks_Disable_Safe_Features") ?: this.hwFixes.disableSafeFeatures,
                disableRenderFixes = boolAt("EmuCore/GS/UserHacks_DisableRenderFixes") ?: this.hwFixes.disableRenderFixes,
                preloadFrameData = boolAt("EmuCore/GS/preload_frame_with_gs_data") ?: this.hwFixes.preloadFrameData,
                estimateTextureRegion = boolAt("EmuCore/GS/UserHacks_EstimateTextureRegion") ?: this.hwFixes.estimateTextureRegion,
                triFilter = intAt("EmuCore/GS/TriFilter") ?: this.hwFixes.triFilter,
                maxAnisotropy = intAt("EmuCore/GS/MaxAnisotropy") ?: this.hwFixes.maxAnisotropy,
                gpuProfile = when (strAt("EmuCore/GS/AndroidGpuProfileOverride")) {
                "mali" -> 1
                "adreno" -> 2
                "powervr" -> 3
                "xclipse" -> 4
                "auto" -> 0
                else -> this.hwFixes.gpuProfile
            },
            ),
            output = this.output.copy(
                // ---- Renderer + upscale (base-layer keys, not applyTo put()) ----
                renderer = recoveredRenderer,
                upscaleFloat = floatAt("EmuCore/GS/upscale_multiplier") ?: this.output.upscaleFloat,
                // ---- EmuCore/GS (writeGsToNative). Aspect/FMV/gpuProfile stored as names. ----
                customAspectRatio = floatAt("EmuCore/GS/CustomAspectRatio") ?: this.output.customAspectRatio,
                aspectRatio = when (strAt("EmuCore/GS/AspectRatio")) {
                "Stretch" -> 0
                "Auto 4:3/3:2" -> 1
                "4:3" -> 2
                "16:9" -> 3
                "10:7" -> 4
                "21:9" -> 5
                "20:9" -> 6
                "19.5:9" -> 7
                "Custom" -> 8
                else -> this.output.aspectRatio
            },
                fmvAspectRatio = when (strAt("EmuCore/GS/FMVAspectRatioSwitch")) {
                "Off" -> 0
                "Auto 4:3/3:2" -> 1
                "4:3" -> 2
                "16:9" -> 3
                "10:7" -> 4
                "21:9" -> 5
                "20:9" -> 6
                "19.5:9" -> 7
                "Custom" -> 8
                else -> this.output.fmvAspectRatio
            },
                deinterlaceMode = intAt("EmuCore/GS/deinterlace_mode") ?: this.output.deinterlaceMode,
                framerateNtsc = floatAt("EmuCore/GS/FramerateNTSC") ?: this.output.framerateNtsc,
                frameratePal = floatAt("EmuCore/GS/FrameratePAL") ?: this.output.frameratePal,
                autoFlushSw = boolAt("EmuCore/GS/autoflush_sw") ?: this.output.autoFlushSw,
                mipmapSw = boolAt("EmuCore/GS/mipmap") ?: this.output.mipmapSw,
                swThreads = intAt("EmuCore/GS/extrathreads") ?: this.output.swThreads,
                swThreadsHeight = intAt("EmuCore/GS/extrathreads_height") ?: this.output.swThreadsHeight,
            ),
            network = this.network.copy(
                // ---- DEV9 — Ethernet / HDD ----
                dev9EthEnable = boolAt("DEV9/Eth/EthEnable") ?: this.network.dev9EthEnable,
                dev9EthApi = strAt("DEV9/Eth/EthApi") ?: this.network.dev9EthApi,
                localLinkHost = boolAt("DEV9/Eth/LocalLinkHost") ?: this.network.localLinkHost,
                localLinkAddress = strAt("DEV9/Eth/LocalLinkAddress") ?: this.network.localLinkAddress,
                localLinkPort = intAt("DEV9/Eth/LocalLinkPort") ?: this.network.localLinkPort,
                localLinkPeerId = intAt("DEV9/Eth/LocalLinkPeerId") ?: this.network.localLinkPeerId,
                localLinkRoomCode = strAt("DEV9/Eth/LocalLinkRoomCode") ?: this.network.localLinkRoomCode,
                dev9EthDevice = strAt("DEV9/Eth/EthDevice") ?: this.network.dev9EthDevice,
                dev9EthLogDhcp = boolAt("DEV9/Eth/EthLogDHCP") ?: this.network.dev9EthLogDhcp,
                dev9EthLogDns = boolAt("DEV9/Eth/EthLogDNS") ?: this.network.dev9EthLogDns,
                dev9InterceptDhcp = boolAt("DEV9/Eth/InterceptDHCP") ?: this.network.dev9InterceptDhcp,
                dev9Ps2Ip = strAt("DEV9/Eth/PS2IP") ?: this.network.dev9Ps2Ip,
                dev9Mask = strAt("DEV9/Eth/Mask") ?: this.network.dev9Mask,
                dev9Gateway = strAt("DEV9/Eth/Gateway") ?: this.network.dev9Gateway,
                dev9Dns1 = strAt("DEV9/Eth/DNS1") ?: this.network.dev9Dns1,
                dev9Dns2 = strAt("DEV9/Eth/DNS2") ?: this.network.dev9Dns2,
                dev9AutoMask = boolAt("DEV9/Eth/AutoMask") ?: this.network.dev9AutoMask,
                dev9AutoGateway = boolAt("DEV9/Eth/AutoGateway") ?: this.network.dev9AutoGateway,
                dev9ModeDns1 = strAt("DEV9/Eth/ModeDNS1") ?: this.network.dev9ModeDns1,
                dev9ModeDns2 = strAt("DEV9/Eth/ModeDNS2") ?: this.network.dev9ModeDns2,
                // Internal-DNS host overrides — Count gates Host{i} sections (Desc is ignored).
                dev9EthHosts = run {
                val count = intAt("DEV9/Eth/Hosts/Count") ?: return@run this.network.dev9EthHosts
                (0 until count).mapNotNull { idx ->
                    val url = ini["DEV9/Eth/Hosts/Host$idx/Url"] ?: return@mapNotNull null
                    Dev9HostMapping(
                        url = url,
                        ip = (ini["DEV9/Eth/Hosts/Host$idx/Address"] ?: "0.0.0.0").ifEmpty { "0.0.0.0" },
                        enabled = boolAt("DEV9/Eth/Hosts/Host$idx/Enabled") ?: true,
                    )
                }.filter { it.url.isNotBlank() }
            },
                dev9HddEnable = boolAt("DEV9/Hdd/HddEnable") ?: this.network.dev9HddEnable,
                dev9HddFile = strAt("DEV9/Hdd/HddFile") ?: this.network.dev9HddFile,
            ),
            system = this.system.copy(
                // ---- MemoryCards ----
                memoryCardSlot1Enabled = boolAt("MemoryCards/Slot1_Enable") ?: this.system.memoryCardSlot1Enabled,
                memoryCardSlot1Filename = strAt("MemoryCards/Slot1_Filename") ?: this.system.memoryCardSlot1Filename,
                memoryCardSlot2Enabled = boolAt("MemoryCards/Slot2_Enable") ?: this.system.memoryCardSlot2Enabled,
                memoryCardSlot2Filename = strAt("MemoryCards/Slot2_Filename") ?: this.system.memoryCardSlot2Filename,
                // ---- USB keyboard (USB1/Type = hidkbd/None) ----
                usbKeyboard = strAt("USB1/Type")?.let { it == "hidkbd" } ?: this.system.usbKeyboard,
            ),
            graphics = this.graphics.copy(
                hwMipmap = boolAt("EmuCore/GS/hw_mipmap") ?: this.graphics.hwMipmap,
                accurateBlendingUnit = intAt("EmuCore/GS/accurate_blending_unit") ?: this.graphics.accurateBlendingUnit,
                textureFiltering = intAt("EmuCore/GS/filter") ?: this.graphics.textureFiltering,
                displayBilinear = intAt("EmuCore/GS/linear_present_mode") ?: this.graphics.displayBilinear,
                texturePreloading = intAt("EmuCore/GS/texture_preloading") ?: this.graphics.texturePreloading,
                hardwareDownloadMode = intAt("EmuCore/GS/HWDownloadMode") ?: this.graphics.hardwareDownloadMode,
                tvShader = intAt("EmuCore/GS/TVShader") ?: this.graphics.tvShader,
                shadeBoost = boolAt("EmuCore/GS/ShadeBoost") ?: this.graphics.shadeBoost,
                shadeBoostBrightness = intAt("EmuCore/GS/ShadeBoost_Brightness") ?: this.graphics.shadeBoostBrightness,
                shadeBoostContrast = intAt("EmuCore/GS/ShadeBoost_Contrast") ?: this.graphics.shadeBoostContrast,
                shadeBoostSaturation = intAt("EmuCore/GS/ShadeBoost_Saturation") ?: this.graphics.shadeBoostSaturation,
                shadeBoostGamma = intAt("EmuCore/GS/ShadeBoost_Gamma") ?: this.graphics.shadeBoostGamma,
                fxaa = boolAt("EmuCore/GS/fxaa") ?: this.graphics.fxaa,
                shaderChainEnabled = boolAt("EmuCore/GS/ShaderChainEnabled") ?: this.graphics.shaderChainEnabled,
                shaderChainPreset = strAt("EmuCore/GS/ShaderChainPreset") ?: this.graphics.shaderChainPreset,
                lsfgEnabled = boolAt("EmuCore/GS/LsfgEnabled") ?: this.graphics.lsfgEnabled,
                lsfgMultiplier = intAt("EmuCore/GS/LsfgMultiplier") ?: this.graphics.lsfgMultiplier,
                lsfgDllPath = strAt("EmuCore/GS/LsfgDllPath") ?: this.graphics.lsfgDllPath,
                lsfgPerformance = boolAt("EmuCore/GS/LsfgPerformance") ?: this.graphics.lsfgPerformance,
                lsfgFlowScale = intAt("EmuCore/GS/LsfgFlowScale") ?: this.graphics.lsfgFlowScale,
                lsfgTargetRate = intAt("EmuCore/GS/LsfgTargetRate") ?: this.graphics.lsfgTargetRate,
                shaderChainParams = strAt("EmuCore/GS/ShaderChainParams")?.let { raw ->
                // Hand-editable file, so a malformed blob is a real possibility: keep the
                // rest of the recovered settings rather than throwing the lot away.
                runCatching { shaderChainParamsFromJson(JSONObject(raw)) }.getOrNull()
            } ?: this.graphics.shaderChainParams,
                casMode = intAt("EmuCore/GS/CASMode") ?: this.graphics.casMode,
                casSharpness = intAt("EmuCore/GS/CASSharpness") ?: this.graphics.casSharpness,
                upscaler = intAt("EmuCore/GS/Upscaler") ?: this.graphics.upscaler,
                fsrSharpness = intAt("EmuCore/GS/FSRSharpness") ?: this.graphics.fsrSharpness,
                sgsrSharpness = intAt("EmuCore/GS/SGSRSharpness") ?: this.graphics.sgsrSharpness,
                loadTextureReplacements = boolAt("EmuCore/GS/LoadTextureReplacements") ?: this.graphics.loadTextureReplacements,
                loadTextureReplacementsAsync = boolAt("EmuCore/GS/LoadTextureReplacementsAsync") ?: this.graphics.loadTextureReplacementsAsync,
                precacheTextureReplacements = boolAt("EmuCore/GS/PrecacheTextureReplacements") ?: this.graphics.precacheTextureReplacements,
                dumpReplaceableTextures = boolAt("EmuCore/GS/DumpReplaceableTextures") ?: this.graphics.dumpReplaceableTextures,
                osdShowTextureReplacements = boolAt("EmuCore/GS/OsdShowTextureReplacements") ?: this.graphics.osdShowTextureReplacements,
            ),
            osd = this.osd.copy(
                osdShowFps = boolAt("EmuCore/GS/OsdShowFPS") ?: this.osd.osdShowFps,
                osdScale = intAt("EmuCore/GS/OsdScale") ?: this.osd.osdScale,
                osdColor = intAt("EmuCore/GS/OsdColor") ?: this.osd.osdColor,
                osdPosition = intAt("EmuCore/GS/OsdPerformancePos") ?: this.osd.osdPosition,
                osdShowVps = boolAt("EmuCore/GS/OsdShowVPS") ?: this.osd.osdShowVps,
                osdShowSpeed = boolAt("EmuCore/GS/OsdShowSpeed") ?: this.osd.osdShowSpeed,
                osdShowCpu = boolAt("EmuCore/GS/OsdShowCPU") ?: this.osd.osdShowCpu,
                osdShowGpu = boolAt("EmuCore/GS/OsdShowGPU") ?: this.osd.osdShowGpu,
                osdShowResolution = boolAt("EmuCore/GS/OsdShowResolution") ?: this.osd.osdShowResolution,
                osdShowGsStats = boolAt("EmuCore/GS/OsdShowGSStats") ?: this.osd.osdShowGsStats,
                osdShowFrameTimes = boolAt("EmuCore/GS/OsdShowFrameTimes") ?: this.osd.osdShowFrameTimes,
                osdShowHardwareInfo = boolAt("EmuCore/GS/OsdShowHardwareInfo") ?: this.osd.osdShowHardwareInfo,
                // OsdMessagesPos is an enum int (0 None / 1 TopLeft); applyTo writes 1 when shown.
                osdShowMessages = intAt("EmuCore/GS/OsdMessagesPos")?.let { it != 0 } ?: this.osd.osdShowMessages,
                osdShowGpuStats = boolAt("EmuCore/GS/OsdShowGPUStats") ?: this.osd.osdShowGpuStats,
                osdShowVersion = boolAt("EmuCore/GS/OsdShowVersion") ?: this.osd.osdShowVersion,
                osdShowSettings = boolAt("EmuCore/GS/OsdShowSettings") ?: this.osd.osdShowSettings,
                osdShowInputs = boolAt("EmuCore/GS/OsdShowInputs") ?: this.osd.osdShowInputs,
            ),
        )
    }

    /** Upstream-style per-game export (mirrors PCSX2's FullscreenUI): write the keys that
     *  differ from [global] into the game's gamesettings/<serial>_<CRC>.ini, so the on-disk
     *  layer is sparse and portable (a later global tweak still reaches the game for keys it
     *  didn't override). Reuses applyTo's exact field→key mapping via [emitSink]: the global
     *  pass captures a baseline, the effective pass writes the diff.
     *
     *  Also written, even where they equal global: every key the game database contends that
     *  [claimsFor]'s own settings set, and the keys of the database entries switched off for it
     *  (GameDbOverrides). Presence in this file is how the core tells a per-game choice from an
     *  inherited one, and only a per-game choice outranks the database. Pass [claimsFor]
     *  whenever the game is known; without it the file carries only the diff.
     *
     *  With a running VM the target is the current game; with no VM, [serial]'s existing file.
     *  No-op when there is neither. */
    fun writeGameSettingsIni(global: Settings, serial: String? = null, claimsFor: String? = serial) {
        synchronized(gameIniExportLock) {
            // With a running VM the target is the current game (gameIniBeginWrite). With no VM — a
            // per-game Reset done from the library — pass [serial] to locate the file directly;
            // false there means no stale override file exists, so there is nothing to rewrite.
            val began = if (serial == null) NativeApp.gameIniBeginWrite()
                        else NativeApp.gameIniBeginWriteForSerial(serial)
            if (!began) return
            streamGameSettingsIni(global, claimsFor)
            NativeApp.gameIniCommitWrite()
        }
    }

    /** [writeGameSettingsIni] for a game about to boot. Its file cannot be named yet, because the
     *  name carries the disc CRC, so the native side holds the result and writes it just before
     *  the core loads the file (VMManager::UpdateGameSettingsLayer). Without this, a game whose
     *  settings were only ever changed from the library had no file at boot, and the game
     *  database overwrote every one of those settings it also sets. */
    fun stageGameSettingsIni(global: Settings, serial: String) {
        synchronized(gameIniExportLock) {
            if (!NativeApp.gameIniBeginStage(serial)) return
            streamGameSettingsIni(global, serial)
            NativeApp.gameIniCommitWrite()
        }
    }

    private fun streamGameSettingsIni(global: Settings, claimsFor: String?) {
        // applyTo early-returns before the live pokes/commit while a sink is set, so neither pass
        // touches the VM.
        val baseline = global.emittedKeys()
        val effective = emittedKeys()
        // What outranks the GameDB is key presence in the game layer
        // (ComputePerGameOverrides), so VU1's group is written even where its values match
        // global's...
        val forced = HashSet<String>()
        if (cpu.vu1ClampMode != global.cpu.vu1ClampMode)
            listOf("vu1Overflow", "vu1ExtraOverflow", "vu1SignOverflow", "vu1ExactMode")
                .mapTo(forced) { "EmuCore/CPU/Recompiler/$it" }
        // ...and so is everything the game's own settings and switched-off entries claim.
        val claims = runCatching { GameDbOverrides.claimsFor(claimsFor, this, global, effective) }
            .getOrDefault(GameDbOverrides.Claims.NONE)
        forced.addAll(claims.keys)
        for ((id, value) in effective) {
            if (baseline[id] == value && id !in forced) continue
            val cut = id.lastIndexOf('/')
            NativeApp.gameIniPut(id.substring(0, cut), id.substring(cut + 1), value)
        }
        // Some switched-off entries have no setting in this app (the EE division rounding mode,
        // for one), so nothing above wrote their key. The native side fills those in.
        for (id in claims.switchedOffKeys) {
            val cut = id.lastIndexOf('/')
            NativeApp.gameIniClaim(id.substring(0, cut), id.substring(cut + 1))
        }
    }

    /** Every key [applyTo] persists, as "section/key" to value, without writing any of them. */
    internal fun emittedKeys(): LinkedHashMap<String, String> {
        val out = LinkedHashMap<String, String>()
        val outer = emitSink
        emitSink = { section, key, _, value -> out["$section/$key"] = value }
        try {
            applyTo()
        } finally {
            emitSink = outer
        }
        return out
    }

    /** Writes every EmuCore/GS key (display + renderer + hardware/upscaling
     *  fixes) into the native BASE settings layer. Pure persistence — no live
     *  pokes, no commit. Shared by [applyTo] (cold start / restart) and
     *  [applyGsLive] (running VM). Keep the key list in sync with
     *  Pcsx2Config::GSOptions::LoadSave. */
    private fun writeGsToNative() {
        val aspectRatioName = when (output.aspectRatio.coerceIn(0, 8)) {
            0 -> "Stretch"
            2 -> "4:3"
            3 -> "16:9"
            4 -> "10:7"
            5 -> "21:9"
            6 -> "20:9"
            7 -> "19.5:9"
            8 -> "Custom"
            else -> "Auto 4:3/3:2"
        }
        put("EmuCore/GS", "AspectRatio", "string", aspectRatioName)
        val fmvAspectRatioName = when (output.fmvAspectRatio.coerceIn(0, 8)) {
            1 -> "Auto 4:3/3:2"
            2 -> "4:3"
            3 -> "16:9"
            4 -> "10:7"
            5 -> "21:9"
            6 -> "20:9"
            7 -> "19.5:9"
            8 -> "Custom"
            else -> "Off"
        }
        put("EmuCore/GS", "FMVAspectRatioSwitch", "string", fmvAspectRatioName)
        put("EmuCore/GS", "CustomAspectRatio", "float", output.customAspectRatio.coerceIn(0.5f, 5.0f).toString())
        put("EmuCore/GS", "deinterlace_mode", "int", output.deinterlaceMode.coerceIn(0, 9).toString())
        put("EmuCore/GS", "FramerateNTSC", "float", output.framerateNtsc.toString())
        put("EmuCore/GS", "FrameratePAL", "float", output.frameratePal.toString())
        put("EmuCore/GS", "hw_mipmap", "bool", graphics.hwMipmap.toString())
        put("EmuCore/GS", "accurate_blending_unit", "int", graphics.accurateBlendingUnit.toString())
        put("EmuCore/GS", "filter", "int", graphics.textureFiltering.toString())
        put("EmuCore/GS", "linear_present_mode", "int", graphics.displayBilinear.coerceIn(0, 2).toString())
        put("EmuCore/GS", "texture_preloading", "int", graphics.texturePreloading.toString())
        // Upper bound MUST match the highest GSHardwareDownloadMode value (now 5 = Asynchronous).
        // This clamp silently swallowed anything above it, so a new mode would have looked like it
        // simply did nothing — the same failure shape that cost hours on the DEV9 hunt.
        put("EmuCore/GS", "HWDownloadMode", "int", graphics.hardwareDownloadMode.coerceIn(0, 5).toString())
        put("EmuCore/GS", "TVShader", "int", graphics.tvShader.coerceIn(0, 7).toString())
        put("EmuCore/GS", "ShadeBoost", "bool", graphics.shadeBoost.toString())
        put("EmuCore/GS", "ShadeBoost_Brightness", "int", graphics.shadeBoostBrightness.coerceIn(1, 100).toString())
        put("EmuCore/GS", "ShadeBoost_Contrast", "int", graphics.shadeBoostContrast.coerceIn(1, 100).toString())
        put("EmuCore/GS", "ShadeBoost_Saturation", "int", graphics.shadeBoostSaturation.coerceIn(1, 100).toString())
        put("EmuCore/GS", "ShadeBoost_Gamma", "int", graphics.shadeBoostGamma.coerceIn(1, 100).toString())
        put("EmuCore/GS", "fxaa", "bool", graphics.fxaa.toString())
        put("EmuCore/GS", "ShaderChainEnabled", "bool", graphics.shaderChainEnabled.toString())
        put("EmuCore/GS", "ShaderChainPreset", "string", graphics.shaderChainPreset)
        put("EmuCore/GS", "LsfgEnabled", "bool", graphics.lsfgEnabled.toString())
        put("EmuCore/GS", "LsfgMultiplier", "int", graphics.lsfgMultiplier.toString())
        put("EmuCore/GS", "LsfgDllPath", "string", graphics.lsfgDllPath)
        put("EmuCore/GS", "LsfgPerformance", "bool", graphics.lsfgPerformance.toString())
        // Clamped to the same 25..100 the native side enforces. A value outside it would be
        // coerced there anyway, and the two disagreeing is how a slider ends up looking stuck.
        put("EmuCore/GS", "LsfgFlowScale", "int", graphics.lsfgFlowScale.coerceIn(25, 100).toString())
        put("EmuCore/GS", "LsfgTargetRate", "int", graphics.lsfgTargetRate.coerceIn(0, 1000).toString())
        // Parameter overrides, as one opaque JSON blob. Nothing in emucore reads this key —
        // there is no GSConfig field behind it, and the live values reach the renderer via
        // the push below, not through here. It is written so the map survives the same
        // round-trips every other field gets: settings export/import, and the reused-folder
        // recovery that rebuilds prefs from the INI after a fresh install.
        put("EmuCore/GS", "ShaderChainParams", "string", shaderChainParamsToJson(graphics.shaderChainParams).toString())
        // The actual live apply. Only the CURRENT preset's values are pushed (the rest are
        // kept for when the user picks those presets again), and only the overrides — a
        // parameter left out keeps what the chain has, which for the freshly built or
        // rebuilt chain this runs against is the author's initial. Resets are handled by
        // the UI's own pushEffective, which sends initials explicitly to a live chain.
        // Skipped under emitSink: an export has no renderer to push to.
        if (emitSink == null)
            ShaderParams.push(graphics.shaderChainPreset, graphics.shaderChainParams[graphics.shaderChainPreset].orEmpty())
        put("EmuCore/GS", "CASMode", "int", graphics.casMode.coerceIn(0, 2).toString())
        put("EmuCore/GS", "CASSharpness", "int", graphics.casSharpness.coerceIn(0, 100).toString())
        // Upper bound is the HIGHEST enum value, not the count of options this UI shows —
        // clamping to the visible choices would silently rewrite the top one back to Off. This
        // bound has to move every time the core enum grows, which is exactly the trap it was
        // written to warn about: it was still UPSCALER_FSR1 when SGSR was added.
        put("EmuCore/GS", "Upscaler", "int", graphics.upscaler.coerceIn(UPSCALER_OFF, UPSCALER_SGSR_EDGE).toString())
        put("EmuCore/GS", "FSRSharpness", "int", graphics.fsrSharpness.coerceIn(0, 100).toString())
        put("EmuCore/GS", "SGSRSharpness", "int", graphics.sgsrSharpness.coerceIn(0, 200).toString())
        put("EmuCore/GS", "LoadTextureReplacements", "bool", graphics.loadTextureReplacements.toString())
        put("EmuCore/GS", "LoadTextureReplacementsAsync", "bool", graphics.loadTextureReplacementsAsync.toString())
        put("EmuCore/GS", "PrecacheTextureReplacements", "bool", graphics.precacheTextureReplacements.toString())
        put("EmuCore/GS", "DumpReplaceableTextures", "bool", graphics.dumpReplaceableTextures.toString())
        put("EmuCore/GS", "OsdShowTextureReplacements", "bool", graphics.osdShowTextureReplacements.toString())
        put("EmuCore/GS", "OsdShowFPS", "bool", osd.osdShowFps.toString())
        put("EmuCore/GS", "OsdScale", "int", osd.osdScale.coerceIn(25, 500).toString())
        put("EmuCore/GS", "OsdColor", "int", (osd.osdColor and 0xFFFFFF).toString())
        put("EmuCore/GS", "OsdPerformancePos", "int", osd.osdPosition.coerceIn(0, 9).toString())
        put("EmuCore/GS", "VsyncEnable", "bool", display.vsyncEnable.toString())
        put("EmuCore/GS", "OsdShowVPS", "bool", osd.osdShowVps.toString())
        put("EmuCore/GS", "OsdShowSpeed", "bool", osd.osdShowSpeed.toString())
        put("EmuCore/GS", "OsdShowCPU", "bool", osd.osdShowCpu.toString())
        put("EmuCore/GS", "OsdShowGPU", "bool", osd.osdShowGpu.toString())
        put("EmuCore/GS", "OsdShowResolution", "bool", osd.osdShowResolution.toString())
        put("EmuCore/GS", "OsdShowGSStats", "bool", osd.osdShowGsStats.toString())
        put("EmuCore/GS", "OsdShowFrameTimes", "bool", osd.osdShowFrameTimes.toString())
        put("EmuCore/GS", "OsdShowHardwareInfo", "bool", osd.osdShowHardwareInfo.toString())
        put("EmuCore/GS", "OsdMessagesPos", "int", if (osd.osdShowMessages) "1" else "0")
        put("EmuCore/GS", "OsdShowGPUStats", "bool", osd.osdShowGpuStats.toString())
        put("EmuCore/GS", "OsdShowVersion", "bool", osd.osdShowVersion.toString())
        put("EmuCore/GS", "OsdShowSettings", "bool", osd.osdShowSettings.toString())
        put("EmuCore/GS", "OsdShowInputs", "bool", osd.osdShowInputs.toString())
        // Display / PCRTC fixes (not gated by the UserHacks master).
        put("EmuCore/GS", "pcrtc_offsets", "bool", display.screenOffsets.toString())
        put("EmuCore/GS", "pcrtc_overscan", "bool", display.showOverscan.toString())
        put("EmuCore/GS", "pcrtc_antiblur", "bool", display.antiBlur.toString())
        put("EmuCore/GS", "disable_interlace_offset", "bool", display.disableInterlaceOffset.toString())
        put("EmuCore/GS", "SyncToHostRefreshRate", "bool", display.syncToHostRefresh.toString())
        put("EmuCore/GS", "DisableFramebufferFetch", "bool", display.disableFramebufferFetch.toString())
        put("EmuCore/GS", "HWROV", "bool", display.hwRov.toString())
        put("EmuCore/GS", "HWAA1", "bool", display.hwAa1.toString())
        put("EmuCore/GS", "CoalesceRenderPasses", "bool", display.coalesceRenderPasses.toString())
        put("EmuCore/GS", "ForceMaliFramebufferFetch", "bool", display.forceMaliFbFetch.toString())
        // Parity write (native reads the ARMSX2_ANGLE_EGL_LIBRARY env var set by
        // MainActivityRuntime.applyAngleEnv, not this key) — kept so the config file
        // reflects the toggle.
        put("EmuCore/GS", "AndroidUseAngleOpenGL", "bool", display.useAngleOpenGL.toString())
        put("EmuCore/GS", "OverrideTextureBarriers", "int", display.overrideTextureBarriers.coerceIn(-1, 1).toString())
        put("EmuCore/GS", "GSBackThreadMode", "int", display.gsBackThreadMode.coerceIn(0, 3).toString())
        put("EmuCore/GS", "DisableVertexShaderExpand", "bool", display.disableVertexShaderExpand.toString())
        put("EmuCore/GS", "UseBlitSwapChain", "bool", display.useBlitSwapChain.toString())
        put("EmuCore/GS", "DisableShaderCache", "bool", display.disableShaderCache.toString())
        put("EmuCore/GS", "HWAccurateAlphaTest", "bool", display.hwAccurateAlphaTest.toString())
        put("EmuCore/GS", "UserHacks_DrawBuffering", "bool", hwFixes.drawBuffering.toString())
        put("EmuCore/GS", "HWSpinGPUForReadbacks", "bool", hwFixes.spinGpuReadbacks.toString())
        put("EmuCore/GS", "HWSpinCPUForReadbacks", "bool", hwFixes.spinCpuReadbacks.toString())
        put("EmuCore/GS", "IntegerScaling", "bool", hwFixes.integerScaling.toString())
        // Display zoom (#383) overrides the manual crops while active: trim every edge by the
        // same fraction so the picture scales up without distortion. Nominal 640x448 native
        // frame; the zoom factor is what matters visually, so an approximate frame size is fine.
        // (1 - 100/Z)/2 is the per-edge fraction that leaves 1/Z of the image visible, centred.
        val zoom = hwFixes.displayZoom.coerceIn(100, 150)
        val zCropX = if (zoom > 100) ((640.0 * (1.0 - 100.0 / zoom)) / 2.0).toInt() else -1
        val zCropY = if (zoom > 100) ((448.0 * (1.0 - 100.0 / zoom)) / 2.0).toInt() else -1
        val effLeft = if (zCropX >= 0) zCropX else hwFixes.cropLeft
        val effRight = if (zCropX >= 0) zCropX else hwFixes.cropRight
        val effTop = if (zCropY >= 0) zCropY else hwFixes.cropTop
        val effBottom = if (zCropY >= 0) zCropY else hwFixes.cropBottom
        put("EmuCore/GS", "CropLeft", "int", effLeft.coerceIn(0, 640).toString())
        put("EmuCore/GS", "CropTop", "int", effTop.coerceIn(0, 640).toString())
        put("EmuCore/GS", "CropRight", "int", effRight.coerceIn(0, 640).toString())
        put("EmuCore/GS", "CropBottom", "int", effBottom.coerceIn(0, 640).toString())
        put("EmuCore/GS", "dithering_ps2", "int", hwFixes.dithering.coerceIn(0, 3).toString())
        put("EmuCore/GS", "VsyncQueueSize", "int", hwFixes.vsyncQueueSize.coerceIn(0, 3).toString())
        put("EmuCore/GS", "autoflush_sw", "bool", output.autoFlushSw.toString())
        put("EmuCore/GS", "mipmap", "bool", output.mipmapSw.toString())
        put("EmuCore/GS", "extrathreads", "int", output.swThreads.coerceIn(0, 10).toString())
        put("EmuCore/GS", "extrathreads_height", "int", output.swThreadsHeight.coerceIn(0, 8).toString())
        // Skip-draw is a UserHack (gated by the master toggle below).
        put("EmuCore/GS", "UserHacks_SkipDraw_Start", "int", hwFixes.skipDrawStart.coerceAtLeast(0).toString())
        put("EmuCore/GS", "UserHacks_SkipDraw_End", "int", hwFixes.skipDrawEnd.coerceAtLeast(0).toString())
        // Master hardware-fixes toggle. Auto-enables when ANY individual hack is
        // non-default so the user doesn't have to flip it; PCSX2 masks every
        // UserHacks_* key when this is off (GSOptions::MaskUserHacks).
        put("EmuCore/GS", "UserHacks", "bool", anyUserHackEnabled().toString())
        put("EmuCore/GS", "UserHacks_AutoFlushLevel", "int", hwFixes.autoFlush.coerceIn(0, 2).toString())
        put("EmuCore/GS", "UserHacks_HalfPixelOffset", "int", hwFixes.halfPixelOffset.coerceIn(0, 5).toString())
        put("EmuCore/GS", "UserHacks_Limit24BitDepth", "int", hwFixes.limit24BitDepth.coerceIn(0, 2).toString())
        put("EmuCore/GS", "UserHacks_TextureInsideRt", "int", hwFixes.textureInsideRt.coerceIn(0, 2).toString())
        put("EmuCore/GS", "UserHacks_native_scaling", "int", hwFixes.nativeScaling.coerceIn(0, 4).toString())
        put("EmuCore/GS", "UserHacks_round_sprite_offset", "int", hwFixes.roundSprite.coerceIn(0, 2).toString())
        put("EmuCore/GS", "UserHacks_BilinearHack", "int", hwFixes.bilinearUpscale.coerceIn(0, 3).toString())
        put("EmuCore/GS", "UserHacks_GPUTargetCLUTMode", "int", hwFixes.gpuTargetClut.coerceIn(0, 2).toString())
        put("EmuCore/GS", "UserHacks_CPUSpriteRenderBW", "int", hwFixes.cpuSpriteRenderBw.coerceIn(0, 3).toString())
        put("EmuCore/GS", "UserHacks_CPUSpriteRenderLevel", "int", hwFixes.cpuSpriteRenderLevel.coerceIn(0, 5).toString())
        put("EmuCore/GS", "UserHacks_CPUCLUTRender", "int", hwFixes.cpuClutRender.coerceIn(0, 2).toString())
        // Upscaling fixes (parity additions)
        put("EmuCore/GS", "UserHacks_align_sprite_X", "bool", hwFixes.alignSprite.toString())
        put("EmuCore/GS", "UserHacks_merge_pp_sprite", "bool", hwFixes.mergeSprite.toString())
        put("EmuCore/GS", "UserHacks_ForceEvenSpritePosition", "bool", hwFixes.forceEvenSpritePosition.toString())
        put("EmuCore/GS", "UserHacks_NativePaletteDraw", "bool", hwFixes.unscaledPaletteDraw.toString())
        put("EmuCore/GS", "UserHacks_TCOffsetX", "int", hwFixes.textureOffsetX.coerceIn(0, 10000).toString())
        put("EmuCore/GS", "UserHacks_TCOffsetY", "int", hwFixes.textureOffsetY.coerceIn(0, 10000).toString())
        // Hardware fixes (parity additions)
        put("EmuCore/GS", "paltex", "bool", hwFixes.gpuPaletteConversion.toString())
        put("EmuCore/GS", "UserHacks_CPU_FB_Conversion", "bool", hwFixes.cpuFramebufferConversion.toString())
        put("EmuCore/GS", "UserHacks_ReadTCOnClose", "bool", hwFixes.readTargetsWhenClosing.toString())
        put("EmuCore/GS", "UserHacks_DisableDepthSupport", "bool", hwFixes.disableDepthEmulation.toString())
        put("EmuCore/GS", "UserHacks_DisablePartialInvalidation", "bool", hwFixes.disablePartialInvalidation.toString())
        put("EmuCore/GS", "UserHacks_Disable_Safe_Features", "bool", hwFixes.disableSafeFeatures.toString())
        put("EmuCore/GS", "UserHacks_DisableRenderFixes", "bool", hwFixes.disableRenderFixes.toString())
        put("EmuCore/GS", "preload_frame_with_gs_data", "bool", hwFixes.preloadFrameData.toString())
        put("EmuCore/GS", "UserHacks_EstimateTextureRegion", "bool", hwFixes.estimateTextureRegion.toString())
        put("EmuCore/GS", "TriFilter", "int", hwFixes.triFilter.toString())
        put("EmuCore/GS", "MaxAnisotropy", "int", hwFixes.maxAnisotropy.toString())
        val gpuProfileStr = when (hwFixes.gpuProfile) {
            1 -> "mali"
            2 -> "adreno"
            3 -> "powervr"
            4 -> "xclipse"
            else -> "auto"
        }
        put("EmuCore/GS", "AndroidGpuProfileOverride", "string", gpuProfileStr)
    }

    /** True when any hardware/upscaling fix is non-default — used to auto-enable
     *  the UserHacks master so individual hacks aren't silently masked off. */
    internal fun anyUserHackEnabled(): Boolean =
        hwFixes.manualUserHacks ||
            hwFixes.autoFlush != 0 || hwFixes.halfPixelOffset != 0 || hwFixes.limit24BitDepth != 0 ||
            hwFixes.textureInsideRt != 0 || hwFixes.nativeScaling != 0 || hwFixes.roundSprite != 0 ||
            hwFixes.bilinearUpscale != 0 || hwFixes.gpuTargetClut != 0 || hwFixes.cpuSpriteRenderBw != 0 ||
            hwFixes.cpuSpriteRenderLevel != 0 || hwFixes.cpuClutRender != 0 ||
            hwFixes.textureOffsetX != 0 || hwFixes.textureOffsetY != 0 ||
            hwFixes.alignSprite || hwFixes.mergeSprite || hwFixes.forceEvenSpritePosition || hwFixes.unscaledPaletteDraw ||
            hwFixes.gpuPaletteConversion || hwFixes.cpuFramebufferConversion || hwFixes.readTargetsWhenClosing ||
            hwFixes.disableDepthEmulation || hwFixes.disablePartialInvalidation || hwFixes.disableSafeFeatures ||
            hwFixes.disableRenderFixes || hwFixes.preloadFrameData || hwFixes.estimateTextureRegion || hwFixes.drawBuffering ||
            hwFixes.skipDrawStart != 0 || hwFixes.skipDrawEnd != 0

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
    // NOTE: FramerateNTSC/PAL are intentionally NOT here — the generic GS live
    // reconfigure (applyGSSettingsLive) doesn't recompute the vsync target. They
    // get their OWN live path instead: applySafeLiveDelta routes a framerate change
    // to LiveGsApplyQueue.applyFramerate → NativeApp.applyFramerateLive, which parks
    // the VM and recomputes vsync. Keeping them out of here avoids a redundant
    // (and park-free, thus ineffective) GS reconfigure for a framerate-only edit.
    fun gsDiffersFrom(other: Settings): Boolean =
        output.deinterlaceMode != other.output.deinterlaceMode ||
            graphics.textureFiltering != other.graphics.textureFiltering ||
            graphics.displayBilinear != other.graphics.displayBilinear ||
            graphics.texturePreloading != other.graphics.texturePreloading ||
            graphics.hardwareDownloadMode != other.graphics.hardwareDownloadMode ||
            graphics.tvShader != other.graphics.tvShader ||
            graphics.shadeBoost != other.graphics.shadeBoost ||
            graphics.shadeBoostBrightness != other.graphics.shadeBoostBrightness ||
            graphics.shadeBoostContrast != other.graphics.shadeBoostContrast ||
            graphics.shadeBoostSaturation != other.graphics.shadeBoostSaturation ||
            graphics.shadeBoostGamma != other.graphics.shadeBoostGamma ||
            graphics.fxaa != other.graphics.fxaa ||
            graphics.lsfgEnabled != other.graphics.lsfgEnabled ||
            graphics.lsfgMultiplier != other.graphics.lsfgMultiplier ||
            graphics.lsfgDllPath != other.graphics.lsfgDllPath ||
            graphics.lsfgPerformance != other.graphics.lsfgPerformance ||
            graphics.lsfgFlowScale != other.graphics.lsfgFlowScale ||
            graphics.lsfgTargetRate != other.graphics.lsfgTargetRate ||
            osd.osdPosition != other.osd.osdPosition ||
            graphics.casMode != other.graphics.casMode ||
            graphics.casSharpness != other.graphics.casSharpness ||
            graphics.upscaler != other.graphics.upscaler ||
            graphics.fsrSharpness != other.graphics.fsrSharpness ||
            graphics.sgsrSharpness != other.graphics.sgsrSharpness ||
            graphics.accurateBlendingUnit != other.graphics.accurateBlendingUnit ||
            graphics.hwMipmap != other.graphics.hwMipmap ||
            hwFixes.triFilter != other.hwFixes.triFilter ||
            hwFixes.maxAnisotropy != other.hwFixes.maxAnisotropy ||
            hwFixes.manualUserHacks != other.hwFixes.manualUserHacks ||
            hwFixes.autoFlush != other.hwFixes.autoFlush ||
            hwFixes.halfPixelOffset != other.hwFixes.halfPixelOffset ||
            hwFixes.limit24BitDepth != other.hwFixes.limit24BitDepth ||
            hwFixes.textureInsideRt != other.hwFixes.textureInsideRt ||
            hwFixes.nativeScaling != other.hwFixes.nativeScaling ||
            hwFixes.roundSprite != other.hwFixes.roundSprite ||
            hwFixes.bilinearUpscale != other.hwFixes.bilinearUpscale ||
            hwFixes.gpuTargetClut != other.hwFixes.gpuTargetClut ||
            hwFixes.cpuSpriteRenderBw != other.hwFixes.cpuSpriteRenderBw ||
            hwFixes.cpuSpriteRenderLevel != other.hwFixes.cpuSpriteRenderLevel ||
            hwFixes.cpuClutRender != other.hwFixes.cpuClutRender ||
            hwFixes.alignSprite != other.hwFixes.alignSprite ||
            hwFixes.mergeSprite != other.hwFixes.mergeSprite ||
            hwFixes.forceEvenSpritePosition != other.hwFixes.forceEvenSpritePosition ||
            hwFixes.unscaledPaletteDraw != other.hwFixes.unscaledPaletteDraw ||
            hwFixes.textureOffsetX != other.hwFixes.textureOffsetX ||
            hwFixes.textureOffsetY != other.hwFixes.textureOffsetY ||
            hwFixes.gpuPaletteConversion != other.hwFixes.gpuPaletteConversion ||
            hwFixes.cpuFramebufferConversion != other.hwFixes.cpuFramebufferConversion ||
            hwFixes.readTargetsWhenClosing != other.hwFixes.readTargetsWhenClosing ||
            hwFixes.disableDepthEmulation != other.hwFixes.disableDepthEmulation ||
            hwFixes.disablePartialInvalidation != other.hwFixes.disablePartialInvalidation ||
            hwFixes.disableSafeFeatures != other.hwFixes.disableSafeFeatures ||
            hwFixes.disableRenderFixes != other.hwFixes.disableRenderFixes ||
            hwFixes.preloadFrameData != other.hwFixes.preloadFrameData ||
            hwFixes.estimateTextureRegion != other.hwFixes.estimateTextureRegion ||
            display.hwAccurateAlphaTest != other.display.hwAccurateAlphaTest ||
            hwFixes.drawBuffering != other.hwFixes.drawBuffering ||
            // Texture-replacement toggles: without these here the in-game "Load Texture
            // Packs" switch only wrote the base layer (setSetting) and never fired the
            // live GS reconfigure, so a just-imported pack didn't appear until the next
            // game boot. Including them routes through applyGSSettingsLive → GSUpdateConfig
            // → GSTextureReplacements reload/purge, so the pack loads immediately.
            graphics.loadTextureReplacements != other.graphics.loadTextureReplacements ||
            graphics.loadTextureReplacementsAsync != other.graphics.loadTextureReplacementsAsync ||
            graphics.precacheTextureReplacements != other.graphics.precacheTextureReplacements ||
            graphics.dumpReplaceableTextures != other.graphics.dumpReplaceableTextures ||
            graphics.osdShowTextureReplacements != other.graphics.osdShowTextureReplacements

    fun toJson(): JSONObject = JSONObject().apply {
        put("eeCycleRate", cpu.eeCycleRate)
        put("eeCycleSkip", cpu.eeCycleSkip)
        put("eeClampMode", cpu.eeClampMode)
        put("vuClampMode", cpu.vuClampMode)
        put("vu1ClampMode", cpu.vu1ClampMode)
        put("mtvu", cpu.mtvu)
        put("vu1Instant", cpu.vu1Instant)
        put("vuFlagHack", cpu.vuFlagHack)
        put("fastCDVD", cpu.fastCDVD)
        put("intcStat", cpu.intcStat)
        put("waitLoop", cpu.waitLoop)
        put("vuNeonFusions", cpu.vuNeonFusions)
        put("vuDeferredWrites", cpu.vuDeferredWrites)
        put("vuSkipStallSim", cpu.vuSkipStallSim)
        put("frameLimitEnable", frameLimit.frameLimitEnable)
        put("nominalSpeedPercent", frameLimit.nominalSpeedPercent)
        put("fpsLimit", frameLimit.fpsLimit)
        put("frameSkip", frameLimit.frameSkip)
        put("audioVolume", audio.audioVolume)
        put("audioMuted", audio.audioMuted)
        put("audioSwapChannels", audio.audioSwapChannels)
        put("audioTimeStretch", audio.audioTimeStretch)
        put("audioBufferMs", audio.audioBufferMs)
        put("audioOutputLatencyMs", audio.audioOutputLatencyMs)
        put("audioFastForwardVolume", audio.audioFastForwardVolume)
        put("spu2NeonReverb", audio.spu2NeonReverb)
        put("audioOpenSLES", audio.audioOpenSLES)
        put("spu2LightweightMix", audio.spu2LightweightMix)
        put("renderer", output.renderer)
        put("upscaleFloat", output.upscaleFloat.toDouble())
        put("customDriverId", output.customDriverId)
        put("orientation", output.orientation)
        put("portraitRenderTop", output.portraitRenderTop)
        put("landscapeRenderTop", output.landscapeRenderTop)
        put("autoProgressiveScan", output.autoProgressiveScan)
        put("affinityMode", output.affinityMode)
        put("framerateNtsc", output.framerateNtsc.toDouble())
        put("frameratePal", output.frameratePal.toDouble())
        put("enablePatches", emuCore.enablePatches)
        put("enableCheats", emuCore.enableCheats)
        put("enableWideScreenPatches", emuCore.enableWideScreenPatches)
        put("enableNoInterlacingPatches", emuCore.enableNoInterlacingPatches)
        put("enableFastBoot", emuCore.enableFastBoot)
        put("hostFs", emuCore.hostFs)
        put("achievementsEnabled", emuCore.achievements.enabled)
        put("achievementsHardcore", emuCore.achievements.hardcore)
        put("achievementsNotifications", emuCore.achievements.notifications)
        put("achievementsLeaderboardNotifications", emuCore.achievements.leaderboardNotifications)
        put("achievementsOverlays", emuCore.achievements.overlays)
        put("achievementsLbOverlays", emuCore.achievements.lbOverlays)
        put("achievementsSoundEffects", emuCore.achievements.soundEffects)
        put("achievementsEncoreMode", emuCore.achievements.encoreMode)
        put("achievementsSpectatorMode", emuCore.achievements.spectatorMode)
        put("achievementsUnofficialTestMode", emuCore.achievements.unofficialTestMode)
        put("achievementsNotificationsDuration", emuCore.achievements.notificationsDuration)
        put("achievementsLeaderboardsDuration", emuCore.achievements.leaderboardsDuration)
        put("achievementsNotificationPosition", emuCore.achievements.notificationPosition)
        put("achievementsOverlayPosition", emuCore.achievements.overlayPosition)
        put("achievementsNotificationScale", emuCore.achievements.notificationScale)
        put("pineEnabled", emuCore.pineEnabled)
        put("pineSlot", emuCore.pineSlot)
        put("enableGameFixes", emuCore.enableGameFixes)
        put("gamefixSoftwareRendererFmv", emuCore.gamefixSoftwareRendererFmv)
        put("gamefixSkipMpeg", emuCore.gamefixSkipMpeg)
        put("gamefixEETiming", emuCore.gamefixEETiming)
        put("gamefixInstantDma", emuCore.gamefixInstantDma)
        put("gamefixBlitInternalFps", emuCore.gamefixBlitInternalFps)
        put("gamefixOphFlag", emuCore.gamefixOphFlag)
        put("gamefixGifFifo", emuCore.gamefixGifFifo)
        put("gamefixDmaBusy", emuCore.gamefixDmaBusy)
        put("gamefixVif1Stall", emuCore.gamefixVif1Stall)
        put("gamefixIbit", emuCore.gamefixIbit)
        put("gamefixFullVu0Sync", emuCore.gamefixFullVu0Sync)
        put("gamefixVuAddSub", emuCore.gamefixVuAddSub)
        put("gamefixVuOverflow", emuCore.gamefixVuOverflow)
        put("gamefixXgkick", emuCore.gamefixXgkick)
        put("gamefixGoemonTlb", emuCore.gamefixGoemonTlb)
        put("gamefixVuSync", emuCore.gamefixVuSync)
        put("skipDuplicateFrames", emuCore.skipDuplicateFrames)
        put("eeFpuRoundMode", emuCore.eeFpuRoundMode)
        put("vu0RoundMode", emuCore.vu0RoundMode)
        put("vu1RoundMode", emuCore.vu1RoundMode)
        put("screenOffsets", display.screenOffsets)
        put("showOverscan", display.showOverscan)
        put("antiBlur", display.antiBlur)
        put("disableInterlaceOffset", display.disableInterlaceOffset)
        put("syncToHostRefresh", display.syncToHostRefresh)
        put("disableFramebufferFetch", display.disableFramebufferFetch)
        put("hwRov", display.hwRov)
        put("hwAa1", display.hwAa1)
        put("coalesceRenderPasses", display.coalesceRenderPasses)
        put("forceMaliFbFetch", display.forceMaliFbFetch)
        put("useAngleOpenGL", display.useAngleOpenGL)
        put("overrideTextureBarriers", display.overrideTextureBarriers)
        put("gsBackThreadMode", display.gsBackThreadMode)
        put("disableVertexShaderExpand", display.disableVertexShaderExpand)
        put("useBlitSwapChain", display.useBlitSwapChain)
        put("disableShaderCache", display.disableShaderCache)
        put("hwAccurateAlphaTest", display.hwAccurateAlphaTest)
        put("skipDrawStart", hwFixes.skipDrawStart)
        put("skipDrawEnd", hwFixes.skipDrawEnd)
        put("spinGpuReadbacks", hwFixes.spinGpuReadbacks)
        put("spinCpuReadbacks", hwFixes.spinCpuReadbacks)
        put("integerScaling", hwFixes.integerScaling)
        put("cropLeft", hwFixes.cropLeft)
        put("displayZoom", hwFixes.displayZoom)
        put("cropTop", hwFixes.cropTop)
        put("cropRight", hwFixes.cropRight)
        put("cropBottom", hwFixes.cropBottom)
        put("dithering", hwFixes.dithering)
        put("vsyncQueueSize", hwFixes.vsyncQueueSize)
        put("hwScaler", output.hwScaler)
        put("screenResOverride", output.screenResOverride)
        put("autoFlushSw", output.autoFlushSw)
        put("mipmapSw", output.mipmapSw)
        put("swThreads", output.swThreads)
        put("swThreadsHeight", output.swThreadsHeight)
        put("aspectRatio", output.aspectRatio)
        put("fmvAspectRatio", output.fmvAspectRatio)
        put("customAspectRatio", output.customAspectRatio.toDouble())
        put("deinterlaceMode", output.deinterlaceMode)
        put("dev9EthEnable", network.dev9EthEnable)
        put("dev9EthApi", network.dev9EthApi)
        put("localLinkHost", network.localLinkHost)
        put("localLinkAddress", network.localLinkAddress)
        put("localLinkPort", network.localLinkPort)
        put("localLinkPeerId", network.localLinkPeerId)
        put("localLinkRoomCode", network.localLinkRoomCode)
        put("dev9EthDevice", network.dev9EthDevice)
        put("dev9EthLogDhcp", network.dev9EthLogDhcp)
        put("dev9EthLogDns", network.dev9EthLogDns)
        put("dev9InterceptDhcp", network.dev9InterceptDhcp)
        put("dev9Ps2Ip", network.dev9Ps2Ip)
        put("dev9Mask", network.dev9Mask)
        put("dev9Gateway", network.dev9Gateway)
        put("dev9Dns1", network.dev9Dns1)
        put("dev9Dns2", network.dev9Dns2)
        put("dev9AutoMask", network.dev9AutoMask)
        put("dev9AutoGateway", network.dev9AutoGateway)
        put("dev9ModeDns1", network.dev9ModeDns1)
        put("dev9ModeDns2", network.dev9ModeDns2)
        put("dev9EthHosts", JSONArray().apply {
            network.dev9EthHosts.forEach { h ->
                put(JSONObject().apply {
                    put("url", h.url)
                    put("ip", h.ip)
                    put("enabled", h.enabled)
                })
            }
        })
        put("dev9HddEnable", network.dev9HddEnable)
        put("dev9HddFile", network.dev9HddFile)
        put("memoryCardSlot1Enabled", system.memoryCardSlot1Enabled)
        put("memoryCardSlot1Filename", system.memoryCardSlot1Filename)
        put("biosFilename", system.biosFilename)
        put("memoryCardSlot2Enabled", system.memoryCardSlot2Enabled)
        put("memoryCardSlot2Filename", system.memoryCardSlot2Filename)
        put("usbKeyboard", system.usbKeyboard)
        put("recEE", cpu.recEE)
        put("recIOP", cpu.recIOP)
        put("recVU0", cpu.recVU0)
        put("recVU1", cpu.recVU1)
        put("enableFastmem", cpu.enableFastmem)
        put("vu1InlineFmacStall", cpu.vu1InlineFmacStall)
        put("vu1CrossBlockPState", cpu.vu1CrossBlockPState)
        put("vu1InlineDrainTestPipes", cpu.vu1InlineDrainTestPipes)
        put("vu1FmacInstanceRouting", cpu.vu1FmacInstanceRouting)
        put("hwMipmap", graphics.hwMipmap)
        put("accurateBlendingUnit", graphics.accurateBlendingUnit)
        put("textureFiltering", graphics.textureFiltering)
        put("displayBilinear", graphics.displayBilinear)
        put("texturePreloading", graphics.texturePreloading)
        put("hardwareDownloadMode", graphics.hardwareDownloadMode)
        put("tvShader", graphics.tvShader)
        put("shadeBoost", graphics.shadeBoost)
        put("shadeBoostBrightness", graphics.shadeBoostBrightness)
        put("shadeBoostContrast", graphics.shadeBoostContrast)
        put("shadeBoostSaturation", graphics.shadeBoostSaturation)
        put("shadeBoostGamma", graphics.shadeBoostGamma)
        put("fxaa", graphics.fxaa)
        put("shaderChainEnabled", graphics.shaderChainEnabled)
        put("shaderChainPreset", graphics.shaderChainPreset)
        put("shaderChainParams", shaderChainParamsToJson(graphics.shaderChainParams))
        // These five were missing from the JSON round-trip entirely, which IS the persistence
        // format — so every LSFG choice, the imported DLL path included, was thrown away the
        // moment the app was restarted.
        put("lsfgEnabled", graphics.lsfgEnabled)
        put("lsfgMultiplier", graphics.lsfgMultiplier)
        put("lsfgDllPath", graphics.lsfgDllPath)
        put("lsfgPerformance", graphics.lsfgPerformance)
        put("lsfgFlowScale", graphics.lsfgFlowScale)
        put("lsfgTargetRate", graphics.lsfgTargetRate)
        put("casMode", graphics.casMode)
        put("casSharpness", graphics.casSharpness)
        put("upscaler", graphics.upscaler)
        put("fsrSharpness", graphics.fsrSharpness)
        put("sgsrSharpness", graphics.sgsrSharpness)
        put("loadTextureReplacements", graphics.loadTextureReplacements)
        put("loadTextureReplacementsAsync", graphics.loadTextureReplacementsAsync)
        put("precacheTextureReplacements", graphics.precacheTextureReplacements)
        put("dumpReplaceableTextures", graphics.dumpReplaceableTextures)
        put("osdShowTextureReplacements", graphics.osdShowTextureReplacements)
        put("osdShowFps", osd.osdShowFps)
        put("osdScale", osd.osdScale)
        put("osdColor", osd.osdColor)
        put("osdPosition", osd.osdPosition)
        put("vsyncEnable", display.vsyncEnable)
        put("osdShowVps", osd.osdShowVps)
        put("osdShowSpeed", osd.osdShowSpeed)
        put("osdShowCpu", osd.osdShowCpu)
        put("osdShowGpu", osd.osdShowGpu)
        put("osdShowResolution", osd.osdShowResolution)
        put("osdShowGsStats", osd.osdShowGsStats)
        put("osdShowFrameTimes", osd.osdShowFrameTimes)
        put("osdShowHardwareInfo", osd.osdShowHardwareInfo)
        put("osdShowMessages", osd.osdShowMessages)
        put("osdShowGpuStats", osd.osdShowGpuStats)
        put("osdShowVersion", osd.osdShowVersion)
        put("osdShowSettings", osd.osdShowSettings)
        put("osdShowInputs", osd.osdShowInputs)
        put("autoFlush", hwFixes.autoFlush)
        put("halfPixelOffset", hwFixes.halfPixelOffset)
        put("limit24BitDepth", hwFixes.limit24BitDepth)
        put("manualUserHacks", hwFixes.manualUserHacks)
        put("textureInsideRt", hwFixes.textureInsideRt)
        put("nativeScaling", hwFixes.nativeScaling)
        put("roundSprite", hwFixes.roundSprite)
        put("bilinearUpscale", hwFixes.bilinearUpscale)
        put("gpuTargetClut", hwFixes.gpuTargetClut)
        put("cpuSpriteRenderBw", hwFixes.cpuSpriteRenderBw)
        put("cpuSpriteRenderLevel", hwFixes.cpuSpriteRenderLevel)
        put("alignSprite", hwFixes.alignSprite)
        put("mergeSprite", hwFixes.mergeSprite)
        put("forceEvenSpritePosition", hwFixes.forceEvenSpritePosition)
        put("unscaledPaletteDraw", hwFixes.unscaledPaletteDraw)
        put("textureOffsetX", hwFixes.textureOffsetX)
        put("textureOffsetY", hwFixes.textureOffsetY)
        put("gpuPaletteConversion", hwFixes.gpuPaletteConversion)
        put("cpuFramebufferConversion", hwFixes.cpuFramebufferConversion)
        put("readTargetsWhenClosing", hwFixes.readTargetsWhenClosing)
        put("disableDepthEmulation", hwFixes.disableDepthEmulation)
        put("disablePartialInvalidation", hwFixes.disablePartialInvalidation)
        put("disableSafeFeatures", hwFixes.disableSafeFeatures)
        put("disableRenderFixes", hwFixes.disableRenderFixes)
        put("preloadFrameData", hwFixes.preloadFrameData)
        put("estimateTextureRegion", hwFixes.estimateTextureRegion)
        put("drawBuffering", hwFixes.drawBuffering)
        put("cpuClutRender", hwFixes.cpuClutRender)
        put("triFilter", hwFixes.triFilter)
        put("maxAnisotropy", hwFixes.maxAnisotropy)
        put("gpuProfile", hwFixes.gpuProfile)
    }

    companion object {
        /** When non-null, [put] routes persisted-key emits here instead of the
         *  native base layer. Set transiently by [writeGameSettingsIni] (via
         *  [emittedKeys]) to capture the key set for the sparse per-game INI export
         *  without touching the base layer or re-poking the running VM.
         *
         *  Per thread. A launch applies settings for real on the VM launch thread while the
         *  UI thread can be running an export, and a shared sink would divert that launch's
         *  writes into the export: the game would boot without them. */
        private val emitSinkLocal = ThreadLocal<((String, String, String, String) -> Unit)?>()

        /** Every RetroAchievements field, by JSON key: what the RetroAchievements screen clears to
         *  put one game back on the global settings. */
        val ACHIEVEMENTS_KEYS: List<String> = listOf("achievementsEnabled", "achievementsHardcore", "achievementsNotifications", "achievementsLeaderboardNotifications", "achievementsOverlays", "achievementsLbOverlays", "achievementsSoundEffects", "achievementsEncoreMode", "achievementsSpectatorMode", "achievementsUnofficialTestMode", "achievementsNotificationsDuration", "achievementsLeaderboardsDuration", "achievementsNotificationPosition", "achievementsOverlayPosition", "achievementsNotificationScale")

        /** One per-game INI export at a time. The native side streams it through a single
         *  begin/put/commit state, and a game launch stages one on the launch thread while the UI
         *  thread can be saving settings. */
        private val gameIniExportLock = Any()

        @JvmStatic
        internal var emitSink: ((String, String, String, String) -> Unit)?
            get() = emitSinkLocal.get()
            set(value) = emitSinkLocal.set(value)

        /** [upscaler] values, straight from the core's GSUpscaler. Named because 1 is Apple's
         *  MetalFX and never appears in this UI, so FSR1's value (2) does NOT line up with its
         *  position in any Android picker — writing the picker index would select MetalFX. */
        const val UPSCALER_OFF = 0
        const val UPSCALER_FSR1 = 2
        const val UPSCALER_SGSR = 3
        const val UPSCALER_SGSR_EDGE = 4

        /** One-tap "Low-End" performance snapshot applied on top of [base].
         *  Only cheap, safe-for-most levers that already exist as fields:
         *    - accurate_blending_unit = Minimum (0)   — cheapest blend path
         *    - internal resolution   = 1x (native)     — biggest GPU win
         *    - hw mipmap off, GPU palette conversion off — drop optional GPU work
         *    - texture preloading    = Partial (1)      — lower upload stalls
         *    - HW ROV off                                — never a win on tilers
         *    - EE cycle skip         = 1                 — mild CPU headroom
         *    - MTVU                   = device-aware      — only when >= 6 cores
         *  [mtvu] is passed in (from [com.armsx2.DeviceTier.mtvuDefault]) rather
         *  than read here so config/ stays free of Android context deps.
         *  NOTE: intentionally does NOT touch CAS — there is no CAS Settings
         *  field wired in this build. */
        fun lowEndPreset(base: Settings, mtvu: Boolean): Settings = base.copy(
            cpu = base.cpu.copy(
                eeCycleSkip = 1,
                mtvu = mtvu,
            ),
            display = base.display.copy(
                hwRov = false,              // ROV off
            ),
            hwFixes = base.hwFixes.copy(
                gpuPaletteConversion = false,
            ),
            output = base.output.copy(
                upscaleFloat = 1.0f,        // native resolution
            ),
            graphics = base.graphics.copy(
                accurateBlendingUnit = 0,   // Minimum
                hwMipmap = false,           // mipmap off
                texturePreloading = 1,      // Partial
            ),
        )

        /** Lenient parse — missing keys fall back to defaults so old saved
         *  blobs survive when new fields are added. */
        fun fromJson(json: JSONObject): Settings {
            val def = Settings()
            return Settings(
                cpu = CpuSettings(
                    eeCycleRate = json.optInt("eeCycleRate", def.cpu.eeCycleRate),
                    eeCycleSkip = json.optInt("eeCycleSkip", def.cpu.eeCycleSkip),
                    eeClampMode = json.optInt("eeClampMode", def.cpu.eeClampMode),
                    vuClampMode = json.optInt("vuClampMode", def.cpu.vuClampMode),
                    vu1ClampMode = json.optInt("vu1ClampMode", def.cpu.vu1ClampMode),
                    mtvu = json.optBoolean("mtvu", def.cpu.mtvu),
                    vu1Instant = json.optBoolean("vu1Instant", def.cpu.vu1Instant),
                    vuFlagHack = json.optBoolean("vuFlagHack", def.cpu.vuFlagHack),
                    fastCDVD = json.optBoolean("fastCDVD", def.cpu.fastCDVD),
                    intcStat = json.optBoolean("intcStat", def.cpu.intcStat),
                    waitLoop = json.optBoolean("waitLoop", def.cpu.waitLoop),
                    vuNeonFusions = json.optBoolean("vuNeonFusions", def.cpu.vuNeonFusions),
                    vuDeferredWrites = json.optBoolean("vuDeferredWrites", def.cpu.vuDeferredWrites),
                    vuSkipStallSim = json.optBoolean("vuSkipStallSim", def.cpu.vuSkipStallSim),
                    recEE = json.optBoolean("recEE", def.cpu.recEE),
                    recIOP = json.optBoolean("recIOP", def.cpu.recIOP),
                    recVU0 = json.optBoolean("recVU0", def.cpu.recVU0),
                    recVU1 = json.optBoolean("recVU1", def.cpu.recVU1),
                    enableFastmem = json.optBoolean("enableFastmem", def.cpu.enableFastmem),
                    useMacEE = true,
                    useMacIOP = true,
                    useMacVU0 = true,
                    useMacVU1 = true,
                    vu1InlineFmacStall = json.optBoolean("vu1InlineFmacStall", def.cpu.vu1InlineFmacStall),
                    vu1CrossBlockPState = json.optBoolean("vu1CrossBlockPState", def.cpu.vu1CrossBlockPState),
                    vu1InlineDrainTestPipes = json.optBoolean("vu1InlineDrainTestPipes", def.cpu.vu1InlineDrainTestPipes),
                    vu1FmacInstanceRouting = json.optBoolean("vu1FmacInstanceRouting", def.cpu.vu1FmacInstanceRouting),
                ),
                frameLimit = FrameLimitSettings(
                    frameLimitEnable = json.optBoolean("frameLimitEnable", def.frameLimit.frameLimitEnable),
                    nominalSpeedPercent = json.optInt("nominalSpeedPercent", def.frameLimit.nominalSpeedPercent),
                    fpsLimit = json.optInt("fpsLimit", def.frameLimit.fpsLimit),
                    frameSkip = json.optInt("frameSkip", def.frameLimit.frameSkip),
                ),
                audio = AudioSettings(
                    audioVolume = json.optInt("audioVolume", def.audio.audioVolume),
                    audioMuted = json.optBoolean("audioMuted", def.audio.audioMuted),
                    audioSwapChannels = json.optBoolean("audioSwapChannels", def.audio.audioSwapChannels),
                    audioTimeStretch = json.optBoolean("audioTimeStretch", def.audio.audioTimeStretch),
                    audioBufferMs = json.optInt("audioBufferMs", def.audio.audioBufferMs),
                    audioOutputLatencyMs = json.optInt("audioOutputLatencyMs", def.audio.audioOutputLatencyMs),
                    audioFastForwardVolume = json.optInt("audioFastForwardVolume", def.audio.audioFastForwardVolume),
                    spu2NeonReverb = json.optBoolean("spu2NeonReverb", def.audio.spu2NeonReverb),
                    audioOpenSLES = json.optBoolean("audioOpenSLES", def.audio.audioOpenSLES),
                    spu2LightweightMix = json.optBoolean("spu2LightweightMix", def.audio.spu2LightweightMix),
                ),
                emuCore = EmuCoreSettings(
                    enablePatches = json.optBoolean("enablePatches", def.emuCore.enablePatches),
                    enableCheats = json.optBoolean("enableCheats", def.emuCore.enableCheats),
                    enableWideScreenPatches = json.optBoolean("enableWideScreenPatches", def.emuCore.enableWideScreenPatches),
                    enableNoInterlacingPatches = json.optBoolean("enableNoInterlacingPatches", def.emuCore.enableNoInterlacingPatches),
                    enableFastBoot = json.optBoolean("enableFastBoot", def.emuCore.enableFastBoot),
                    hostFs = json.optBoolean("hostFs", def.emuCore.hostFs),
                    achievements = AchievementsSettings(
                    enabled = json.optBoolean("achievementsEnabled", def.emuCore.achievements.enabled),
                    hardcore = json.optBoolean("achievementsHardcore", def.emuCore.achievements.hardcore),
                    notifications = json.optBoolean("achievementsNotifications", def.emuCore.achievements.notifications),
                    leaderboardNotifications = json.optBoolean("achievementsLeaderboardNotifications", def.emuCore.achievements.leaderboardNotifications),
                    overlays = json.optBoolean("achievementsOverlays", def.emuCore.achievements.overlays),
                    lbOverlays = json.optBoolean("achievementsLbOverlays", def.emuCore.achievements.lbOverlays),
                    soundEffects = json.optBoolean("achievementsSoundEffects", def.emuCore.achievements.soundEffects),
                    encoreMode = json.optBoolean("achievementsEncoreMode", def.emuCore.achievements.encoreMode),
                    spectatorMode = json.optBoolean("achievementsSpectatorMode", def.emuCore.achievements.spectatorMode),
                    unofficialTestMode = json.optBoolean("achievementsUnofficialTestMode", def.emuCore.achievements.unofficialTestMode),
                    notificationsDuration = json.optInt("achievementsNotificationsDuration", def.emuCore.achievements.notificationsDuration),
                    leaderboardsDuration = json.optInt("achievementsLeaderboardsDuration", def.emuCore.achievements.leaderboardsDuration),
                    notificationPosition = json.optInt("achievementsNotificationPosition", def.emuCore.achievements.notificationPosition),
                    overlayPosition = json.optInt("achievementsOverlayPosition", def.emuCore.achievements.overlayPosition),
                    notificationScale = json.optInt("achievementsNotificationScale", def.emuCore.achievements.notificationScale),
                ),
                    pineEnabled = json.optBoolean("pineEnabled", def.emuCore.pineEnabled),
                    pineSlot = json.optInt("pineSlot", def.emuCore.pineSlot),
                    enableGameFixes = json.optBoolean("enableGameFixes", def.emuCore.enableGameFixes),
                    gamefixSoftwareRendererFmv = json.optBoolean("gamefixSoftwareRendererFmv", def.emuCore.gamefixSoftwareRendererFmv),
                    gamefixSkipMpeg = json.optBoolean("gamefixSkipMpeg", def.emuCore.gamefixSkipMpeg),
                    gamefixEETiming = json.optBoolean("gamefixEETiming", def.emuCore.gamefixEETiming),
                    gamefixInstantDma = json.optBoolean("gamefixInstantDma", def.emuCore.gamefixInstantDma),
                    gamefixBlitInternalFps = json.optBoolean("gamefixBlitInternalFps", def.emuCore.gamefixBlitInternalFps),
                    gamefixOphFlag = json.optBoolean("gamefixOphFlag", def.emuCore.gamefixOphFlag),
                    gamefixGifFifo = json.optBoolean("gamefixGifFifo", def.emuCore.gamefixGifFifo),
                    gamefixDmaBusy = json.optBoolean("gamefixDmaBusy", def.emuCore.gamefixDmaBusy),
                    gamefixVif1Stall = json.optBoolean("gamefixVif1Stall", def.emuCore.gamefixVif1Stall),
                    gamefixIbit = json.optBoolean("gamefixIbit", def.emuCore.gamefixIbit),
                    gamefixFullVu0Sync = json.optBoolean("gamefixFullVu0Sync", def.emuCore.gamefixFullVu0Sync),
                    gamefixVuAddSub = json.optBoolean("gamefixVuAddSub", def.emuCore.gamefixVuAddSub),
                    gamefixVuOverflow = json.optBoolean("gamefixVuOverflow", def.emuCore.gamefixVuOverflow),
                    gamefixXgkick = json.optBoolean("gamefixXgkick", def.emuCore.gamefixXgkick),
                    gamefixGoemonTlb = json.optBoolean("gamefixGoemonTlb", def.emuCore.gamefixGoemonTlb),
                    gamefixVuSync = json.optBoolean("gamefixVuSync", def.emuCore.gamefixVuSync),
                    skipDuplicateFrames = json.optBoolean("skipDuplicateFrames", def.emuCore.skipDuplicateFrames),
                    eeFpuRoundMode = json.optInt("eeFpuRoundMode", def.emuCore.eeFpuRoundMode),
                    vu0RoundMode = json.optInt("vu0RoundMode", def.emuCore.vu0RoundMode),
                    vu1RoundMode = json.optInt("vu1RoundMode", def.emuCore.vu1RoundMode),
                ),
                display = DisplaySettings(
                    screenOffsets = json.optBoolean("screenOffsets", def.display.screenOffsets),
                    showOverscan = json.optBoolean("showOverscan", def.display.showOverscan),
                    antiBlur = json.optBoolean("antiBlur", def.display.antiBlur),
                    disableInterlaceOffset = json.optBoolean("disableInterlaceOffset", def.display.disableInterlaceOffset),
                    syncToHostRefresh = json.optBoolean("syncToHostRefresh", def.display.syncToHostRefresh),
                    disableFramebufferFetch = json.optBoolean("disableFramebufferFetch", def.display.disableFramebufferFetch),
                    hwRov = json.optBoolean("hwRov", def.display.hwRov),
                    hwAa1 = json.optBoolean("hwAa1", def.display.hwAa1),
                    hwAat = false,
                    coalesceRenderPasses = json.optBoolean("coalesceRenderPasses", def.display.coalesceRenderPasses),
                    forceMaliFbFetch = json.optBoolean("forceMaliFbFetch", def.display.forceMaliFbFetch),
                    useAngleOpenGL = json.optBoolean("useAngleOpenGL", def.display.useAngleOpenGL),
                    overrideTextureBarriers = json.optInt("overrideTextureBarriers", def.display.overrideTextureBarriers),
                    gsBackThreadMode = json.optInt("gsBackThreadMode", def.display.gsBackThreadMode),
                    disableVertexShaderExpand = json.optBoolean("disableVertexShaderExpand", def.display.disableVertexShaderExpand),
                    useBlitSwapChain = json.optBoolean("useBlitSwapChain", def.display.useBlitSwapChain),
                    disableShaderCache = json.optBoolean("disableShaderCache", def.display.disableShaderCache),
                    hwAccurateAlphaTest = json.optBoolean(
                    "hwAccurateAlphaTest",
                    json.optBoolean("hwAat", def.display.hwAccurateAlphaTest),
                ),
                    vsyncEnable = json.optBoolean("vsyncEnable", def.display.vsyncEnable),
                ),
                hwFixes = HwFixesSettings(
                    skipDrawStart = json.optInt("skipDrawStart", def.hwFixes.skipDrawStart),
                    skipDrawEnd = json.optInt("skipDrawEnd", def.hwFixes.skipDrawEnd),
                    spinGpuReadbacks = json.optBoolean("spinGpuReadbacks", def.hwFixes.spinGpuReadbacks),
                    spinCpuReadbacks = json.optBoolean("spinCpuReadbacks", def.hwFixes.spinCpuReadbacks),
                    integerScaling = json.optBoolean("integerScaling", def.hwFixes.integerScaling),
                    cropLeft = json.optInt("cropLeft", def.hwFixes.cropLeft),
                    displayZoom = json.optInt("displayZoom", def.hwFixes.displayZoom),
                    cropTop = json.optInt("cropTop", def.hwFixes.cropTop),
                    cropRight = json.optInt("cropRight", def.hwFixes.cropRight),
                    cropBottom = json.optInt("cropBottom", def.hwFixes.cropBottom),
                    dithering = json.optInt("dithering", def.hwFixes.dithering),
                    vsyncQueueSize = json.optInt("vsyncQueueSize", def.hwFixes.vsyncQueueSize),
                    autoFlush = json.optInt("autoFlush", def.hwFixes.autoFlush),
                    halfPixelOffset = json.optInt("halfPixelOffset", def.hwFixes.halfPixelOffset),
                    limit24BitDepth = json.optInt("limit24BitDepth", def.hwFixes.limit24BitDepth),
                    manualUserHacks = json.optBoolean("manualUserHacks", def.hwFixes.manualUserHacks),
                    textureInsideRt = json.optInt("textureInsideRt", def.hwFixes.textureInsideRt),
                    nativeScaling = json.optInt("nativeScaling", def.hwFixes.nativeScaling),
                    roundSprite = json.optInt("roundSprite", def.hwFixes.roundSprite),
                    bilinearUpscale = json.optInt("bilinearUpscale", def.hwFixes.bilinearUpscale),
                    gpuTargetClut = json.optInt("gpuTargetClut", def.hwFixes.gpuTargetClut),
                    cpuSpriteRenderBw = json.optInt("cpuSpriteRenderBw", def.hwFixes.cpuSpriteRenderBw),
                    cpuSpriteRenderLevel = json.optInt("cpuSpriteRenderLevel", def.hwFixes.cpuSpriteRenderLevel),
                    alignSprite = json.optBoolean("alignSprite", def.hwFixes.alignSprite),
                    mergeSprite = json.optBoolean("mergeSprite", def.hwFixes.mergeSprite),
                    forceEvenSpritePosition = json.optBoolean("forceEvenSpritePosition", def.hwFixes.forceEvenSpritePosition),
                    unscaledPaletteDraw = json.optBoolean("unscaledPaletteDraw", def.hwFixes.unscaledPaletteDraw),
                    textureOffsetX = json.optInt("textureOffsetX", def.hwFixes.textureOffsetX),
                    textureOffsetY = json.optInt("textureOffsetY", def.hwFixes.textureOffsetY),
                    gpuPaletteConversion = json.optBoolean("gpuPaletteConversion", def.hwFixes.gpuPaletteConversion),
                    cpuFramebufferConversion = json.optBoolean("cpuFramebufferConversion", def.hwFixes.cpuFramebufferConversion),
                    readTargetsWhenClosing = json.optBoolean("readTargetsWhenClosing", def.hwFixes.readTargetsWhenClosing),
                    disableDepthEmulation = json.optBoolean("disableDepthEmulation", def.hwFixes.disableDepthEmulation),
                    disablePartialInvalidation = json.optBoolean("disablePartialInvalidation", def.hwFixes.disablePartialInvalidation),
                    disableSafeFeatures = json.optBoolean("disableSafeFeatures", def.hwFixes.disableSafeFeatures),
                    disableRenderFixes = json.optBoolean("disableRenderFixes", def.hwFixes.disableRenderFixes),
                    preloadFrameData = json.optBoolean("preloadFrameData", def.hwFixes.preloadFrameData),
                    estimateTextureRegion = json.optBoolean("estimateTextureRegion", def.hwFixes.estimateTextureRegion),
                    drawBuffering = json.optBoolean("drawBuffering", def.hwFixes.drawBuffering),
                    cpuClutRender = json.optInt("cpuClutRender", def.hwFixes.cpuClutRender),
                    triFilter = json.optInt("triFilter", def.hwFixes.triFilter),
                    maxAnisotropy = json.optInt("maxAnisotropy", def.hwFixes.maxAnisotropy),
                    gpuProfile = json.optInt("gpuProfile", def.hwFixes.gpuProfile),
                ),
                output = OutputSettings(
                    renderer = json.optString("renderer", def.output.renderer),
                    upscaleFloat = json.optDouble("upscaleFloat", def.output.upscaleFloat.toDouble()).toFloat(),
                    customDriverId = json.optString("customDriverId", def.output.customDriverId),
                    orientation = json.optInt("orientation", def.output.orientation),
                    portraitRenderTop = json.optBoolean("portraitRenderTop", def.output.portraitRenderTop),
                    landscapeRenderTop = json.optBoolean("landscapeRenderTop", def.output.landscapeRenderTop),
                    autoProgressiveScan = json.optBoolean("autoProgressiveScan", def.output.autoProgressiveScan),
                    affinityMode = json.optInt("affinityMode", def.output.affinityMode),
                    framerateNtsc = json.optDouble("framerateNtsc", def.output.framerateNtsc.toDouble()).toFloat(),
                    frameratePal = json.optDouble("frameratePal", def.output.frameratePal.toDouble()).toFloat(),
                    hwScaler = json.optInt("hwScaler", def.output.hwScaler),
                    screenResOverride = json.optString("screenResOverride", def.output.screenResOverride).ifEmpty { def.output.screenResOverride },
                    autoFlushSw = json.optBoolean("autoFlushSw", def.output.autoFlushSw),
                    mipmapSw = json.optBoolean("mipmapSw", def.output.mipmapSw),
                    swThreads = json.optInt("swThreads", def.output.swThreads),
                    swThreadsHeight = json.optInt("swThreadsHeight", def.output.swThreadsHeight),
                    aspectRatio = json.optInt("aspectRatio", def.output.aspectRatio),
                    fmvAspectRatio = json.optInt("fmvAspectRatio", def.output.fmvAspectRatio),
                    customAspectRatio = json.optDouble("customAspectRatio", def.output.customAspectRatio.toDouble()).toFloat(),
                    deinterlaceMode = json.optInt("deinterlaceMode", def.output.deinterlaceMode),
                ),
                network = NetworkSettings(
                    dev9EthEnable = json.optBoolean("dev9EthEnable", def.network.dev9EthEnable),
                    dev9EthApi = json.optString("dev9EthApi", def.network.dev9EthApi).ifEmpty { def.network.dev9EthApi },
                    localLinkHost = json.optBoolean("localLinkHost", def.network.localLinkHost),
                    localLinkAddress = json.optString("localLinkAddress", def.network.localLinkAddress),
                    localLinkPort = json.optInt("localLinkPort", def.network.localLinkPort),
                    localLinkPeerId = json.optInt("localLinkPeerId", def.network.localLinkPeerId),
                    localLinkRoomCode = json.optString("localLinkRoomCode", def.network.localLinkRoomCode),
                    dev9EthDevice = json.optString("dev9EthDevice", def.network.dev9EthDevice).ifEmpty { def.network.dev9EthDevice },
                    dev9EthLogDhcp = json.optBoolean("dev9EthLogDhcp", def.network.dev9EthLogDhcp),
                    dev9EthLogDns = json.optBoolean("dev9EthLogDns", def.network.dev9EthLogDns),
                    dev9InterceptDhcp = json.optBoolean("dev9InterceptDhcp", def.network.dev9InterceptDhcp),
                    dev9Ps2Ip = json.optString("dev9Ps2Ip", def.network.dev9Ps2Ip).ifEmpty { def.network.dev9Ps2Ip },
                    dev9Mask = json.optString("dev9Mask", def.network.dev9Mask).ifEmpty { def.network.dev9Mask },
                    dev9Gateway = json.optString("dev9Gateway", def.network.dev9Gateway).ifEmpty { def.network.dev9Gateway },
                    dev9Dns1 = json.optString("dev9Dns1", def.network.dev9Dns1).ifEmpty { def.network.dev9Dns1 },
                    dev9Dns2 = json.optString("dev9Dns2", def.network.dev9Dns2).ifEmpty { def.network.dev9Dns2 },
                    dev9AutoMask = json.optBoolean("dev9AutoMask", def.network.dev9AutoMask),
                    dev9AutoGateway = json.optBoolean("dev9AutoGateway", def.network.dev9AutoGateway),
                    dev9ModeDns1 = json.optString("dev9ModeDns1", def.network.dev9ModeDns1).ifEmpty { def.network.dev9ModeDns1 },
                    dev9ModeDns2 = json.optString("dev9ModeDns2", def.network.dev9ModeDns2).ifEmpty { def.network.dev9ModeDns2 },
                    dev9EthHosts = json.optJSONArray("dev9EthHosts")?.let { arr ->
                    (0 until arr.length()).mapNotNull { idx ->
                        arr.optJSONObject(idx)?.let { o ->
                            Dev9HostMapping(
                                url = o.optString("url", ""),
                                ip = o.optString("ip", "0.0.0.0").ifEmpty { "0.0.0.0" },
                                enabled = o.optBoolean("enabled", true),
                            )
                        }
                    }.filter { it.url.isNotBlank() }
                } ?: def.network.dev9EthHosts,
                    dev9HddEnable = json.optBoolean("dev9HddEnable", def.network.dev9HddEnable),
                    dev9HddFile = json.optString("dev9HddFile", def.network.dev9HddFile).ifEmpty { def.network.dev9HddFile },
                ),
                system = SystemSettings(
                    memoryCardSlot1Enabled = json.optBoolean("memoryCardSlot1Enabled", def.system.memoryCardSlot1Enabled),
                    memoryCardSlot1Filename = json.optString("memoryCardSlot1Filename", def.system.memoryCardSlot1Filename).ifEmpty { def.system.memoryCardSlot1Filename },
                    biosFilename = json.optString("biosFilename", def.system.biosFilename),
                    memoryCardSlot2Enabled = json.optBoolean("memoryCardSlot2Enabled", def.system.memoryCardSlot2Enabled),
                    memoryCardSlot2Filename = json.optString("memoryCardSlot2Filename", def.system.memoryCardSlot2Filename).ifEmpty { def.system.memoryCardSlot2Filename },
                    usbKeyboard = json.optBoolean("usbKeyboard", def.system.usbKeyboard),
                ),
                graphics = GraphicsSettings(
                    hwMipmap = json.optBoolean("hwMipmap", def.graphics.hwMipmap),
                    accurateBlendingUnit = json.optInt("accurateBlendingUnit", def.graphics.accurateBlendingUnit),
                    textureFiltering = json.optInt("textureFiltering", def.graphics.textureFiltering),
                    displayBilinear = json.optInt("displayBilinear", def.graphics.displayBilinear),
                    texturePreloading = json.optInt("texturePreloading", def.graphics.texturePreloading),
                    hardwareDownloadMode = json.optInt("hardwareDownloadMode", def.graphics.hardwareDownloadMode),
                    tvShader = json.optInt("tvShader", def.graphics.tvShader),
                    shadeBoost = json.optBoolean("shadeBoost", def.graphics.shadeBoost),
                    shadeBoostBrightness = json.optInt("shadeBoostBrightness", def.graphics.shadeBoostBrightness),
                    shadeBoostContrast = json.optInt("shadeBoostContrast", def.graphics.shadeBoostContrast),
                    shadeBoostSaturation = json.optInt("shadeBoostSaturation", def.graphics.shadeBoostSaturation),
                    shadeBoostGamma = json.optInt("shadeBoostGamma", def.graphics.shadeBoostGamma),
                    fxaa = json.optBoolean("fxaa", def.graphics.fxaa),
                    shaderChainEnabled = json.optBoolean("shaderChainEnabled", def.graphics.shaderChainEnabled),
                    shaderChainPreset = json.optString("shaderChainPreset", def.graphics.shaderChainPreset),
                    shaderChainParams = json.optJSONObject("shaderChainParams")
                    ?.let { shaderChainParamsFromJson(it) } ?: def.graphics.shaderChainParams,
                    lsfgEnabled = json.optBoolean("lsfgEnabled", def.graphics.lsfgEnabled),
                    lsfgMultiplier = json.optInt("lsfgMultiplier", def.graphics.lsfgMultiplier),
                    lsfgDllPath = json.optString("lsfgDllPath", def.graphics.lsfgDllPath),
                    lsfgPerformance = json.optBoolean("lsfgPerformance", def.graphics.lsfgPerformance),
                    lsfgFlowScale = json.optInt("lsfgFlowScale", def.graphics.lsfgFlowScale),
                    lsfgTargetRate = json.optInt("lsfgTargetRate", def.graphics.lsfgTargetRate),
                    casMode = json.optInt("casMode", def.graphics.casMode),
                    casSharpness = json.optInt("casSharpness", def.graphics.casSharpness),
                    upscaler = json.optInt("upscaler", def.graphics.upscaler),
                    fsrSharpness = json.optInt("fsrSharpness", def.graphics.fsrSharpness),
                    sgsrSharpness = json.optInt("sgsrSharpness", def.graphics.sgsrSharpness),
                    loadTextureReplacements = json.optBoolean("loadTextureReplacements", def.graphics.loadTextureReplacements),
                    loadTextureReplacementsAsync = json.optBoolean("loadTextureReplacementsAsync", def.graphics.loadTextureReplacementsAsync),
                    precacheTextureReplacements = json.optBoolean("precacheTextureReplacements", def.graphics.precacheTextureReplacements),
                    dumpReplaceableTextures = json.optBoolean("dumpReplaceableTextures", def.graphics.dumpReplaceableTextures),
                    osdShowTextureReplacements = json.optBoolean("osdShowTextureReplacements", def.graphics.osdShowTextureReplacements),
                ),
                osd = OsdSettings(
                    osdShowFps = json.optBoolean("osdShowFps", def.osd.osdShowFps),
                    osdScale = json.optInt("osdScale", def.osd.osdScale),
                    osdColor = json.optInt("osdColor", def.osd.osdColor),
                    osdPosition = json.optInt("osdPosition", def.osd.osdPosition),
                    osdShowVps = json.optBoolean("osdShowVps", def.osd.osdShowVps),
                    osdShowSpeed = json.optBoolean("osdShowSpeed", def.osd.osdShowSpeed),
                    osdShowCpu = json.optBoolean("osdShowCpu", def.osd.osdShowCpu),
                    osdShowGpu = json.optBoolean("osdShowGpu", def.osd.osdShowGpu),
                    osdShowResolution = json.optBoolean("osdShowResolution", def.osd.osdShowResolution),
                    osdShowGsStats = json.optBoolean("osdShowGsStats", def.osd.osdShowGsStats),
                    osdShowFrameTimes = json.optBoolean("osdShowFrameTimes", def.osd.osdShowFrameTimes),
                    osdShowHardwareInfo = json.optBoolean("osdShowHardwareInfo", def.osd.osdShowHardwareInfo),
                    osdShowMessages = json.optBoolean("osdShowMessages", def.osd.osdShowMessages),
                    osdShowGpuStats = json.optBoolean("osdShowGpuStats", def.osd.osdShowGpuStats),
                    osdShowVersion = json.optBoolean("osdShowVersion", def.osd.osdShowVersion),
                    osdShowSettings = json.optBoolean("osdShowSettings", def.osd.osdShowSettings),
                    osdShowInputs = json.optBoolean("osdShowInputs", def.osd.osdShowInputs),
                ),
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
            if (current.cpu.eeCycleRate         != base.cpu.eeCycleRate)         j.put("eeCycleRate", current.cpu.eeCycleRate)
            if (current.cpu.eeCycleSkip         != base.cpu.eeCycleSkip)         j.put("eeCycleSkip", current.cpu.eeCycleSkip)
            if (current.cpu.eeClampMode         != base.cpu.eeClampMode)         j.put("eeClampMode", current.cpu.eeClampMode)
            if (current.cpu.vuClampMode         != base.cpu.vuClampMode)         j.put("vuClampMode", current.cpu.vuClampMode)
            if (current.cpu.vu1ClampMode        != base.cpu.vu1ClampMode)        j.put("vu1ClampMode", current.cpu.vu1ClampMode)
            if (current.cpu.mtvu                != base.cpu.mtvu)                j.put("mtvu", current.cpu.mtvu)
            if (current.cpu.vu1Instant          != base.cpu.vu1Instant)          j.put("vu1Instant", current.cpu.vu1Instant)
            if (current.cpu.vuFlagHack          != base.cpu.vuFlagHack)          j.put("vuFlagHack", current.cpu.vuFlagHack)
            if (current.cpu.fastCDVD            != base.cpu.fastCDVD)            j.put("fastCDVD", current.cpu.fastCDVD)
            if (current.cpu.intcStat            != base.cpu.intcStat)            j.put("intcStat", current.cpu.intcStat)
            if (current.cpu.waitLoop            != base.cpu.waitLoop)            j.put("waitLoop", current.cpu.waitLoop)
            if (current.cpu.vuNeonFusions       != base.cpu.vuNeonFusions)       j.put("vuNeonFusions", current.cpu.vuNeonFusions)
            if (current.cpu.vuDeferredWrites    != base.cpu.vuDeferredWrites)    j.put("vuDeferredWrites", current.cpu.vuDeferredWrites)
            if (current.cpu.vuSkipStallSim      != base.cpu.vuSkipStallSim)      j.put("vuSkipStallSim", current.cpu.vuSkipStallSim)
            if (current.frameLimit.frameLimitEnable    != base.frameLimit.frameLimitEnable)    j.put("frameLimitEnable", current.frameLimit.frameLimitEnable)
            if (current.frameLimit.nominalSpeedPercent != base.frameLimit.nominalSpeedPercent) j.put("nominalSpeedPercent", current.frameLimit.nominalSpeedPercent)
            if (current.frameLimit.fpsLimit            != base.frameLimit.fpsLimit)            j.put("fpsLimit", current.frameLimit.fpsLimit)
            if (current.frameLimit.frameSkip != base.frameLimit.frameSkip) j.put("frameSkip", current.frameLimit.frameSkip)
            if (current.audio.audioVolume != base.audio.audioVolume) j.put("audioVolume", current.audio.audioVolume)
            if (current.audio.audioMuted != base.audio.audioMuted) j.put("audioMuted", current.audio.audioMuted)
            if (current.audio.audioSwapChannels != base.audio.audioSwapChannels) j.put("audioSwapChannels", current.audio.audioSwapChannels)
            if (current.audio.audioTimeStretch != base.audio.audioTimeStretch) j.put("audioTimeStretch", current.audio.audioTimeStretch)
            if (current.audio.audioBufferMs != base.audio.audioBufferMs) j.put("audioBufferMs", current.audio.audioBufferMs)
            if (current.audio.audioOutputLatencyMs != base.audio.audioOutputLatencyMs) j.put("audioOutputLatencyMs", current.audio.audioOutputLatencyMs)
            if (current.audio.audioFastForwardVolume != base.audio.audioFastForwardVolume) j.put("audioFastForwardVolume", current.audio.audioFastForwardVolume)
            if (current.audio.spu2NeonReverb != base.audio.spu2NeonReverb) j.put("spu2NeonReverb", current.audio.spu2NeonReverb)
            if (current.audio.audioOpenSLES != base.audio.audioOpenSLES) j.put("audioOpenSLES", current.audio.audioOpenSLES)
            if (current.audio.spu2LightweightMix != base.audio.spu2LightweightMix) j.put("spu2LightweightMix", current.audio.spu2LightweightMix)
            if (current.output.renderer != base.output.renderer) j.put("renderer", current.output.renderer)
            if (current.output.upscaleFloat != base.output.upscaleFloat) j.put("upscaleFloat", current.output.upscaleFloat.toDouble())
            if (current.output.customDriverId != base.output.customDriverId) j.put("customDriverId", current.output.customDriverId)
            if (current.output.orientation != base.output.orientation) j.put("orientation", current.output.orientation)
            if (current.output.portraitRenderTop != base.output.portraitRenderTop) j.put("portraitRenderTop", current.output.portraitRenderTop)
            if (current.output.landscapeRenderTop != base.output.landscapeRenderTop) j.put("landscapeRenderTop", current.output.landscapeRenderTop)
            if (current.output.autoProgressiveScan != base.output.autoProgressiveScan) j.put("autoProgressiveScan", current.output.autoProgressiveScan)
            if (current.output.affinityMode != base.output.affinityMode) j.put("affinityMode", current.output.affinityMode)
            if (current.output.framerateNtsc != base.output.framerateNtsc) j.put("framerateNtsc", current.output.framerateNtsc.toDouble())
            if (current.output.frameratePal != base.output.frameratePal) j.put("frameratePal", current.output.frameratePal.toDouble())
            if (current.emuCore.enablePatches != base.emuCore.enablePatches) j.put("enablePatches", current.emuCore.enablePatches)
            if (current.emuCore.enableCheats != base.emuCore.enableCheats) j.put("enableCheats", current.emuCore.enableCheats)
            if (current.emuCore.enableWideScreenPatches != base.emuCore.enableWideScreenPatches) j.put("enableWideScreenPatches", current.emuCore.enableWideScreenPatches)
            if (current.emuCore.enableNoInterlacingPatches != base.emuCore.enableNoInterlacingPatches) j.put("enableNoInterlacingPatches", current.emuCore.enableNoInterlacingPatches)
            if (current.emuCore.enableFastBoot != base.emuCore.enableFastBoot) j.put("enableFastBoot", current.emuCore.enableFastBoot)
            if (current.emuCore.hostFs != base.emuCore.hostFs) j.put("hostFs", current.emuCore.hostFs)
            if (current.emuCore.achievements.enabled != base.emuCore.achievements.enabled) j.put("achievementsEnabled", current.emuCore.achievements.enabled)
            if (current.emuCore.achievements.hardcore != base.emuCore.achievements.hardcore) j.put("achievementsHardcore", current.emuCore.achievements.hardcore)
            if (current.emuCore.achievements.notifications != base.emuCore.achievements.notifications) j.put("achievementsNotifications", current.emuCore.achievements.notifications)
            if (current.emuCore.achievements.leaderboardNotifications != base.emuCore.achievements.leaderboardNotifications) j.put("achievementsLeaderboardNotifications", current.emuCore.achievements.leaderboardNotifications)
            if (current.emuCore.achievements.overlays != base.emuCore.achievements.overlays) j.put("achievementsOverlays", current.emuCore.achievements.overlays)
            if (current.emuCore.achievements.lbOverlays != base.emuCore.achievements.lbOverlays) j.put("achievementsLbOverlays", current.emuCore.achievements.lbOverlays)
            if (current.emuCore.achievements.soundEffects != base.emuCore.achievements.soundEffects) j.put("achievementsSoundEffects", current.emuCore.achievements.soundEffects)
            if (current.emuCore.achievements.encoreMode != base.emuCore.achievements.encoreMode) j.put("achievementsEncoreMode", current.emuCore.achievements.encoreMode)
            if (current.emuCore.achievements.spectatorMode != base.emuCore.achievements.spectatorMode) j.put("achievementsSpectatorMode", current.emuCore.achievements.spectatorMode)
            if (current.emuCore.achievements.unofficialTestMode != base.emuCore.achievements.unofficialTestMode) j.put("achievementsUnofficialTestMode", current.emuCore.achievements.unofficialTestMode)
            if (current.emuCore.achievements.notificationsDuration != base.emuCore.achievements.notificationsDuration) j.put("achievementsNotificationsDuration", current.emuCore.achievements.notificationsDuration)
            if (current.emuCore.achievements.leaderboardsDuration != base.emuCore.achievements.leaderboardsDuration) j.put("achievementsLeaderboardsDuration", current.emuCore.achievements.leaderboardsDuration)
            if (current.emuCore.achievements.notificationPosition != base.emuCore.achievements.notificationPosition) j.put("achievementsNotificationPosition", current.emuCore.achievements.notificationPosition)
            if (current.emuCore.achievements.overlayPosition != base.emuCore.achievements.overlayPosition) j.put("achievementsOverlayPosition", current.emuCore.achievements.overlayPosition)
            if (current.emuCore.achievements.notificationScale != base.emuCore.achievements.notificationScale) j.put("achievementsNotificationScale", current.emuCore.achievements.notificationScale)
            if (current.emuCore.enableGameFixes != base.emuCore.enableGameFixes) j.put("enableGameFixes", current.emuCore.enableGameFixes)
            if (current.emuCore.gamefixSoftwareRendererFmv != base.emuCore.gamefixSoftwareRendererFmv) j.put("gamefixSoftwareRendererFmv", current.emuCore.gamefixSoftwareRendererFmv)
            if (current.emuCore.gamefixSkipMpeg != base.emuCore.gamefixSkipMpeg) j.put("gamefixSkipMpeg", current.emuCore.gamefixSkipMpeg)
            if (current.emuCore.gamefixEETiming != base.emuCore.gamefixEETiming) j.put("gamefixEETiming", current.emuCore.gamefixEETiming)
            if (current.emuCore.gamefixInstantDma != base.emuCore.gamefixInstantDma) j.put("gamefixInstantDma", current.emuCore.gamefixInstantDma)
            if (current.emuCore.gamefixBlitInternalFps != base.emuCore.gamefixBlitInternalFps) j.put("gamefixBlitInternalFps", current.emuCore.gamefixBlitInternalFps)
            if (current.emuCore.gamefixOphFlag       != base.emuCore.gamefixOphFlag)       j.put("gamefixOphFlag", current.emuCore.gamefixOphFlag)
            if (current.emuCore.gamefixGifFifo       != base.emuCore.gamefixGifFifo)       j.put("gamefixGifFifo", current.emuCore.gamefixGifFifo)
            if (current.emuCore.gamefixDmaBusy       != base.emuCore.gamefixDmaBusy)       j.put("gamefixDmaBusy", current.emuCore.gamefixDmaBusy)
            if (current.emuCore.gamefixVif1Stall     != base.emuCore.gamefixVif1Stall)     j.put("gamefixVif1Stall", current.emuCore.gamefixVif1Stall)
            if (current.emuCore.gamefixIbit          != base.emuCore.gamefixIbit)          j.put("gamefixIbit", current.emuCore.gamefixIbit)
            if (current.emuCore.gamefixFullVu0Sync   != base.emuCore.gamefixFullVu0Sync)   j.put("gamefixFullVu0Sync", current.emuCore.gamefixFullVu0Sync)
            if (current.emuCore.gamefixVuAddSub      != base.emuCore.gamefixVuAddSub)      j.put("gamefixVuAddSub", current.emuCore.gamefixVuAddSub)
            if (current.emuCore.gamefixVuOverflow    != base.emuCore.gamefixVuOverflow)    j.put("gamefixVuOverflow", current.emuCore.gamefixVuOverflow)
            if (current.emuCore.gamefixXgkick        != base.emuCore.gamefixXgkick)        j.put("gamefixXgkick", current.emuCore.gamefixXgkick)
            if (current.emuCore.gamefixGoemonTlb     != base.emuCore.gamefixGoemonTlb)     j.put("gamefixGoemonTlb", current.emuCore.gamefixGoemonTlb)
            if (current.emuCore.gamefixVuSync        != base.emuCore.gamefixVuSync)        j.put("gamefixVuSync", current.emuCore.gamefixVuSync)
            if (current.emuCore.skipDuplicateFrames  != base.emuCore.skipDuplicateFrames)  j.put("skipDuplicateFrames", current.emuCore.skipDuplicateFrames)
            if (current.emuCore.eeFpuRoundMode       != base.emuCore.eeFpuRoundMode)       j.put("eeFpuRoundMode", current.emuCore.eeFpuRoundMode)
            if (current.emuCore.vu0RoundMode         != base.emuCore.vu0RoundMode)         j.put("vu0RoundMode", current.emuCore.vu0RoundMode)
            if (current.emuCore.vu1RoundMode         != base.emuCore.vu1RoundMode)         j.put("vu1RoundMode", current.emuCore.vu1RoundMode)
            if (current.display.screenOffsets        != base.display.screenOffsets)        j.put("screenOffsets", current.display.screenOffsets)
            if (current.display.showOverscan         != base.display.showOverscan)         j.put("showOverscan", current.display.showOverscan)
            if (current.display.antiBlur             != base.display.antiBlur)             j.put("antiBlur", current.display.antiBlur)
            if (current.display.disableInterlaceOffset != base.display.disableInterlaceOffset) j.put("disableInterlaceOffset", current.display.disableInterlaceOffset)
            if (current.display.syncToHostRefresh    != base.display.syncToHostRefresh)    j.put("syncToHostRefresh", current.display.syncToHostRefresh)
            if (current.display.disableFramebufferFetch != base.display.disableFramebufferFetch) j.put("disableFramebufferFetch", current.display.disableFramebufferFetch)
            if (current.display.hwRov != base.display.hwRov) j.put("hwRov", current.display.hwRov)
            if (current.display.hwAa1 != base.display.hwAa1) j.put("hwAa1", current.display.hwAa1)
            if (current.display.coalesceRenderPasses != base.display.coalesceRenderPasses) j.put("coalesceRenderPasses", current.display.coalesceRenderPasses)
            if (current.display.forceMaliFbFetch != base.display.forceMaliFbFetch) j.put("forceMaliFbFetch", current.display.forceMaliFbFetch)
            if (current.display.useAngleOpenGL != base.display.useAngleOpenGL) j.put("useAngleOpenGL", current.display.useAngleOpenGL)
            if (current.display.overrideTextureBarriers != base.display.overrideTextureBarriers) j.put("overrideTextureBarriers", current.display.overrideTextureBarriers)
            if (current.display.gsBackThreadMode != base.display.gsBackThreadMode) j.put("gsBackThreadMode", current.display.gsBackThreadMode)
            if (current.display.disableVertexShaderExpand != base.display.disableVertexShaderExpand) j.put("disableVertexShaderExpand", current.display.disableVertexShaderExpand)
            if (current.display.useBlitSwapChain     != base.display.useBlitSwapChain)     j.put("useBlitSwapChain", current.display.useBlitSwapChain)
            if (current.display.disableShaderCache   != base.display.disableShaderCache)   j.put("disableShaderCache", current.display.disableShaderCache)
            if (current.display.hwAccurateAlphaTest  != base.display.hwAccurateAlphaTest)  j.put("hwAccurateAlphaTest", current.display.hwAccurateAlphaTest)
            if (current.hwFixes.skipDrawStart        != base.hwFixes.skipDrawStart)        j.put("skipDrawStart", current.hwFixes.skipDrawStart)
            if (current.hwFixes.skipDrawEnd          != base.hwFixes.skipDrawEnd)          j.put("skipDrawEnd", current.hwFixes.skipDrawEnd)
            if (current.hwFixes.spinGpuReadbacks     != base.hwFixes.spinGpuReadbacks)     j.put("spinGpuReadbacks", current.hwFixes.spinGpuReadbacks)
            if (current.hwFixes.spinCpuReadbacks     != base.hwFixes.spinCpuReadbacks)     j.put("spinCpuReadbacks", current.hwFixes.spinCpuReadbacks)
            if (current.hwFixes.integerScaling       != base.hwFixes.integerScaling)       j.put("integerScaling", current.hwFixes.integerScaling)
            if (current.hwFixes.cropLeft             != base.hwFixes.cropLeft)             j.put("cropLeft", current.hwFixes.cropLeft)
            if (current.hwFixes.displayZoom          != base.hwFixes.displayZoom)          j.put("displayZoom", current.hwFixes.displayZoom)
            if (current.hwFixes.cropTop              != base.hwFixes.cropTop)              j.put("cropTop", current.hwFixes.cropTop)
            if (current.hwFixes.cropRight            != base.hwFixes.cropRight)            j.put("cropRight", current.hwFixes.cropRight)
            if (current.hwFixes.cropBottom           != base.hwFixes.cropBottom)           j.put("cropBottom", current.hwFixes.cropBottom)
            if (current.hwFixes.dithering            != base.hwFixes.dithering)            j.put("dithering", current.hwFixes.dithering)
            if (current.hwFixes.vsyncQueueSize       != base.hwFixes.vsyncQueueSize)       j.put("vsyncQueueSize", current.hwFixes.vsyncQueueSize)
            if (current.output.hwScaler             != base.output.hwScaler)             j.put("hwScaler", current.output.hwScaler)
            if (current.output.screenResOverride    != base.output.screenResOverride)    j.put("screenResOverride", current.output.screenResOverride)
            if (current.output.autoFlushSw          != base.output.autoFlushSw)          j.put("autoFlushSw", current.output.autoFlushSw)
            if (current.output.mipmapSw             != base.output.mipmapSw)             j.put("mipmapSw", current.output.mipmapSw)
            if (current.output.swThreads            != base.output.swThreads)            j.put("swThreads", current.output.swThreads)
            if (current.output.swThreadsHeight      != base.output.swThreadsHeight)      j.put("swThreadsHeight", current.output.swThreadsHeight)
            if (current.output.aspectRatio         != base.output.aspectRatio)         j.put("aspectRatio", current.output.aspectRatio)
            if (current.output.fmvAspectRatio      != base.output.fmvAspectRatio)      j.put("fmvAspectRatio", current.output.fmvAspectRatio)
            if (current.output.customAspectRatio   != base.output.customAspectRatio)   j.put("customAspectRatio", current.output.customAspectRatio.toDouble())
            if (current.output.deinterlaceMode     != base.output.deinterlaceMode)     j.put("deinterlaceMode", current.output.deinterlaceMode)
            if (current.network.dev9EthEnable       != base.network.dev9EthEnable)       j.put("dev9EthEnable", current.network.dev9EthEnable)
            if (current.network.dev9EthApi          != base.network.dev9EthApi)          j.put("dev9EthApi", current.network.dev9EthApi)
            if (current.network.localLinkHost != base.network.localLinkHost) j.put("localLinkHost", current.network.localLinkHost)
            if (current.network.localLinkAddress != base.network.localLinkAddress) j.put("localLinkAddress", current.network.localLinkAddress)
            if (current.network.localLinkPort != base.network.localLinkPort) j.put("localLinkPort", current.network.localLinkPort)
            if (current.network.localLinkPeerId != base.network.localLinkPeerId) j.put("localLinkPeerId", current.network.localLinkPeerId)
            if (current.network.localLinkRoomCode != base.network.localLinkRoomCode) j.put("localLinkRoomCode", current.network.localLinkRoomCode)
            if (current.network.dev9EthDevice       != base.network.dev9EthDevice)       j.put("dev9EthDevice", current.network.dev9EthDevice)
            if (current.network.dev9EthLogDhcp      != base.network.dev9EthLogDhcp)      j.put("dev9EthLogDhcp", current.network.dev9EthLogDhcp)
            if (current.network.dev9EthLogDns       != base.network.dev9EthLogDns)       j.put("dev9EthLogDns", current.network.dev9EthLogDns)
            if (current.network.dev9InterceptDhcp   != base.network.dev9InterceptDhcp)   j.put("dev9InterceptDhcp", current.network.dev9InterceptDhcp)
            if (current.network.dev9Ps2Ip           != base.network.dev9Ps2Ip)           j.put("dev9Ps2Ip", current.network.dev9Ps2Ip)
            if (current.network.dev9Mask            != base.network.dev9Mask)            j.put("dev9Mask", current.network.dev9Mask)
            if (current.network.dev9Gateway         != base.network.dev9Gateway)         j.put("dev9Gateway", current.network.dev9Gateway)
            if (current.network.dev9Dns1            != base.network.dev9Dns1)            j.put("dev9Dns1", current.network.dev9Dns1)
            if (current.network.dev9Dns2            != base.network.dev9Dns2)            j.put("dev9Dns2", current.network.dev9Dns2)
            if (current.network.dev9AutoMask        != base.network.dev9AutoMask)        j.put("dev9AutoMask", current.network.dev9AutoMask)
            if (current.network.dev9AutoGateway     != base.network.dev9AutoGateway)     j.put("dev9AutoGateway", current.network.dev9AutoGateway)
            if (current.network.dev9ModeDns1        != base.network.dev9ModeDns1)        j.put("dev9ModeDns1", current.network.dev9ModeDns1)
            if (current.network.dev9ModeDns2        != base.network.dev9ModeDns2)        j.put("dev9ModeDns2", current.network.dev9ModeDns2)
            if (current.network.dev9EthHosts        != base.network.dev9EthHosts) {
                j.put("dev9EthHosts", JSONArray().apply {
                    current.network.dev9EthHosts.forEach { host ->
                        put(JSONObject().apply {
                            put("url", host.url)
                            put("ip", host.ip)
                            put("enabled", host.enabled)
                        })
                    }
                })
            }
            if (current.network.dev9HddEnable       != base.network.dev9HddEnable)       j.put("dev9HddEnable", current.network.dev9HddEnable)
            if (current.network.dev9HddFile         != base.network.dev9HddFile)         j.put("dev9HddFile", current.network.dev9HddFile)
            if (current.system.memoryCardSlot1Enabled != base.system.memoryCardSlot1Enabled) j.put("memoryCardSlot1Enabled", current.system.memoryCardSlot1Enabled)
            if (current.system.memoryCardSlot1Filename != base.system.memoryCardSlot1Filename) j.put("memoryCardSlot1Filename", current.system.memoryCardSlot1Filename)
            if (current.system.biosFilename != base.system.biosFilename) j.put("biosFilename", current.system.biosFilename)
            if (current.system.memoryCardSlot2Enabled != base.system.memoryCardSlot2Enabled) j.put("memoryCardSlot2Enabled", current.system.memoryCardSlot2Enabled)
            if (current.system.memoryCardSlot2Filename != base.system.memoryCardSlot2Filename) j.put("memoryCardSlot2Filename", current.system.memoryCardSlot2Filename)
            if (current.system.usbKeyboard         != base.system.usbKeyboard)         j.put("usbKeyboard", current.system.usbKeyboard)
            if (current.cpu.recEE               != base.cpu.recEE)               j.put("recEE", current.cpu.recEE)
            if (current.cpu.recIOP              != base.cpu.recIOP)              j.put("recIOP", current.cpu.recIOP)
            if (current.cpu.recVU0              != base.cpu.recVU0)              j.put("recVU0", current.cpu.recVU0)
            if (current.cpu.recVU1              != base.cpu.recVU1)              j.put("recVU1", current.cpu.recVU1)
            if (current.cpu.enableFastmem       != base.cpu.enableFastmem)       j.put("enableFastmem", current.cpu.enableFastmem)
            if (current.cpu.vu1InlineFmacStall  != base.cpu.vu1InlineFmacStall)  j.put("vu1InlineFmacStall", current.cpu.vu1InlineFmacStall)
            if (current.cpu.vu1CrossBlockPState != base.cpu.vu1CrossBlockPState) j.put("vu1CrossBlockPState", current.cpu.vu1CrossBlockPState)
            if (current.cpu.vu1InlineDrainTestPipes != base.cpu.vu1InlineDrainTestPipes) j.put("vu1InlineDrainTestPipes", current.cpu.vu1InlineDrainTestPipes)
            if (current.cpu.vu1FmacInstanceRouting != base.cpu.vu1FmacInstanceRouting) j.put("vu1FmacInstanceRouting", current.cpu.vu1FmacInstanceRouting)
            if (current.graphics.hwMipmap            != base.graphics.hwMipmap)            j.put("hwMipmap", current.graphics.hwMipmap)
            if (current.graphics.accurateBlendingUnit!= base.graphics.accurateBlendingUnit)j.put("accurateBlendingUnit", current.graphics.accurateBlendingUnit)
            if (current.graphics.textureFiltering    != base.graphics.textureFiltering)    j.put("textureFiltering", current.graphics.textureFiltering)
            if (current.graphics.displayBilinear     != base.graphics.displayBilinear)     j.put("displayBilinear", current.graphics.displayBilinear)
            if (current.graphics.texturePreloading   != base.graphics.texturePreloading)   j.put("texturePreloading", current.graphics.texturePreloading)
            if (current.graphics.hardwareDownloadMode!= base.graphics.hardwareDownloadMode)j.put("hardwareDownloadMode", current.graphics.hardwareDownloadMode)
            if (current.graphics.tvShader            != base.graphics.tvShader)            j.put("tvShader", current.graphics.tvShader)
            if (current.graphics.shadeBoost          != base.graphics.shadeBoost)          j.put("shadeBoost", current.graphics.shadeBoost)
            if (current.graphics.shadeBoostBrightness != base.graphics.shadeBoostBrightness) j.put("shadeBoostBrightness", current.graphics.shadeBoostBrightness)
            if (current.graphics.shadeBoostContrast  != base.graphics.shadeBoostContrast)  j.put("shadeBoostContrast", current.graphics.shadeBoostContrast)
            if (current.graphics.shadeBoostSaturation != base.graphics.shadeBoostSaturation) j.put("shadeBoostSaturation", current.graphics.shadeBoostSaturation)
            if (current.graphics.shadeBoostGamma     != base.graphics.shadeBoostGamma)     j.put("shadeBoostGamma", current.graphics.shadeBoostGamma)
            if (current.graphics.fxaa                != base.graphics.fxaa)                j.put("fxaa", current.graphics.fxaa)
            if (current.graphics.shaderChainEnabled  != base.graphics.shaderChainEnabled)  j.put("shaderChainEnabled", current.graphics.shaderChainEnabled)
            if (current.graphics.shaderChainPreset   != base.graphics.shaderChainPreset)   j.put("shaderChainPreset", current.graphics.shaderChainPreset)
            if (current.graphics.shaderChainParams   != base.graphics.shaderChainParams)   j.put("shaderChainParams", shaderChainParamsToJson(current.graphics.shaderChainParams))
            if (current.graphics.lsfgEnabled         != base.graphics.lsfgEnabled)         j.put("lsfgEnabled", current.graphics.lsfgEnabled)
            if (current.graphics.lsfgMultiplier      != base.graphics.lsfgMultiplier)      j.put("lsfgMultiplier", current.graphics.lsfgMultiplier)
            if (current.graphics.lsfgDllPath         != base.graphics.lsfgDllPath)         j.put("lsfgDllPath", current.graphics.lsfgDllPath)
            if (current.graphics.lsfgPerformance     != base.graphics.lsfgPerformance)     j.put("lsfgPerformance", current.graphics.lsfgPerformance)
            if (current.graphics.lsfgFlowScale       != base.graphics.lsfgFlowScale)       j.put("lsfgFlowScale", current.graphics.lsfgFlowScale)
            if (current.graphics.lsfgTargetRate      != base.graphics.lsfgTargetRate)      j.put("lsfgTargetRate", current.graphics.lsfgTargetRate)
            if (current.graphics.casMode             != base.graphics.casMode)             j.put("casMode", current.graphics.casMode)
            if (current.graphics.casSharpness        != base.graphics.casSharpness)        j.put("casSharpness", current.graphics.casSharpness)
            if (current.graphics.upscaler            != base.graphics.upscaler)            j.put("upscaler", current.graphics.upscaler)
            if (current.graphics.fsrSharpness        != base.graphics.fsrSharpness)        j.put("fsrSharpness", current.graphics.fsrSharpness)
            if (current.graphics.sgsrSharpness       != base.graphics.sgsrSharpness)       j.put("sgsrSharpness", current.graphics.sgsrSharpness)
            if (current.graphics.loadTextureReplacements != base.graphics.loadTextureReplacements) j.put("loadTextureReplacements", current.graphics.loadTextureReplacements)
            if (current.graphics.loadTextureReplacementsAsync != base.graphics.loadTextureReplacementsAsync) j.put("loadTextureReplacementsAsync", current.graphics.loadTextureReplacementsAsync)
            if (current.graphics.precacheTextureReplacements != base.graphics.precacheTextureReplacements) j.put("precacheTextureReplacements", current.graphics.precacheTextureReplacements)
            if (current.graphics.dumpReplaceableTextures != base.graphics.dumpReplaceableTextures) j.put("dumpReplaceableTextures", current.graphics.dumpReplaceableTextures)
            if (current.graphics.osdShowTextureReplacements != base.graphics.osdShowTextureReplacements) j.put("osdShowTextureReplacements", current.graphics.osdShowTextureReplacements)
            if (current.osd.osdShowFps != base.osd.osdShowFps) j.put("osdShowFps", current.osd.osdShowFps)
            if (current.osd.osdScale != base.osd.osdScale) j.put("osdScale", current.osd.osdScale)
            if (current.osd.osdColor != base.osd.osdColor) j.put("osdColor", current.osd.osdColor)
            if (current.osd.osdPosition != base.osd.osdPosition) j.put("osdPosition", current.osd.osdPosition)
            if (current.display.vsyncEnable != base.display.vsyncEnable) j.put("vsyncEnable", current.display.vsyncEnable)
            if (current.osd.osdShowVps != base.osd.osdShowVps) j.put("osdShowVps", current.osd.osdShowVps)
            if (current.osd.osdShowSpeed != base.osd.osdShowSpeed) j.put("osdShowSpeed", current.osd.osdShowSpeed)
            if (current.osd.osdShowCpu != base.osd.osdShowCpu) j.put("osdShowCpu", current.osd.osdShowCpu)
            if (current.osd.osdShowGpu != base.osd.osdShowGpu) j.put("osdShowGpu", current.osd.osdShowGpu)
            if (current.osd.osdShowResolution != base.osd.osdShowResolution) j.put("osdShowResolution", current.osd.osdShowResolution)
            if (current.osd.osdShowGsStats != base.osd.osdShowGsStats) j.put("osdShowGsStats", current.osd.osdShowGsStats)
            if (current.osd.osdShowFrameTimes != base.osd.osdShowFrameTimes) j.put("osdShowFrameTimes", current.osd.osdShowFrameTimes)
            if (current.osd.osdShowHardwareInfo != base.osd.osdShowHardwareInfo) j.put("osdShowHardwareInfo", current.osd.osdShowHardwareInfo)
            if (current.osd.osdShowMessages != base.osd.osdShowMessages) j.put("osdShowMessages", current.osd.osdShowMessages)
            if (current.osd.osdShowGpuStats != base.osd.osdShowGpuStats) j.put("osdShowGpuStats", current.osd.osdShowGpuStats)
            if (current.osd.osdShowVersion != base.osd.osdShowVersion) j.put("osdShowVersion", current.osd.osdShowVersion)
            if (current.osd.osdShowSettings != base.osd.osdShowSettings) j.put("osdShowSettings", current.osd.osdShowSettings)
            if (current.osd.osdShowInputs != base.osd.osdShowInputs) j.put("osdShowInputs", current.osd.osdShowInputs)
            if (current.hwFixes.autoFlush           != base.hwFixes.autoFlush)           j.put("autoFlush", current.hwFixes.autoFlush)
            if (current.hwFixes.halfPixelOffset     != base.hwFixes.halfPixelOffset)     j.put("halfPixelOffset", current.hwFixes.halfPixelOffset)
            if (current.hwFixes.limit24BitDepth     != base.hwFixes.limit24BitDepth)     j.put("limit24BitDepth", current.hwFixes.limit24BitDepth)
            if (current.hwFixes.manualUserHacks     != base.hwFixes.manualUserHacks)     j.put("manualUserHacks", current.hwFixes.manualUserHacks)
            if (current.hwFixes.textureInsideRt     != base.hwFixes.textureInsideRt)     j.put("textureInsideRt", current.hwFixes.textureInsideRt)
            if (current.hwFixes.nativeScaling       != base.hwFixes.nativeScaling)       j.put("nativeScaling", current.hwFixes.nativeScaling)
            if (current.hwFixes.roundSprite         != base.hwFixes.roundSprite)         j.put("roundSprite", current.hwFixes.roundSprite)
            if (current.hwFixes.bilinearUpscale     != base.hwFixes.bilinearUpscale)     j.put("bilinearUpscale", current.hwFixes.bilinearUpscale)
            if (current.hwFixes.gpuTargetClut       != base.hwFixes.gpuTargetClut)       j.put("gpuTargetClut", current.hwFixes.gpuTargetClut)
            if (current.hwFixes.cpuSpriteRenderBw   != base.hwFixes.cpuSpriteRenderBw)   j.put("cpuSpriteRenderBw", current.hwFixes.cpuSpriteRenderBw)
            if (current.hwFixes.cpuSpriteRenderLevel != base.hwFixes.cpuSpriteRenderLevel) j.put("cpuSpriteRenderLevel", current.hwFixes.cpuSpriteRenderLevel)
            if (current.hwFixes.alignSprite         != base.hwFixes.alignSprite)         j.put("alignSprite", current.hwFixes.alignSprite)
            if (current.hwFixes.mergeSprite         != base.hwFixes.mergeSprite)         j.put("mergeSprite", current.hwFixes.mergeSprite)
            if (current.hwFixes.forceEvenSpritePosition != base.hwFixes.forceEvenSpritePosition) j.put("forceEvenSpritePosition", current.hwFixes.forceEvenSpritePosition)
            if (current.hwFixes.unscaledPaletteDraw != base.hwFixes.unscaledPaletteDraw) j.put("unscaledPaletteDraw", current.hwFixes.unscaledPaletteDraw)
            if (current.hwFixes.textureOffsetX      != base.hwFixes.textureOffsetX)      j.put("textureOffsetX", current.hwFixes.textureOffsetX)
            if (current.hwFixes.textureOffsetY      != base.hwFixes.textureOffsetY)      j.put("textureOffsetY", current.hwFixes.textureOffsetY)
            if (current.hwFixes.gpuPaletteConversion != base.hwFixes.gpuPaletteConversion) j.put("gpuPaletteConversion", current.hwFixes.gpuPaletteConversion)
            if (current.hwFixes.cpuFramebufferConversion != base.hwFixes.cpuFramebufferConversion) j.put("cpuFramebufferConversion", current.hwFixes.cpuFramebufferConversion)
            if (current.hwFixes.readTargetsWhenClosing != base.hwFixes.readTargetsWhenClosing) j.put("readTargetsWhenClosing", current.hwFixes.readTargetsWhenClosing)
            if (current.hwFixes.disableDepthEmulation != base.hwFixes.disableDepthEmulation) j.put("disableDepthEmulation", current.hwFixes.disableDepthEmulation)
            if (current.hwFixes.disablePartialInvalidation != base.hwFixes.disablePartialInvalidation) j.put("disablePartialInvalidation", current.hwFixes.disablePartialInvalidation)
            if (current.hwFixes.disableSafeFeatures != base.hwFixes.disableSafeFeatures) j.put("disableSafeFeatures", current.hwFixes.disableSafeFeatures)
            if (current.hwFixes.disableRenderFixes  != base.hwFixes.disableRenderFixes)  j.put("disableRenderFixes", current.hwFixes.disableRenderFixes)
            if (current.hwFixes.preloadFrameData    != base.hwFixes.preloadFrameData)    j.put("preloadFrameData", current.hwFixes.preloadFrameData)
            if (current.hwFixes.estimateTextureRegion != base.hwFixes.estimateTextureRegion) j.put("estimateTextureRegion", current.hwFixes.estimateTextureRegion)
            if (current.hwFixes.drawBuffering        != base.hwFixes.drawBuffering)        j.put("drawBuffering", current.hwFixes.drawBuffering)
            if (current.hwFixes.cpuClutRender       != base.hwFixes.cpuClutRender)       j.put("cpuClutRender", current.hwFixes.cpuClutRender)
            if (current.hwFixes.triFilter           != base.hwFixes.triFilter)           j.put("triFilter", current.hwFixes.triFilter)
            if (current.hwFixes.maxAnisotropy       != base.hwFixes.maxAnisotropy)       j.put("maxAnisotropy", current.hwFixes.maxAnisotropy)
            if (current.hwFixes.gpuProfile          != base.hwFixes.gpuProfile)          j.put("gpuProfile", current.hwFixes.gpuProfile)
            return j
        }

        fun merge(base: Settings, overrides: JSONObject): Settings = Settings(
            cpu = CpuSettings(
                eeCycleRate = if (overrides.has("eeCycleRate")) overrides.getInt("eeCycleRate") else base.cpu.eeCycleRate,
                eeCycleSkip = if (overrides.has("eeCycleSkip")) overrides.getInt("eeCycleSkip") else base.cpu.eeCycleSkip,
                eeClampMode = if (overrides.has("eeClampMode")) overrides.getInt("eeClampMode") else base.cpu.eeClampMode,
                vuClampMode = if (overrides.has("vuClampMode")) overrides.getInt("vuClampMode") else base.cpu.vuClampMode,
                vu1ClampMode = if (overrides.has("vu1ClampMode")) overrides.getInt("vu1ClampMode") else base.cpu.vu1ClampMode,
                mtvu = if (overrides.has("mtvu")) overrides.getBoolean("mtvu") else base.cpu.mtvu,
                vu1Instant = if (overrides.has("vu1Instant")) overrides.getBoolean("vu1Instant") else base.cpu.vu1Instant,
                vuFlagHack = if (overrides.has("vuFlagHack")) overrides.getBoolean("vuFlagHack") else base.cpu.vuFlagHack,
                fastCDVD = if (overrides.has("fastCDVD")) overrides.getBoolean("fastCDVD") else base.cpu.fastCDVD,
                intcStat = if (overrides.has("intcStat")) overrides.getBoolean("intcStat") else base.cpu.intcStat,
                waitLoop = if (overrides.has("waitLoop")) overrides.getBoolean("waitLoop") else base.cpu.waitLoop,
                vuNeonFusions = if (overrides.has("vuNeonFusions")) overrides.getBoolean("vuNeonFusions") else base.cpu.vuNeonFusions,
                vuDeferredWrites = if (overrides.has("vuDeferredWrites")) overrides.getBoolean("vuDeferredWrites") else base.cpu.vuDeferredWrites,
                vuSkipStallSim = if (overrides.has("vuSkipStallSim")) overrides.getBoolean("vuSkipStallSim") else base.cpu.vuSkipStallSim,
                recEE = if (overrides.has("recEE")) overrides.getBoolean("recEE") else base.cpu.recEE,
                recIOP = if (overrides.has("recIOP")) overrides.getBoolean("recIOP") else base.cpu.recIOP,
                recVU0 = if (overrides.has("recVU0")) overrides.getBoolean("recVU0") else base.cpu.recVU0,
                recVU1 = if (overrides.has("recVU1")) overrides.getBoolean("recVU1") else base.cpu.recVU1,
                enableFastmem = if (overrides.has("enableFastmem")) overrides.getBoolean("enableFastmem") else base.cpu.enableFastmem,
                useMacEE = true,
                useMacIOP = true,
                useMacVU0 = true,
                useMacVU1 = true,
                vu1InlineFmacStall = if (overrides.has("vu1InlineFmacStall")) overrides.getBoolean("vu1InlineFmacStall") else base.cpu.vu1InlineFmacStall,
                vu1CrossBlockPState = if (overrides.has("vu1CrossBlockPState")) overrides.getBoolean("vu1CrossBlockPState") else base.cpu.vu1CrossBlockPState,
                vu1InlineDrainTestPipes = if (overrides.has("vu1InlineDrainTestPipes")) overrides.getBoolean("vu1InlineDrainTestPipes") else base.cpu.vu1InlineDrainTestPipes,
                vu1FmacInstanceRouting = if (overrides.has("vu1FmacInstanceRouting")) overrides.getBoolean("vu1FmacInstanceRouting") else base.cpu.vu1FmacInstanceRouting,
            ),
            frameLimit = FrameLimitSettings(
                frameLimitEnable = if (overrides.has("frameLimitEnable")) overrides.getBoolean("frameLimitEnable") else base.frameLimit.frameLimitEnable,
                nominalSpeedPercent = if (overrides.has("nominalSpeedPercent")) overrides.getInt("nominalSpeedPercent") else base.frameLimit.nominalSpeedPercent,
                fpsLimit = if (overrides.has("fpsLimit")) overrides.getInt("fpsLimit") else base.frameLimit.fpsLimit,
                frameSkip = if (overrides.has("frameSkip")) overrides.getInt("frameSkip") else base.frameLimit.frameSkip,
            ),
            audio = AudioSettings(
                audioVolume = if (overrides.has("audioVolume")) overrides.getInt("audioVolume") else base.audio.audioVolume,
                audioMuted = if (overrides.has("audioMuted")) overrides.getBoolean("audioMuted") else base.audio.audioMuted,
                audioSwapChannels = if (overrides.has("audioSwapChannels")) overrides.getBoolean("audioSwapChannels") else base.audio.audioSwapChannels,
                audioTimeStretch = if (overrides.has("audioTimeStretch")) overrides.getBoolean("audioTimeStretch") else base.audio.audioTimeStretch,
                audioBufferMs = if (overrides.has("audioBufferMs")) overrides.getInt("audioBufferMs") else base.audio.audioBufferMs,
                audioOutputLatencyMs = if (overrides.has("audioOutputLatencyMs")) overrides.getInt("audioOutputLatencyMs") else base.audio.audioOutputLatencyMs,
                audioFastForwardVolume = if (overrides.has("audioFastForwardVolume")) overrides.getInt("audioFastForwardVolume") else base.audio.audioFastForwardVolume,
                spu2NeonReverb = if (overrides.has("spu2NeonReverb")) overrides.getBoolean("spu2NeonReverb") else base.audio.spu2NeonReverb,
                audioOpenSLES = if (overrides.has("audioOpenSLES")) overrides.getBoolean("audioOpenSLES") else base.audio.audioOpenSLES,
                spu2LightweightMix = if (overrides.has("spu2LightweightMix")) overrides.getBoolean("spu2LightweightMix") else base.audio.spu2LightweightMix,
            ),
            emuCore = EmuCoreSettings(
                enablePatches = if (overrides.has("enablePatches")) overrides.getBoolean("enablePatches") else base.emuCore.enablePatches,
                enableCheats = if (overrides.has("enableCheats")) overrides.getBoolean("enableCheats") else base.emuCore.enableCheats,
                enableWideScreenPatches = if (overrides.has("enableWideScreenPatches")) overrides.getBoolean("enableWideScreenPatches") else base.emuCore.enableWideScreenPatches,
                enableNoInterlacingPatches = if (overrides.has("enableNoInterlacingPatches")) overrides.getBoolean("enableNoInterlacingPatches") else base.emuCore.enableNoInterlacingPatches,
                enableFastBoot = if (overrides.has("enableFastBoot")) overrides.getBoolean("enableFastBoot") else base.emuCore.enableFastBoot,
                hostFs = if (overrides.has("hostFs")) overrides.getBoolean("hostFs") else base.emuCore.hostFs,
                achievements = AchievementsSettings(
                enabled = if (overrides.has("achievementsEnabled")) overrides.getBoolean("achievementsEnabled") else base.emuCore.achievements.enabled,
                hardcore = if (overrides.has("achievementsHardcore")) overrides.getBoolean("achievementsHardcore") else base.emuCore.achievements.hardcore,
                notifications = if (overrides.has("achievementsNotifications")) overrides.getBoolean("achievementsNotifications") else base.emuCore.achievements.notifications,
                leaderboardNotifications = if (overrides.has("achievementsLeaderboardNotifications")) overrides.getBoolean("achievementsLeaderboardNotifications") else base.emuCore.achievements.leaderboardNotifications,
                overlays = if (overrides.has("achievementsOverlays")) overrides.getBoolean("achievementsOverlays") else base.emuCore.achievements.overlays,
                lbOverlays = if (overrides.has("achievementsLbOverlays")) overrides.getBoolean("achievementsLbOverlays") else base.emuCore.achievements.lbOverlays,
                soundEffects = if (overrides.has("achievementsSoundEffects")) overrides.getBoolean("achievementsSoundEffects") else base.emuCore.achievements.soundEffects,
                encoreMode = if (overrides.has("achievementsEncoreMode")) overrides.getBoolean("achievementsEncoreMode") else base.emuCore.achievements.encoreMode,
                spectatorMode = if (overrides.has("achievementsSpectatorMode")) overrides.getBoolean("achievementsSpectatorMode") else base.emuCore.achievements.spectatorMode,
                unofficialTestMode = if (overrides.has("achievementsUnofficialTestMode")) overrides.getBoolean("achievementsUnofficialTestMode") else base.emuCore.achievements.unofficialTestMode,
                notificationsDuration = if (overrides.has("achievementsNotificationsDuration")) overrides.getInt("achievementsNotificationsDuration") else base.emuCore.achievements.notificationsDuration,
                leaderboardsDuration = if (overrides.has("achievementsLeaderboardsDuration")) overrides.getInt("achievementsLeaderboardsDuration") else base.emuCore.achievements.leaderboardsDuration,
                notificationPosition = if (overrides.has("achievementsNotificationPosition")) overrides.getInt("achievementsNotificationPosition") else base.emuCore.achievements.notificationPosition,
                overlayPosition = if (overrides.has("achievementsOverlayPosition")) overrides.getInt("achievementsOverlayPosition") else base.emuCore.achievements.overlayPosition,
                notificationScale = if (overrides.has("achievementsNotificationScale")) overrides.getInt("achievementsNotificationScale") else base.emuCore.achievements.notificationScale,
            ),
                // Always the global value: PINE is one server for the process, so "this game runs
                // with PINE on" is not a thing that can be true. Deliberately absent from the diff
                // above too, so a per-game file never acquires the key -- but it still has to be
                // listed HERE, because this is a full constructor and an omitted field silently
                // resets to the default rather than inheriting from base.
                pineEnabled = base.emuCore.pineEnabled,
                pineSlot = base.emuCore.pineSlot,
                enableGameFixes = if (overrides.has("enableGameFixes")) overrides.getBoolean("enableGameFixes") else base.emuCore.enableGameFixes,
                gamefixSoftwareRendererFmv = if (overrides.has("gamefixSoftwareRendererFmv")) overrides.getBoolean("gamefixSoftwareRendererFmv") else base.emuCore.gamefixSoftwareRendererFmv,
                gamefixSkipMpeg = if (overrides.has("gamefixSkipMpeg")) overrides.getBoolean("gamefixSkipMpeg") else base.emuCore.gamefixSkipMpeg,
                gamefixEETiming = if (overrides.has("gamefixEETiming")) overrides.getBoolean("gamefixEETiming") else base.emuCore.gamefixEETiming,
                gamefixInstantDma = if (overrides.has("gamefixInstantDma")) overrides.getBoolean("gamefixInstantDma") else base.emuCore.gamefixInstantDma,
                gamefixBlitInternalFps = if (overrides.has("gamefixBlitInternalFps")) overrides.getBoolean("gamefixBlitInternalFps") else base.emuCore.gamefixBlitInternalFps,
                gamefixOphFlag = if (overrides.has("gamefixOphFlag")) overrides.getBoolean("gamefixOphFlag") else base.emuCore.gamefixOphFlag,
                gamefixGifFifo = if (overrides.has("gamefixGifFifo")) overrides.getBoolean("gamefixGifFifo") else base.emuCore.gamefixGifFifo,
                gamefixDmaBusy = if (overrides.has("gamefixDmaBusy")) overrides.getBoolean("gamefixDmaBusy") else base.emuCore.gamefixDmaBusy,
                gamefixVif1Stall = if (overrides.has("gamefixVif1Stall")) overrides.getBoolean("gamefixVif1Stall") else base.emuCore.gamefixVif1Stall,
                gamefixIbit = if (overrides.has("gamefixIbit")) overrides.getBoolean("gamefixIbit") else base.emuCore.gamefixIbit,
                gamefixFullVu0Sync = if (overrides.has("gamefixFullVu0Sync")) overrides.getBoolean("gamefixFullVu0Sync") else base.emuCore.gamefixFullVu0Sync,
                gamefixVuAddSub = if (overrides.has("gamefixVuAddSub")) overrides.getBoolean("gamefixVuAddSub") else base.emuCore.gamefixVuAddSub,
                gamefixVuOverflow = if (overrides.has("gamefixVuOverflow")) overrides.getBoolean("gamefixVuOverflow") else base.emuCore.gamefixVuOverflow,
                gamefixXgkick = if (overrides.has("gamefixXgkick")) overrides.getBoolean("gamefixXgkick") else base.emuCore.gamefixXgkick,
                gamefixGoemonTlb = if (overrides.has("gamefixGoemonTlb")) overrides.getBoolean("gamefixGoemonTlb") else base.emuCore.gamefixGoemonTlb,
                gamefixVuSync = if (overrides.has("gamefixVuSync")) overrides.getBoolean("gamefixVuSync") else base.emuCore.gamefixVuSync,
                skipDuplicateFrames = if (overrides.has("skipDuplicateFrames")) overrides.getBoolean("skipDuplicateFrames") else base.emuCore.skipDuplicateFrames,
                eeFpuRoundMode = if (overrides.has("eeFpuRoundMode")) overrides.getInt("eeFpuRoundMode") else base.emuCore.eeFpuRoundMode,
                vu0RoundMode = if (overrides.has("vu0RoundMode")) overrides.getInt("vu0RoundMode") else base.emuCore.vu0RoundMode,
                vu1RoundMode = if (overrides.has("vu1RoundMode")) overrides.getInt("vu1RoundMode") else base.emuCore.vu1RoundMode,
            ),
            display = DisplaySettings(
                screenOffsets = if (overrides.has("screenOffsets")) overrides.getBoolean("screenOffsets") else base.display.screenOffsets,
                showOverscan = if (overrides.has("showOverscan")) overrides.getBoolean("showOverscan") else base.display.showOverscan,
                antiBlur = if (overrides.has("antiBlur")) overrides.getBoolean("antiBlur") else base.display.antiBlur,
                disableInterlaceOffset = if (overrides.has("disableInterlaceOffset")) overrides.getBoolean("disableInterlaceOffset") else base.display.disableInterlaceOffset,
                syncToHostRefresh = if (overrides.has("syncToHostRefresh")) overrides.getBoolean("syncToHostRefresh") else base.display.syncToHostRefresh,
                disableFramebufferFetch = if (overrides.has("disableFramebufferFetch")) overrides.getBoolean("disableFramebufferFetch") else base.display.disableFramebufferFetch,
                hwRov = if (overrides.has("hwRov")) overrides.getBoolean("hwRov") else base.display.hwRov,
                hwAa1 = if (overrides.has("hwAa1")) overrides.getBoolean("hwAa1") else base.display.hwAa1,
                hwAat = false,
                coalesceRenderPasses = if (overrides.has("coalesceRenderPasses")) overrides.getBoolean("coalesceRenderPasses") else base.display.coalesceRenderPasses,
                forceMaliFbFetch = if (overrides.has("forceMaliFbFetch")) overrides.getBoolean("forceMaliFbFetch") else base.display.forceMaliFbFetch,
                useAngleOpenGL = if (overrides.has("useAngleOpenGL")) overrides.getBoolean("useAngleOpenGL") else base.display.useAngleOpenGL,
                overrideTextureBarriers = if (overrides.has("overrideTextureBarriers")) overrides.getInt("overrideTextureBarriers") else base.display.overrideTextureBarriers,
                gsBackThreadMode = if (overrides.has("gsBackThreadMode")) overrides.getInt("gsBackThreadMode") else base.display.gsBackThreadMode,
                disableVertexShaderExpand = if (overrides.has("disableVertexShaderExpand")) overrides.getBoolean("disableVertexShaderExpand") else base.display.disableVertexShaderExpand,
                useBlitSwapChain = if (overrides.has("useBlitSwapChain")) overrides.getBoolean("useBlitSwapChain") else base.display.useBlitSwapChain,
                disableShaderCache = if (overrides.has("disableShaderCache")) overrides.getBoolean("disableShaderCache") else base.display.disableShaderCache,
                hwAccurateAlphaTest = when {
                overrides.has("hwAccurateAlphaTest") -> overrides.getBoolean("hwAccurateAlphaTest")
                overrides.has("hwAat") -> overrides.getBoolean("hwAat")
                else -> base.display.hwAccurateAlphaTest
            },
                vsyncEnable = if (overrides.has("vsyncEnable")) overrides.getBoolean("vsyncEnable") else base.display.vsyncEnable,
            ),
            hwFixes = HwFixesSettings(
                skipDrawStart = if (overrides.has("skipDrawStart")) overrides.getInt("skipDrawStart") else base.hwFixes.skipDrawStart,
                skipDrawEnd = if (overrides.has("skipDrawEnd")) overrides.getInt("skipDrawEnd") else base.hwFixes.skipDrawEnd,
                spinGpuReadbacks = if (overrides.has("spinGpuReadbacks")) overrides.getBoolean("spinGpuReadbacks") else base.hwFixes.spinGpuReadbacks,
                spinCpuReadbacks = if (overrides.has("spinCpuReadbacks")) overrides.getBoolean("spinCpuReadbacks") else base.hwFixes.spinCpuReadbacks,
                integerScaling = if (overrides.has("integerScaling")) overrides.getBoolean("integerScaling") else base.hwFixes.integerScaling,
                cropLeft = if (overrides.has("cropLeft")) overrides.getInt("cropLeft") else base.hwFixes.cropLeft,
                displayZoom = if (overrides.has("displayZoom")) overrides.getInt("displayZoom") else base.hwFixes.displayZoom,
                cropTop = if (overrides.has("cropTop")) overrides.getInt("cropTop") else base.hwFixes.cropTop,
                cropRight = if (overrides.has("cropRight")) overrides.getInt("cropRight") else base.hwFixes.cropRight,
                cropBottom = if (overrides.has("cropBottom")) overrides.getInt("cropBottom") else base.hwFixes.cropBottom,
                dithering = if (overrides.has("dithering")) overrides.getInt("dithering") else base.hwFixes.dithering,
                vsyncQueueSize = if (overrides.has("vsyncQueueSize")) overrides.getInt("vsyncQueueSize") else base.hwFixes.vsyncQueueSize,
                autoFlush = if (overrides.has("autoFlush")) overrides.getInt("autoFlush") else base.hwFixes.autoFlush,
                halfPixelOffset = if (overrides.has("halfPixelOffset")) overrides.getInt("halfPixelOffset") else base.hwFixes.halfPixelOffset,
                limit24BitDepth = if (overrides.has("limit24BitDepth")) overrides.getInt("limit24BitDepth") else base.hwFixes.limit24BitDepth,
                manualUserHacks = if (overrides.has("manualUserHacks")) overrides.getBoolean("manualUserHacks") else base.hwFixes.manualUserHacks,
                textureInsideRt = if (overrides.has("textureInsideRt")) overrides.getInt("textureInsideRt") else base.hwFixes.textureInsideRt,
                nativeScaling = if (overrides.has("nativeScaling")) overrides.getInt("nativeScaling") else base.hwFixes.nativeScaling,
                roundSprite = if (overrides.has("roundSprite")) overrides.getInt("roundSprite") else base.hwFixes.roundSprite,
                bilinearUpscale = if (overrides.has("bilinearUpscale")) overrides.getInt("bilinearUpscale") else base.hwFixes.bilinearUpscale,
                gpuTargetClut = if (overrides.has("gpuTargetClut")) overrides.getInt("gpuTargetClut") else base.hwFixes.gpuTargetClut,
                cpuSpriteRenderBw = if (overrides.has("cpuSpriteRenderBw")) overrides.getInt("cpuSpriteRenderBw") else base.hwFixes.cpuSpriteRenderBw,
                cpuSpriteRenderLevel = if (overrides.has("cpuSpriteRenderLevel")) overrides.getInt("cpuSpriteRenderLevel") else base.hwFixes.cpuSpriteRenderLevel,
                alignSprite = if (overrides.has("alignSprite")) overrides.getBoolean("alignSprite") else base.hwFixes.alignSprite,
                mergeSprite = if (overrides.has("mergeSprite")) overrides.getBoolean("mergeSprite") else base.hwFixes.mergeSprite,
                forceEvenSpritePosition = if (overrides.has("forceEvenSpritePosition")) overrides.getBoolean("forceEvenSpritePosition") else base.hwFixes.forceEvenSpritePosition,
                unscaledPaletteDraw = if (overrides.has("unscaledPaletteDraw")) overrides.getBoolean("unscaledPaletteDraw") else base.hwFixes.unscaledPaletteDraw,
                textureOffsetX = if (overrides.has("textureOffsetX")) overrides.getInt("textureOffsetX") else base.hwFixes.textureOffsetX,
                textureOffsetY = if (overrides.has("textureOffsetY")) overrides.getInt("textureOffsetY") else base.hwFixes.textureOffsetY,
                gpuPaletteConversion = if (overrides.has("gpuPaletteConversion")) overrides.getBoolean("gpuPaletteConversion") else base.hwFixes.gpuPaletteConversion,
                cpuFramebufferConversion = if (overrides.has("cpuFramebufferConversion")) overrides.getBoolean("cpuFramebufferConversion") else base.hwFixes.cpuFramebufferConversion,
                readTargetsWhenClosing = if (overrides.has("readTargetsWhenClosing")) overrides.getBoolean("readTargetsWhenClosing") else base.hwFixes.readTargetsWhenClosing,
                disableDepthEmulation = if (overrides.has("disableDepthEmulation")) overrides.getBoolean("disableDepthEmulation") else base.hwFixes.disableDepthEmulation,
                disablePartialInvalidation = if (overrides.has("disablePartialInvalidation")) overrides.getBoolean("disablePartialInvalidation") else base.hwFixes.disablePartialInvalidation,
                disableSafeFeatures = if (overrides.has("disableSafeFeatures")) overrides.getBoolean("disableSafeFeatures") else base.hwFixes.disableSafeFeatures,
                disableRenderFixes = if (overrides.has("disableRenderFixes")) overrides.getBoolean("disableRenderFixes") else base.hwFixes.disableRenderFixes,
                preloadFrameData = if (overrides.has("preloadFrameData")) overrides.getBoolean("preloadFrameData") else base.hwFixes.preloadFrameData,
                estimateTextureRegion = if (overrides.has("estimateTextureRegion")) overrides.getBoolean("estimateTextureRegion") else base.hwFixes.estimateTextureRegion,
                drawBuffering = if (overrides.has("drawBuffering")) overrides.getBoolean("drawBuffering") else base.hwFixes.drawBuffering,
                cpuClutRender = if (overrides.has("cpuClutRender")) overrides.getInt("cpuClutRender") else base.hwFixes.cpuClutRender,
                triFilter = if (overrides.has("triFilter")) overrides.getInt("triFilter") else base.hwFixes.triFilter,
                maxAnisotropy = if (overrides.has("maxAnisotropy")) overrides.getInt("maxAnisotropy") else base.hwFixes.maxAnisotropy,
                gpuProfile = if (overrides.has("gpuProfile")) overrides.getInt("gpuProfile") else base.hwFixes.gpuProfile,
            ),
            output = OutputSettings(
                renderer = if (overrides.has("renderer")) overrides.getString("renderer") else base.output.renderer,
                upscaleFloat = if (overrides.has("upscaleFloat")) overrides.getDouble("upscaleFloat").toFloat() else base.output.upscaleFloat,
                customDriverId = if (overrides.has("customDriverId")) overrides.getString("customDriverId") else base.output.customDriverId,
                orientation = if (overrides.has("orientation")) overrides.getInt("orientation") else base.output.orientation,
                portraitRenderTop = if (overrides.has("portraitRenderTop")) overrides.getBoolean("portraitRenderTop") else base.output.portraitRenderTop,
                landscapeRenderTop = if (overrides.has("landscapeRenderTop")) overrides.getBoolean("landscapeRenderTop") else base.output.landscapeRenderTop,
                autoProgressiveScan = if (overrides.has("autoProgressiveScan")) overrides.getBoolean("autoProgressiveScan") else base.output.autoProgressiveScan,
                affinityMode = if (overrides.has("affinityMode")) overrides.getInt("affinityMode") else base.output.affinityMode,
                framerateNtsc = if (overrides.has("framerateNtsc")) overrides.getDouble("framerateNtsc").toFloat() else base.output.framerateNtsc,
                frameratePal = if (overrides.has("frameratePal")) overrides.getDouble("frameratePal").toFloat() else base.output.frameratePal,
                hwScaler = if (overrides.has("hwScaler")) overrides.getInt("hwScaler") else base.output.hwScaler,
                screenResOverride = if (overrides.has("screenResOverride")) overrides.getString("screenResOverride") else base.output.screenResOverride,
                autoFlushSw = if (overrides.has("autoFlushSw")) overrides.getBoolean("autoFlushSw") else base.output.autoFlushSw,
                mipmapSw = if (overrides.has("mipmapSw")) overrides.getBoolean("mipmapSw") else base.output.mipmapSw,
                swThreads = if (overrides.has("swThreads")) overrides.getInt("swThreads") else base.output.swThreads,
                swThreadsHeight = if (overrides.has("swThreadsHeight")) overrides.getInt("swThreadsHeight") else base.output.swThreadsHeight,
                aspectRatio = if (overrides.has("aspectRatio")) overrides.getInt("aspectRatio") else base.output.aspectRatio,
                fmvAspectRatio = if (overrides.has("fmvAspectRatio")) overrides.getInt("fmvAspectRatio") else base.output.fmvAspectRatio,
                customAspectRatio = if (overrides.has("customAspectRatio")) overrides.getDouble("customAspectRatio").toFloat() else base.output.customAspectRatio,
                deinterlaceMode = if (overrides.has("deinterlaceMode")) overrides.getInt("deinterlaceMode") else base.output.deinterlaceMode,
            ),
            network = NetworkSettings(
                dev9EthEnable = if (overrides.has("dev9EthEnable")) overrides.getBoolean("dev9EthEnable") else base.network.dev9EthEnable,
                dev9EthApi = if (overrides.has("dev9EthApi")) overrides.getString("dev9EthApi").ifEmpty { base.network.dev9EthApi } else base.network.dev9EthApi,
                localLinkHost = if (overrides.has("localLinkHost")) overrides.getBoolean("localLinkHost") else base.network.localLinkHost,
                localLinkAddress = if (overrides.has("localLinkAddress")) overrides.getString("localLinkAddress") else base.network.localLinkAddress,
                localLinkPort = if (overrides.has("localLinkPort")) overrides.getInt("localLinkPort") else base.network.localLinkPort,
                localLinkPeerId = if (overrides.has("localLinkPeerId")) overrides.getInt("localLinkPeerId") else base.network.localLinkPeerId,
                localLinkRoomCode = if (overrides.has("localLinkRoomCode")) overrides.getString("localLinkRoomCode") else base.network.localLinkRoomCode,
                dev9EthDevice = if (overrides.has("dev9EthDevice")) overrides.getString("dev9EthDevice").ifEmpty { base.network.dev9EthDevice } else base.network.dev9EthDevice,
                dev9EthLogDhcp = if (overrides.has("dev9EthLogDhcp")) overrides.getBoolean("dev9EthLogDhcp") else base.network.dev9EthLogDhcp,
                dev9EthLogDns = if (overrides.has("dev9EthLogDns")) overrides.getBoolean("dev9EthLogDns") else base.network.dev9EthLogDns,
                dev9InterceptDhcp = if (overrides.has("dev9InterceptDhcp")) overrides.getBoolean("dev9InterceptDhcp") else base.network.dev9InterceptDhcp,
                dev9Ps2Ip = if (overrides.has("dev9Ps2Ip")) overrides.getString("dev9Ps2Ip").ifEmpty { base.network.dev9Ps2Ip } else base.network.dev9Ps2Ip,
                dev9Mask = if (overrides.has("dev9Mask")) overrides.getString("dev9Mask").ifEmpty { base.network.dev9Mask } else base.network.dev9Mask,
                dev9Gateway = if (overrides.has("dev9Gateway")) overrides.getString("dev9Gateway").ifEmpty { base.network.dev9Gateway } else base.network.dev9Gateway,
                dev9Dns1 = if (overrides.has("dev9Dns1")) overrides.getString("dev9Dns1").ifEmpty { base.network.dev9Dns1 } else base.network.dev9Dns1,
                dev9Dns2 = if (overrides.has("dev9Dns2")) overrides.getString("dev9Dns2").ifEmpty { base.network.dev9Dns2 } else base.network.dev9Dns2,
                dev9AutoMask = if (overrides.has("dev9AutoMask")) overrides.getBoolean("dev9AutoMask") else base.network.dev9AutoMask,
                dev9AutoGateway = if (overrides.has("dev9AutoGateway")) overrides.getBoolean("dev9AutoGateway") else base.network.dev9AutoGateway,
                dev9ModeDns1 = if (overrides.has("dev9ModeDns1")) overrides.getString("dev9ModeDns1").ifEmpty { base.network.dev9ModeDns1 } else base.network.dev9ModeDns1,
                dev9ModeDns2 = if (overrides.has("dev9ModeDns2")) overrides.getString("dev9ModeDns2").ifEmpty { base.network.dev9ModeDns2 } else base.network.dev9ModeDns2,
                dev9EthHosts = if (overrides.has("dev9EthHosts")) {
                overrides.optJSONArray("dev9EthHosts")?.let { array ->
                    buildList {
                        repeat(array.length()) { index ->
                            array.optJSONObject(index)?.let { host ->
                                add(
                                    Dev9HostMapping(
                                        url = host.optString("url"),
                                        ip = host.optString("ip", "0.0.0.0"),
                                        enabled = host.optBoolean("enabled", true),
                                    ),
                                )
                            }
                        }
                    }
                } ?: base.network.dev9EthHosts
            } else base.network.dev9EthHosts,
                dev9HddEnable = if (overrides.has("dev9HddEnable")) overrides.getBoolean("dev9HddEnable") else base.network.dev9HddEnable,
                dev9HddFile = if (overrides.has("dev9HddFile")) overrides.getString("dev9HddFile").ifEmpty { base.network.dev9HddFile } else base.network.dev9HddFile,
            ),
            system = SystemSettings(
                memoryCardSlot1Enabled = if (overrides.has("memoryCardSlot1Enabled")) overrides.getBoolean("memoryCardSlot1Enabled") else base.system.memoryCardSlot1Enabled,
                memoryCardSlot1Filename = if (overrides.has("memoryCardSlot1Filename")) overrides.getString("memoryCardSlot1Filename").ifEmpty { base.system.memoryCardSlot1Filename } else base.system.memoryCardSlot1Filename,
                biosFilename = if (overrides.has("biosFilename")) overrides.getString("biosFilename") else base.system.biosFilename,
                memoryCardSlot2Enabled = if (overrides.has("memoryCardSlot2Enabled")) overrides.getBoolean("memoryCardSlot2Enabled") else base.system.memoryCardSlot2Enabled,
                memoryCardSlot2Filename = if (overrides.has("memoryCardSlot2Filename")) overrides.getString("memoryCardSlot2Filename").ifEmpty { base.system.memoryCardSlot2Filename } else base.system.memoryCardSlot2Filename,
                usbKeyboard = if (overrides.has("usbKeyboard")) overrides.getBoolean("usbKeyboard") else base.system.usbKeyboard,
            ),
            graphics = GraphicsSettings(
                hwMipmap = if (overrides.has("hwMipmap")) overrides.getBoolean("hwMipmap") else base.graphics.hwMipmap,
                accurateBlendingUnit = if (overrides.has("accurateBlendingUnit")) overrides.getInt("accurateBlendingUnit") else base.graphics.accurateBlendingUnit,
                textureFiltering = if (overrides.has("textureFiltering")) overrides.getInt("textureFiltering") else base.graphics.textureFiltering,
                displayBilinear = if (overrides.has("displayBilinear")) overrides.getInt("displayBilinear") else base.graphics.displayBilinear,
                texturePreloading = if (overrides.has("texturePreloading")) overrides.getInt("texturePreloading") else base.graphics.texturePreloading,
                hardwareDownloadMode = if (overrides.has("hardwareDownloadMode")) overrides.getInt("hardwareDownloadMode") else base.graphics.hardwareDownloadMode,
                tvShader = if (overrides.has("tvShader")) overrides.getInt("tvShader") else base.graphics.tvShader,
                shadeBoost = if (overrides.has("shadeBoost")) overrides.getBoolean("shadeBoost") else base.graphics.shadeBoost,
                shadeBoostBrightness = if (overrides.has("shadeBoostBrightness")) overrides.getInt("shadeBoostBrightness") else base.graphics.shadeBoostBrightness,
                shadeBoostContrast = if (overrides.has("shadeBoostContrast")) overrides.getInt("shadeBoostContrast") else base.graphics.shadeBoostContrast,
                shadeBoostSaturation = if (overrides.has("shadeBoostSaturation")) overrides.getInt("shadeBoostSaturation") else base.graphics.shadeBoostSaturation,
                shadeBoostGamma = if (overrides.has("shadeBoostGamma")) overrides.getInt("shadeBoostGamma") else base.graphics.shadeBoostGamma,
                fxaa = if (overrides.has("fxaa")) overrides.getBoolean("fxaa") else base.graphics.fxaa,
                shaderChainEnabled = if (overrides.has("shaderChainEnabled")) overrides.getBoolean("shaderChainEnabled") else base.graphics.shaderChainEnabled,
                shaderChainPreset = if (overrides.has("shaderChainPreset")) overrides.getString("shaderChainPreset") else base.graphics.shaderChainPreset,
            // Replaces the global map wholesale rather than merging per parameter: a
            // per-game tweak means "this game's chain looks like THIS", and merging would
            // let a later global edit leak into a game the user had already dialled in.
            shaderChainParams = if (overrides.has("shaderChainParams")) {
                shaderChainParamsFromJson(overrides.optJSONObject("shaderChainParams"))
            } else base.graphics.shaderChainParams,
                lsfgEnabled = if (overrides.has("lsfgEnabled")) overrides.getBoolean("lsfgEnabled") else base.graphics.lsfgEnabled,
                lsfgMultiplier = if (overrides.has("lsfgMultiplier")) overrides.getInt("lsfgMultiplier") else base.graphics.lsfgMultiplier,
                lsfgDllPath = if (overrides.has("lsfgDllPath")) overrides.getString("lsfgDllPath") else base.graphics.lsfgDllPath,
                lsfgPerformance = if (overrides.has("lsfgPerformance")) overrides.getBoolean("lsfgPerformance") else base.graphics.lsfgPerformance,
                lsfgFlowScale = if (overrides.has("lsfgFlowScale")) overrides.getInt("lsfgFlowScale") else base.graphics.lsfgFlowScale,
                lsfgTargetRate = if (overrides.has("lsfgTargetRate")) overrides.getInt("lsfgTargetRate") else base.graphics.lsfgTargetRate,
                casMode = if (overrides.has("casMode")) overrides.getInt("casMode") else base.graphics.casMode,
                casSharpness = if (overrides.has("casSharpness")) overrides.getInt("casSharpness") else base.graphics.casSharpness,
                upscaler = if (overrides.has("upscaler")) overrides.getInt("upscaler") else base.graphics.upscaler,
                fsrSharpness = if (overrides.has("fsrSharpness")) overrides.getInt("fsrSharpness") else base.graphics.fsrSharpness,
                sgsrSharpness = if (overrides.has("sgsrSharpness")) overrides.getInt("sgsrSharpness") else base.graphics.sgsrSharpness,
                loadTextureReplacements = if (overrides.has("loadTextureReplacements")) overrides.getBoolean("loadTextureReplacements") else base.graphics.loadTextureReplacements,
                loadTextureReplacementsAsync = if (overrides.has("loadTextureReplacementsAsync")) overrides.getBoolean("loadTextureReplacementsAsync") else base.graphics.loadTextureReplacementsAsync,
                precacheTextureReplacements = if (overrides.has("precacheTextureReplacements")) overrides.getBoolean("precacheTextureReplacements") else base.graphics.precacheTextureReplacements,
                dumpReplaceableTextures = if (overrides.has("dumpReplaceableTextures")) overrides.getBoolean("dumpReplaceableTextures") else base.graphics.dumpReplaceableTextures,
                osdShowTextureReplacements = if (overrides.has("osdShowTextureReplacements")) overrides.getBoolean("osdShowTextureReplacements") else base.graphics.osdShowTextureReplacements,
            ),
            osd = OsdSettings(
                osdShowFps = if (overrides.has("osdShowFps")) overrides.getBoolean("osdShowFps") else base.osd.osdShowFps,
                osdScale = if (overrides.has("osdScale")) overrides.getInt("osdScale") else base.osd.osdScale,
                osdColor = if (overrides.has("osdColor")) overrides.getInt("osdColor") else base.osd.osdColor,
                osdPosition = if (overrides.has("osdPosition")) overrides.getInt("osdPosition") else base.osd.osdPosition,
                osdShowVps = if (overrides.has("osdShowVps")) overrides.getBoolean("osdShowVps") else base.osd.osdShowVps,
                osdShowSpeed = if (overrides.has("osdShowSpeed")) overrides.getBoolean("osdShowSpeed") else base.osd.osdShowSpeed,
                osdShowCpu = if (overrides.has("osdShowCpu")) overrides.getBoolean("osdShowCpu") else base.osd.osdShowCpu,
                osdShowGpu = if (overrides.has("osdShowGpu")) overrides.getBoolean("osdShowGpu") else base.osd.osdShowGpu,
                osdShowResolution = if (overrides.has("osdShowResolution")) overrides.getBoolean("osdShowResolution") else base.osd.osdShowResolution,
                osdShowGsStats = if (overrides.has("osdShowGsStats")) overrides.getBoolean("osdShowGsStats") else base.osd.osdShowGsStats,
                osdShowFrameTimes = if (overrides.has("osdShowFrameTimes")) overrides.getBoolean("osdShowFrameTimes") else base.osd.osdShowFrameTimes,
                osdShowHardwareInfo = if (overrides.has("osdShowHardwareInfo")) overrides.getBoolean("osdShowHardwareInfo") else base.osd.osdShowHardwareInfo,
                osdShowMessages = if (overrides.has("osdShowMessages")) overrides.getBoolean("osdShowMessages") else base.osd.osdShowMessages,
                osdShowGpuStats = if (overrides.has("osdShowGpuStats")) overrides.getBoolean("osdShowGpuStats") else base.osd.osdShowGpuStats,
                osdShowVersion = if (overrides.has("osdShowVersion")) overrides.getBoolean("osdShowVersion") else base.osd.osdShowVersion,
                osdShowSettings = if (overrides.has("osdShowSettings")) overrides.getBoolean("osdShowSettings") else base.osd.osdShowSettings,
                osdShowInputs = if (overrides.has("osdShowInputs")) overrides.getBoolean("osdShowInputs") else base.osd.osdShowInputs,
            ),
        )
    }
}
