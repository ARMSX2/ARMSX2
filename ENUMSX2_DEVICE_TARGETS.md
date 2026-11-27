# ENUMSX2 Android Device Targets

ENUMSX2 is an Android ARM64 PS2 emulator fork based on ARMSX2/PCSX2.

## Primary targets
- A67L Gen2
- BLU View Ultra 5G

## Optimization policy
- Preserve ARM64 JIT/recompiler paths.
- Use native/1x internal resolution as the safe baseline for constrained hardware.
- Keep Vulkan available as the preferred hardware renderer with OpenGL ES fallback.
- Keep texture/framebuffer memory pressure conservative.
- Reuse shader/pipeline caches to reduce recurring stutter.
- Reduce synchronization/readback overhead only where emulator correctness permits.
- Prefer stable frame pacing and thermal sustainability over forced CPU/GPU clocks.
- Keep game-specific overrides separate from global device defaults.
- Preserve Android 4K/16K page-size compatibility from the ARMSX2 Android build system.

## Legal note
ENUMSX2 does not bundle a PlayStation 2 BIOS or copyrighted game images.