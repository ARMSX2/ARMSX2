// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Host/AudioStreamTypes.h"

#include "common/Pcsx2Defs.h"
#include "common/FPControl.h"

#include <array>
#include <string>
#include <optional>
#include <vector>

// Macro used for removing some of the redtape involved in defining bitfield/union helpers.
//
#define BITFIELD32() \
	union \
	{ \
		u32 bitset; \
		struct \
		{
#define BITFIELD_END \
	} \
	; \
	} \
	;

class Error;
class SettingsInterface;
class SettingsWrapper;

enum class CDVD_SourceType : uint8_t;

namespace Pad
{
	enum class ControllerType : u8;
}

/// Generic setting information which can be reused in multiple components.
struct SettingInfo
{
	using GetOptionsCallback = std::vector<std::pair<std::string, std::string>> (*)();

	enum class Type
	{
		Boolean,
		Integer,
		IntegerList,
		Float,
		String,
		StringList,
		Path,
	};

	Type type;
	const char* name;
	const char* display_name;
	const char* description;
	const char* default_value;
	const char* min_value;
	const char* max_value;
	const char* step_value;
	const char* format;
	const char* const* options; // For integer lists.
	GetOptionsCallback get_options; // For string lists.
	float multiplier;

	const char* StringDefaultValue() const;
	bool BooleanDefaultValue() const;
	s32 IntegerDefaultValue() const;
	s32 IntegerMinValue() const;
	s32 IntegerMaxValue() const;
	s32 IntegerStepValue() const;
	float FloatDefaultValue() const;
	float FloatMinValue() const;
	float FloatMaxValue() const;
	float FloatStepValue() const;

	void SetDefaultValue(SettingsInterface* si, const char* section, const char* key) const;
	void CopyValue(SettingsInterface* dest_si, const SettingsInterface& src_si,
		const char* section, const char* key) const;
};

enum class GenericInputBinding : u8;

// TODO(Stenzek): Move to InputCommon.h or something?
struct InputBindingInfo
{
	enum class Type : u8
	{
		Unknown,
		Button,
		Axis,
		HalfAxis,
		Motor,
		Pointer, // Receive relative mouse movement events, bind_index is offset by the axis.
		Keyboard, // Receive host key events, bind_index is offset by the key code.
		Device, // Used for special-purpose device selection, e.g. force feedback.
		Macro,
	};

	const char* name;
	const char* display_name;
	const char* icon_name;
	Type bind_type;
	u16 bind_index;
	GenericInputBinding generic_mapping;
};

/// Generic input bindings. These roughly match a DualShock 4 or XBox One controller.
/// They are used for automatic binding to PS2 controller types, and for big picture mode navigation.
enum class GenericInputBinding : u8
{
	Unknown,

	DPadUp,
	DPadRight,
	DPadLeft,
	DPadDown,

	LeftStickUp,
	LeftStickRight,
	LeftStickDown,
	LeftStickLeft,
	L3,

	RightStickUp,
	RightStickRight,
	RightStickDown,
	RightStickLeft,
	R3,

	Triangle, // Y on XBox pads.
	Circle, // B on XBox pads.
	Cross, // A on XBox pads.
	Square, // X on XBox pads.

	Select, // Share on DS4, View on XBox pads.
	Start, // Options on DS4, Menu on XBox pads.
	System, // PS button on DS4, Guide button on XBox pads.

	L1, // LB on Xbox pads.
	L2, // Left trigger on XBox pads.
	R1, // RB on XBox pads.
	R2, // Right trigger on Xbox pads.

	SmallMotor, // High frequency vibration.
	LargeMotor, // Low frequency vibration.

	Count,
};

enum GamefixId
{
	GamefixId_FIRST = 0,

	Fix_FpuMultiply = GamefixId_FIRST,
	Fix_GoemonTlbMiss,
	Fix_SoftwareRendererFMV,
	Fix_SkipMpeg,
	Fix_OPHFlag,
	Fix_EETiming,
	Fix_InstantDMA,
	Fix_DMABusy,
	Fix_GIFFIFO,
	Fix_VIFFIFO,
	Fix_VIF1Stall,
	Fix_VuAddSub,
	Fix_Ibit,
	Fix_VUSync,
	Fix_VUOverflow,
	Fix_XGKick,
	Fix_BlitInternalFPS,
	Fix_FullVU0Sync,

	GamefixId_COUNT
};

// TODO - config - not a fan of the excessive use of enums and macros to make them work
// a proper object would likely make more sense (if possible).

enum class SpeedHack
{
	MVUFlag,
	InstantVU1,
	MTVU,
	EECycleRate,
	MaxCount,
};

enum class DebugAnalysisCondition
{
	ALWAYS,
	IF_DEBUGGER_IS_OPEN,
	NEVER
};

struct DebugSymbolSource
{
	std::string Name;
	bool ClearDuringAnalysis = false;

	friend auto operator<=>(const DebugSymbolSource& lhs, const DebugSymbolSource& rhs) = default;
};

struct DebugExtraSymbolFile
{
	std::string Path;
	std::string BaseAddress;
	std::string Condition;

	friend auto operator<=>(const DebugExtraSymbolFile& lhs, const DebugExtraSymbolFile& rhs) = default;
};

enum class DebugFunctionScanMode
{
	SCAN_ELF,
	SCAN_MEMORY,
	SKIP
};

enum class AspectRatioType : u8
{
	Stretch, // Stretches to the whole window/display size
	RAuto4_3_3_2, // Automatically scales to the target aspect ratio if there's a widescreen patch
	R4_3,
	R16_9,
	R10_7,
	// Ultrawide. Only meaningful with a widescreen/ultrawide patch applied -- without one the
	// game still renders 4:3 content and this just crops or pillarboxes it. Requested for fold
	// and tablet users, DeX, and phones driving a 21:9 display, who otherwise had to use
	// Stretch and accept the distortion.
	R21_9,
	// 20:9 is the real panel ratio of most modern phones, which 21:9 only approximates -- close
	// enough to leave thin black bars. Appended rather than slotted in next to R21_9 on purpose:
	// these values are persisted as raw ints in the ini and in the Android prefs, so inserting
	// mid-enum would silently repoint every saved 21:9 config at a different ratio.
	R20_9,
	// 19.5:9 — the other common modern phone ratio (many Xiaomi/Samsung panels). Appended, not
	// slotted next to R20_9, for the same persisted-raw-int reason documented above.
	R19_5_9,
	// User-entered ratio (GSOptions::CustomAspectRatio). Last on purpose: it is the catch-all for
	// panels none of the fixed entries match, and appending keeps every persisted index stable.
	Custom,
	MaxCount
};

enum class FMVAspectRatioSwitchType : u8
{
	Off, // Falls back on the selected generic aspect ratio type
	RAuto4_3_3_2,
	R4_3,
	R16_9,
	R10_7,
	// Ultrawide. Only meaningful with a widescreen/ultrawide patch applied -- without one the
	// game still renders 4:3 content and this just crops or pillarboxes it. Requested for fold
	// and tablet users, DeX, and phones driving a 21:9 display, who otherwise had to use
	// Stretch and accept the distortion.
	R21_9,
	// See AspectRatioType::R20_9 -- appended for the same persisted-index reason.
	R20_9,
	R19_5_9,
	Custom,
	MaxCount
};

// Display rotation applied at present time. Useful for handhelds whose panel
// is mounted in one orientation but the user wants the game in another.
// Rotation is applied to the *final* swapchain blit only; internal GS
// coordinates and aspect-ratio math run in the unrotated frame.
enum class DisplayRotation : u8
{
	Rot0,
	Rot90,
	Rot180,
	Rot270,
	MaxCount
};

enum class MemoryCardType
{
	Empty,
	File,
	Folder,
	MaxCount
};

enum class MemoryCardFileType
{
	Unknown,
	PS2_8MB,
	PS2_16MB,
	PS2_32MB,
	PS2_64MB,
	PS1,
	MaxCount
};

enum class LimiterModeType : u8
{
	Nominal,
	Turbo,
	Slomo,
	Unlimited,
};

enum class GSRendererType : s8
{
	Auto = -1,
	DX11 = 3,
	Null = 11,
	OGL = 12,
	SW = 13,
	VK = 14,
	Metal = 17,
	DX12 = 15,
};

// Which hardware-renderer implementation a Vulkan device runs: the classic desktop-shaped
// GSRendererHW, or the tiler-native Tile renderer. Deliberately orthogonal to GSRendererType:
// a new renderer-type integer would be silently dropped by the Android settings-recovery
// whitelist, would turn every switch into a full device teardown (RestartOptionsAreEqual keys
// on Renderer), and would break GSIsHardwareRenderer() call sites. As a variant, a switch is
// the cheap renderer-only GSreopen. Selection: GS/Renderers/Common/GSTileSelectionPolicy.h.
enum class GSHWRendererVariant : u8
{
	Auto = 0,
	Classic = 1,
	Tile = 2,
	// The GS-on-GPU backend: pass-planned indirect submission with VRAM truth on the GPU.
	// Shares Tile's page/hazard models as libraries but is a separate renderer class (plain
	// GSRenderer subclass, no software floor). Vulkan-only, opt-in, never resolved by Auto.
	TileGpu = 3,
};

enum class GSVSyncMode : u8
{
	Disabled,
	FIFO,
	Mailbox,
	Count
};

enum class GSInterlaceMode : u8
{
	Automatic,
	Off,
	WeaveTFF,
	WeaveBFF,
	BobTFF,
	BobBFF,
	BlendTFF,
	BlendBFF,
	AdaptiveTFF,
	AdaptiveBFF,
	Count
};

enum class GSPostBilinearMode : u8
{
	Off,
	BilinearSmooth,
	BilinearSharp,
};

// Ordering was done to keep compatibility with older ini file.
enum class BiFiltering : u8
{
	Nearest,
	Forced,
	PS2,
	Forced_But_Sprite,
};

enum class TriFiltering : s8
{
	Automatic = -1,
	Off,
	PS2,
	Forced,
};

enum class AccBlendLevel : u8
{
	Minimum,
	Basic,
	Medium,
	High,
	Full,
	Maximum,
	MaxCount
};

enum class OsdOverlayPos : u8
{
	None,
	TopLeft,
	TopCenter,
	TopRight,
	CenterLeft,
	Center,
	CenterRight,
	BottomLeft,
	BottomCenter,
	BottomRight,
};

enum class TexturePreloadingLevel : u8
{
	Off,
	Partial,
	Full,
};

enum class GSScreenshotSize : u8
{
	WindowResolution,
	InternalResolution,
	InternalResolutionUncorrected,
};

enum class GSScreenshotFormat : u8
{
	PNG,
	JPEG,
	WebP,
	Count,
};

enum class GSDumpCompressionMethod : u8
{
	Uncompressed,
	LZMA,
	Zstandard,
};

enum class SavestateCompressionMethod : u8
{
	Uncompressed = 0,
	Deflate = 1,
	Zstandard = 2
};

enum class SavestateCompressionLevel : u8
{
	Low = 0,
	Medium = 1,
	High = 2,
	VeryHigh = 3,
};

enum class GSHardwareDownloadMode : u8
{
	Enabled,
	EnabledForceFull,
	NoReadbacks,
	Unsynchronized,
	Disabled,
	// Appended for wire compatibility (HWDownloadMode is stored as a raw int in the ini and
	// in GameDB). NOTE: because of that, this enum is NO LONGER ordered by "how much readback
	// happens" — never use relational comparisons on it. Use the predicates below instead.
	Asynchronous
};

/// True when the mode performs a real GPU->CPU readback of render targets into local memory.
/// (Asynchronous does the same download, just without making the EE thread wait for it.)
constexpr bool IsHardwareDownloadReadbackEnabled(GSHardwareDownloadMode mode)
{
	return mode == GSHardwareDownloadMode::Enabled ||
	       mode == GSHardwareDownloadMode::EnabledForceFull ||
	       mode == GSHardwareDownloadMode::Asynchronous;
}

/// True when the EE thread services the readback itself instead of waiting on the GS thread.
/// Both modes therefore need the GS thread synchronized around anything that reopens the
/// renderer underneath them. NOTE: this says nothing about *what* gets read — Unsynchronized
/// takes live local memory, Asynchronous takes the mutex-guarded CPU shadow — so it is not the
/// right question to ask about the pipelined front-object split, which cares only about live
/// reads. GS.cpp tests that directly.
constexpr bool IsHardwareDownloadEEThreadRead(GSHardwareDownloadMode mode)
{
	return mode == GSHardwareDownloadMode::Unsynchronized ||
	       mode == GSHardwareDownloadMode::Asynchronous;
}

enum class GSCASMode : u8
{
	Disabled,
	SharpenOnly,
	SharpenAndResize,
};

enum class GSUpscaler : u8
{
	Off,           ///< Plain bilinear present-time stretch (default).
	MetalFXSpatial, ///< Apple MetalFX spatial upscaler (Metal backend, macOS 13+).
	// Appended rather than inserted: this enum is persisted as an integer, so renumbering
	// MetalFXSpatial would silently re-point every existing config at a different upscaler.
	FSR1,          ///< AMD FidelityFX Super Resolution 1 (EASU + RCAS compute passes, Vulkan).
	SGSR,          ///< Qualcomm Snapdragon Game Super Resolution 1 (single compute pass, Vulkan).
	SGSREdge,      ///< SGSR's edge-direction variant: same pass, directional Lanczos, dearer.
};

enum class GSHWAutoFlushLevel : u8
{
	Disabled,
	SpritesOnly,
	Enabled,
};

enum class GSGPUTargetCLUTMode : u8
{
	Disabled,
	Enabled,
	InsideTarget,
};

enum class GSTextureInRtMode : u8
{
	Disabled,
	InsideTargets,
	MergeTargets,
};

enum class GSLimit24BitDepth : u8
{
	Disabled,
	PrioritizeUpper,
	PrioritizeLower,
};

enum class GSBilinearDirtyMode : u8
{
	Automatic,
	ForceBilinear,
	ForceNearest,
	MaxCount
};

enum class GSHalfPixelOffset : u8
{
	Off,
	Normal,
	Special,
	SpecialAggressive,
	Native,
	NativeWTexOffset,
	MaxCount
};

enum class GSNativeScaling : u8
{
	Off,
	Normal,
	Aggressive,
	NormalUpscaled,
	AggressiveUpscaled,
	MaxCount
};

// A hack the player set on purpose. Normally the GameDB gets the last word on these
// unless manual hacks are on, which is all or nothing: switching one hack off throws
// away every automatic fix the game had. Pinning one keeps the rest.
enum class GSUserHackOverride : u8
{
	AlignSprite,
	MergeSprite,
	RoundSprite,
	HalfPixelOffset,
	ForceEvenSpritePosition,
	NativeScaling,
	NativePaletteDraw,
	BilinearHack,
	TextureOffsetX,
	AutoFlush,
	TextureInsideRt,
	// Appended rather than slotted in next to X, so a mask already written to an INI keeps
	// meaning what it meant. Everything below follows the same rule: append only.
	TextureOffsetY,
	PreloadFrameData,
	DisablePartialInvalidation,
	GPUPaletteConversion,
	DisableDepthSupport,
	CPUFBConversion,
	ReadTCOnClose,
	Limit24BitDepth,
	EstimateTextureRegion,
	DrawBuffering,
	CPUSpriteRenderBW,
	CPUSpriteRenderLevel,
	CPUCLUTRender,
	GPUTargetCLUT,
	MaxCount
};

enum class GSDepthFeedbackMode : u8
{
	None      = 0,
	Auto      = 1,
	Depth     = 2,
	DepthAsRT = 3,
};

// GV-7 GS front/back split. Off = today's single-threaded path with no record
// round-trip; InlineRecords = build + execute every record on the calling
// thread (the GV7-0 shape — validation / bisect rung); Lockstep = back thread
// runs but the front drains after every record; Pipelined = the real thing.
enum class GSBackThreadMode : u8
{
	Off           = 0,
	InlineRecords = 1,
	Lockstep      = 2,
	Pipelined     = 3,
};

enum class AchievementOverlayPosition : u8
{
	TopLeft,
	TopCenter,
	TopRight,
	CenterLeft,
	Center,
	CenterRight,
	BottomLeft,
	BottomCenter,
	BottomRight,
	MaxCount
};

// --------------------------------------------------------------------------------------
//  TraceLogsEE
// --------------------------------------------------------------------------------------
struct TraceLogsEE
{
	// EE
	BITFIELD32()
	bool
		bios : 1,
		memory : 1,
		giftag : 1,
		vifcode : 1,
		mskpath3 : 1,
		r5900 : 1,
		cop0 : 1,
		cop1 : 1,
		cop2 : 1,
		cache : 1,
		knownhw : 1,
		unknownhw : 1,
		dmahw : 1,
		ipu : 1,
		dmac : 1,
		counters : 1,
		spr : 1,
		vif : 1,
		gif : 1;
	BITFIELD_END

	TraceLogsEE();

	bool operator==(const TraceLogsEE& right) const;
	bool operator!=(const TraceLogsEE& right) const;
};

// --------------------------------------------------------------------------------------
//  TraceLogsIOP
// --------------------------------------------------------------------------------------
struct TraceLogsIOP
{
	BITFIELD32()
	bool
		bios : 1,
		memcards : 1,
		pad : 1,
		r3000a : 1,
		cop2 : 1,
		memory : 1,
		knownhw : 1,
		unknownhw : 1,
		dmahw : 1,
		dmac : 1,
		counters : 1,
		cdvd : 1,
		mdec : 1;
	BITFIELD_END

	TraceLogsIOP();

	bool operator==(const TraceLogsIOP& right) const;
	bool operator!=(const TraceLogsIOP& right) const;
};

// --------------------------------------------------------------------------------------
//  TraceLogsMISC
// --------------------------------------------------------------------------------------
struct TraceLogsMISC
{
	BITFIELD32()
	bool
		sif : 1;
	BITFIELD_END

	TraceLogsMISC();

	bool operator==(const TraceLogsMISC& right) const;
	bool operator!=(const TraceLogsMISC& right) const;
};

// --------------------------------------------------------------------------------------
//  TraceLogFilters
// --------------------------------------------------------------------------------------
struct TraceLogFilters
{
	bool Enabled;

	TraceLogsEE EE;
	TraceLogsIOP IOP;
	TraceLogsMISC MISC;

	TraceLogFilters();

	void LoadSave(SettingsWrapper& ini);
	// When logging, the tracelogpack is checked, not was in the config.
	// Call this to sync the tracelogpack values with the config values.
	void SyncToConfig() const;
	bool operator==(const TraceLogFilters& right) const;
	bool operator!=(const TraceLogFilters& right) const;
};

// --------------------------------------------------------------------------------------
//  Pcsx2Config class
// --------------------------------------------------------------------------------------
// This is intended to be a public class library between the core emulator and GUI only.
//
// When GUI code performs modifications of this class, it must be done with strict thread
// safety, since the emu runs on a separate thread.  Additionally many components of the
// class require special emu-side resets or state save/recovery to be applied.  Please
// use the provided functions to lock the emulation into a safe state and then apply
// chances on the necessary scope (see Core_Pause, Core_ApplySettings, and Core_Resume).
//
struct Pcsx2Config
{
	struct ProfilerOptions
	{
		BITFIELD32()
		bool
			Enabled : 1, // universal toggle for the profiler.
			RecBlocks_EE : 1, // Enables per-block profiling for the EE recompiler [unimplemented]
			RecBlocks_IOP : 1, // Enables per-block profiling for the IOP recompiler [unimplemented]
			RecBlocks_VU0 : 1, // Enables per-block profiling for the VU0 recompiler [unimplemented]
			RecBlocks_VU1 : 1, // Enables per-block profiling for the VU1 recompiler [unimplemented]
			EnablePerfDump : 1; // Linux: write JIT blocks to perf jitdump (USE_PERF_JITDUMP build only).
		BITFIELD_END

		// Default is Disabled, with all recs enabled underneath.
		ProfilerOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const ProfilerOptions& right) const;
		bool operator!=(const ProfilerOptions& right) const;
	};

	// ------------------------------------------------------------------------
	struct RecompilerOptions
	{
		BITFIELD32()
		bool
			EnableEE : 1,
			EnableIOP : 1,
			EnableVU0 : 1,
			EnableVU1 : 1;

		bool
			vu0Overflow : 1,
			vu0ExtraOverflow : 1,
			vu0SignOverflow : 1,
			vu0ExactMode : 1;

		bool
			vu1Overflow : 1,
			vu1ExtraOverflow : 1,
			vu1SignOverflow : 1,
			vu1ExactMode : 1;

		bool
			fpuOverflow : 1,
			fpuExtraOverflow : 1,
			fpuFullMode : 1,
			fpuExactMode : 1,
			fpuGuardedAddSub : 1; // EE FPU add/sub guard-bit emulation (single-precision fast path). ON by default — the PS2-accurate behavior. Opt-OUT globally via INI for EE-FPU-heavy titles verified to render fine without it (each ADD.S/SUB.S then costs one op instead of the guard sequence). Independent of the clamp tiers: Full mode runs the DOUBLE path, which guards unconditionally regardless of this bit.

		bool
			EnableEECache : 1;
		bool
			EnableFastmem : 1;
		bool
			PauseOnTLBMiss : 1;

		// Cache compiled VU micro-programs to disk and reload them across
		// sessions to cut recompilation stutter on later runs. arm64-only;
		// no-op on x86.
		bool
			EnableVUProgramCache : 1;
		BITFIELD_END

		RecompilerOptions();
		void ApplySanityCheck();

		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const RecompilerOptions& right) const;
		bool operator!=(const RecompilerOptions& right) const;

		u32 GetEEClampMode() const;
		void SetEEClampMode(u32 value);

		u32 GetVUClampMode() const;
	};

	// ------------------------------------------------------------------------
	struct CpuOptions
	{
		BITFIELD32()
		bool
			ExtraMemory : 1;
		BITFIELD_END

		RecompilerOptions Recompiler;

		FPControlRegister FPUFPCR;
		FPControlRegister FPUDivFPCR;
		FPControlRegister VU0FPCR;
		FPControlRegister VU1FPCR;

		CpuOptions();
		void LoadSave(SettingsWrapper& wrap);
		void ApplySanityCheck();

		bool CpusChanged(const CpuOptions& right) const;

		bool operator==(const CpuOptions& right) const;
		bool operator!=(const CpuOptions& right) const;
	};

	// ------------------------------------------------------------------------
	struct GSOptions
	{
		static const char* AspectRatioNames[];
		static const char* FMVAspectRatioSwitchNames[];
		static const char* DisplayRotationNames[];
		static const char* BlendingLevelNames[];
		static const char* CaptureContainers[];

		static const char* GetRendererName(GSRendererType type);

		/// Converts a tri-state option to an optional boolean value.
		static std::optional<bool> TriStateToOptionalBoolean(int value);

		/// Constants for determining default values.
		static constexpr float DEFAULT_FRAME_RATE_NTSC = 59.94f;
		static constexpr float DEFAULT_FRAME_RATE_PAL = 50.00f;

		static constexpr GSRendererType DEFAULT_HW_RENDERER = GSRendererType::Auto;

		static constexpr AspectRatioType DEFAULT_ASPECT_RATIO = AspectRatioType::RAuto4_3_3_2;
		static constexpr GSInterlaceMode DEFAULT_INTERLACE_MODE = GSInterlaceMode::Automatic;
		static constexpr GSPostBilinearMode DEFAULT_BILINEAR_FILTERING_MODE = GSPostBilinearMode::BilinearSmooth;
		static constexpr FMVAspectRatioSwitchType DEFAULT_FMV_ASPECT_RATIO = FMVAspectRatioSwitchType::Off;
		static constexpr GSCASMode DEFAULT_CAS_MODE = GSCASMode::Disabled;
		static constexpr GSUpscaler DEFAULT_UPSCALER = GSUpscaler::Off;

		static constexpr float DEFAULT_UPSCALE_MULTIPLIER = 1.0f;
		static constexpr AccBlendLevel DEFAULT_BLENDING_ACCURACY = AccBlendLevel::Basic;
		static constexpr BiFiltering DEFAULT_TEXTURE_FILTERING_MODE = BiFiltering::PS2;
		static constexpr TriFiltering DEFAULT_TRILINEAR_FILTERING_MODE = TriFiltering::Automatic;

		static constexpr float DEFAULT_OSD_SCALE = 100.0f;
		static constexpr float DEFAULT_OSD_MARGIN = 10.0f;
		static constexpr OsdOverlayPos DEFAULT_OSD_MESSAGE_POS = OsdOverlayPos::TopLeft;
		static constexpr OsdOverlayPos DEFAULT_OSD_PERFORMANCE_POS = OsdOverlayPos::TopRight;

		static constexpr int DEFAULT_VIDEO_CAPTURE_BITRATE = 6000;
		static constexpr int DEFAULT_VIDEO_CAPTURE_WIDTH = 640;
		static constexpr int DEFAULT_VIDEO_CAPTURE_HEIGHT = 480;
		static constexpr int DEFAULT_AUDIO_CAPTURE_BITRATE = 192;
		static const char* DEFAULT_CAPTURE_CONTAINER;

		static constexpr int DEFAULT_SHADEBOOST_BRIGHTNESS = 50;
		static constexpr int DEFAULT_SHADEBOOST_CONTRAST = 50;
		static constexpr int DEFAULT_SHADEBOOST_GAMMA = 50;
		static constexpr int DEFAULT_SHADEBOOST_SATURATION = 50;

		union
		{
			// ⚠️ 133 one-bit flags in 192 bits of array. The flag PAST the array is invisible to
			// OptionsAreEqual, which compares these words and nothing else, so a settings change
			// that moved only that flag would not count as one. Widen the array and add the
			// matching OpEqu row in the SAME commit as the flag that needs it -- 128/128 is how
			// this last ran out.
			u64 bitsets[3];

			struct
			{
				bool
					SynchronousMTGS : 1,
					VsyncEnable : 1,
					DisableMailboxPresentation : 1,
					ExtendedUpscalingMultipliers : 1,
					PCRTCAntiBlur : 1,
					DisableInterlaceOffset : 1,
					PCRTCOffsets : 1,
					PCRTCOverscan : 1,
					IntegerScaling : 1,
					UseDebugDevice : 1,
					UseDebugBlend : 1,
					// Emit per-draw graphics-debugger labels describing the PS2 state.
					// Deliberately separate from UseDebugDevice, which also installs the
					// validation layer and so makes any capture perf-meaningless.
					DebugLabels : 1,
					// Record the per-draw ledger (GSDrawLog). Attribution only -- it is
					// not free, so never leave it on for one arm of an A/B.
					DumpDrawLog : 1,
					// Run the software rasterizer in lockstep against the Tile
					// renderer's native route and record every per-draw divergence
					// (GSTileOracle). Orders of magnitude slower than a plain run --
					// two extra readback drains and a full SW rasterization per native
					// draw -- and meaningless under any renderer but Tile.
					TileDrawOracle : 1,
					// Let the Tile renderer read pages back through an out-of-band
					// copy when the GPU has already finished with them, instead of
					// draining the frame's command buffer for every pull. Default on;
					// the off position is a bisect lever (both roads must be
					// byte-identical), never a user setting.
					TileOutOfBandReadback : 1,
					// Let the Tile renderer gather a palette the game loads off pages a
					// native draw rendered out of the owner's texture on the device,
					// instead of draining the page to load it on the CPU. Default on;
					// the off position is a bisect lever (both roads must be
					// byte-identical), never a user setting.
					TileGpuClut : 1,
					// Let the Tile renderer draw perspective-textured (STQ) TRIANGLES
					// natively instead of flooring them (GSTileFloorReason::
					// TexturePerspective). Default off: the unlock is measured lifted
					// on the corpus and gated on the handoff defect it exposes; the on
					// position is the attribution lever until it ships, and the bisect
					// lever afterwards. Never a user setting.
					TilePerspectiveNative : 1,
					// Lift the read rung's two residual floors (GSTileFloorReason::
					// BlendOverlap / BlendTexSample). Both floors' named causes are fixed
					// (re-measured 2026-08-16); what keeps them is the shared coverage-tie
					// and cross-frame-handoff residues, so the on position is the
					// attribution/perf lever for measuring at full native coverage.
					// Default off. Never a user setting.
					TileBlendOverlapNative : 1,
					TileBlendTexSampleNative : 1,
					// The Classic-parity blend carrier (M4e / #135): serve the
					// variable-carrier clamp-mode blend rows READ-FREE at Classic's
					// own realization and accuracy class (mix/accumulation/HW-rewrite
					// through the ROP, SRC1 where the device has dual-source), instead
					// of routing them to the read rung — whose Adreno realization is a
					// copy plus a pass break PER DRAW, measured carrying 76% of the
					// corpus's blended draws. Bypasses the two blend floors above for
					// the rows it admits (the ROP composites overlap in primitive
					// order). Default off: the shipped path keeps byte-identity to SW;
					// the on position is the perf/attribution lever, in the ceiling
					// profile with the levers above. Never a user setting.
					TileBlendClassicCarrier : 1,
					// Attribution pin for the carrier's no-dual-source realization:
					// the lowering treats the device as having no SRC1 unit, so a
					// variable-As mix row rides its factor through the first
					// output's alpha (Classic's blend_factor_in_alpha fallback)
					// wherever the draw writes no alpha — the Mali shape, made
					// A/B-able on any device. Dev only, default off.
					TileBlendNoDualSource : 1,
					// Perf-ceiling instrument: DROP floored draws (no spill, no SW
					// rasterization). Rendering is wrong by construction; the frame
					// time approximates a 100%-native run. Dev only, default off.
					TileSkipFloorDraws : 1,
					// Design instrument (gsrunner -tilepasssim): score the
					// GS-semantic minimum pass structure of a run — how many pass
					// breaks, snapshots and syncs a backend that keeps every draw
					// on the GPU timeline would be FORCED to take — plus the GIF
					// stream volume the front end decoded. Renders normally;
					// reports at teardown. Attribution arm, never timed. Dev
					// only, default off.
					TilePassSim : 1,
					// The same instrument under the TileGpu variant (gsrunner
					// -tilepasssim arms both): score the GS-semantic minimum pass
					// structure its frames COULD have had, beside the pass structure
					// its planner actually built. Cross-check only -- the planner is
					// fed by the memory model, never by the sim -- and it costs
					// roughly 0.4 ms/frame to feed, so it is off unless a validation
					// run asks for it. Dev only, default off.
					TileGpuPassSim : 1,
					// Validation scaffolding for the TileGpu fragment read-modify-write
					// road: admit EVERY draw the classifier says fixed-function cannot
					// express, rather than only the classes whose consumers have landed.
					// It exists so the declared in-pass read can be exercised over the
					// whole corpus before any accuracy repair rides on it. gsrunner
					// -tilermw arms it; nothing else should. Dev only, default off.
					TileGpuForceSelfRead : 1,
					// Compile each PASS's union fragment program for every draw of
					// it, the way the executor did before the fragment variant
					// joined the indirect run key. Default off; the on position is
					// the forced-vs-narrowed gate (both roads must be
					// byte-identical: a narrower program serving the same draws
					// cannot move a pixel) and the bisect lever afterwards. Never a
					// user setting.
					TileGpuUnionFragmentVariant : 1,
					// Keep the per-draw narrowing of the road, decode arm, self-read
					// use and 16-bit quantise, but stop freezing the per-draw GS
					// state (the alpha test, fog, the texture function, the wrap
					// modes, the filter, the coordinate kind, DATE, TEXA) into the
					// fragment program -- the state row is read at run time again,
					// which is exactly the program set that shipped before the
					// specialization landed. Default off; the on position is the
					// forced-vs-specialized gate's control arm (both roads must be
					// byte-identical, because freezing a value a draw already
					// carries cannot move its pixels) and the bisect lever
					// afterwards. Never a user setting.
					TileGpuUnspecializedFragmentVariant : 1,
					// Serve an upload's sub-block spill by DRAINING the target to the
					// CPU shadow, the way the renderer did before the GPU-side merge:
					// the GS thread blocks on the device, merges the transfer's bytes
					// into the pulled page and hands truth back to the CPU. Default
					// off; the on position is the A/B control arm for the merge (both
					// roads must be byte-identical -- one merges the same bytes on the
					// GPU and never waits) and the bisect lever afterwards. Never a
					// user setting.
					TileGpuUploadSpillReadback : 1,
					// Override the device's answer to "would you rather render MORE
					// passes than see the depth state change inside one?"
					// (GSDevice::TileGpuPrefersDepthUniformPasses). Both off -- the
					// default -- asks the device, which is what ships. Exactly one on
					// pins that polarity; both on is a contradiction and is refused
					// back to the device's answer, loudly, because a lever that
					// silently picks for you is worse than no lever.
					//
					// It exists so a three-arm device A/B runs off ONE binary: the
					// depth key is Adreno's alone (measured, mechanism unnamed until
					// the fragment-variant census found one), and re-measuring it
					// against a build where the fragment program no longer widens
					// when passes merge is the whole point of decoupling the variant
					// from the pass. Dev only; a pass-boundary policy is not a user
					// setting. The renderer reads it ONCE at construction, like every
					// other boundary policy -- a mid-run flip needs a restart.
					TileGpuForceDepthUniformPasses : 1,
					TileGpuForceDepthMergedPasses : 1,
					// Let the PLANNER pick that polarity per frame instead of
					// taking one answer for the whole run.
					//
					// It exists because the corpus answered the flat question and
					// the answer was "it depends on the scene": on an SD865, all
					// 19 dumps, merging wins 7 (Xenosaga -29.5%), ties 5 and loses
					// 7 (Baldur's Gate 2 +7.2%). What separates the two sets is
					// how many passes merging removes PER DRAW -- the winners run
					// 0.084 to 0.841 passes saved per draw, the losers 0.002 to
					// 0.033 -- so the planner counts both groupings of the frame
					// it just built and takes the polarity that number asks for,
					// stickily. See gsTileGpuWantsMergedDepthPasses.
					//
					// DECIDES only where the device asked for uniform passes (the
					// Adreno path) and neither force key above is set -- the force
					// keys stay ABSOLUTE above it. Anywhere else the key still
					// turns on the predictor's CENSUS, which counts both groupings
					// and reports what it would have chosen without moving a
					// boundary, so the thresholds can be checked on a machine that
					// is not the one they were calibrated on. ON by default since
					// the SD865 device gate passed (no losses beyond spread, six
					// wins to -30%, at most one switch per title); off is the
					// escape hatch back to the pre-predictor behaviour -- one
					// polarity for the whole run and no census walks -- and the
					// choice is pixel-inert either way (both polarities render
					// byte-identical frames, proven corpus-wide). Dev only; a
					// pass-boundary policy is not a user setting.
					TileGpuAdaptiveDepthPasses : 1,
					// Construct TileGpu even on a device that FAILS the TileGpu
					// device contract, instead of falling back to Classic.
					//
					// The fallback is the shipped behaviour (GSTileSelectionPolicy.h):
					// a contract-absent device has no executor, so every draw is
					// discarded and the output is black, which is worse for a user
					// than the renderer they did not ask for. This key exists for
					// bring-up on a device whose contract is absent for one term
					// while the rest of the road is being brought up on it -- the
					// pipeline creations and the validation output are the point of
					// the run, not the frame. Fail-closed: off means the fallback is
					// active. Never a user setting.
					TileGpuIgnoreDeviceContract : 1,
					// Hold TileGpu's bindless sampled-target and materialised-source
					// arrays off, whatever the device answers about indexing a
					// sampled-image array by a value the shader computes.
					//
					// Same bring-up shape as the key above and for the same reason:
					// everything the device contract gates comes up in one boot on a
					// device that has only just started passing it, and rule 2 (a draw
					// sampling a resident target) and rule 3 (a draw sampling a
					// materialised source) are the pair worth watching separately from
					// the rest. Read once in CreateDevice, so the shader's defines, the
					// source-array build and the renderer all take one answer -- a
					// shader declaring an array the renderer refuses to bind is the one
					// shape this must never produce. Fail-closed: off means the device's
					// own answer stands. Dev only.
					TileGpuDisableBindlessTargets : 1,
					// Hold TileGpu's in-pass destination read off, whatever the device
					// answers about rasterization-order attachment access.
					//
					// The read rides VK_EXT_rasterization_order_attachment_access, so it
					// is only as trustworthy as the driver's implementation of it. The
					// case this was written for is a proprietary mobile driver that shows
					// run-to-run pixel nondeterminism with the read engaged; the key
					// isolates the read for that A/B. Set, the device answers as if the
					// extension were absent and the renderer takes the road every
					// ROAA-less device already takes: no draw admitted to the read, no
					// declaring pass, snapshot road for DATE. That changes ADMISSION, so
					// the output may legitimately differ from the read-on arm -- this is
					// a diagnostic isolation lever, not a pixel-inert road switch. Read
					// once in CreateDevice like the key above, so the device, the
					// pipelines and the renderer all take one answer. Fail-closed: off
					// means the device's own answer stands. Dev only.
					TileGpuDisableSelfRead : 1,
					// Take away the two things a driver is allowed to do differently with
					// host memory and with a read that leaves its buffer.
					//
					// Set, the device asks for robustBufferAccess (core, and every
					// implementation must offer it), so a storage-buffer read past the end
					// of its binding returns zero instead of whatever the address space
					// holds; and every host-visible stream buffer is REQUIRED to land on a
					// HOST_COHERENT memory type instead of merely preferring one, so a CPU
					// write is visible to the GPU without depending on the flush being
					// correctly ranged and correctly ordered.
					//
					// The case this was written for is a proprietary mobile driver that
					// renders block-shaped garbage texels and does not repeat itself
					// run-to-run, on a device where the same dumps are clean and
					// deterministic under another vendor's driver. Those are the two
					// mechanisms that produce exactly that shape -- an out-of-bounds read
					// whose result is undefined, and a stale page of host memory -- and the
					// key collapses both at once so the device can say whether either is
					// live. It costs performance on every allocation it touches and buys
					// nothing on a correct road; it is an isolation lever, not a fix.
					// Fail-closed: off means today's behaviour, unchanged. Dev only.
					TileGpuStrictMemory : 1,
					// Fill every GPU allocation TileGpu owns with a loud, per-class
					// sentinel at the moment it enters service, so a read of memory
					// nothing ever wrote shows a known colour instead of allocation
					// dirt -- and the COLOUR names the class that was under-covered.
					//
					//   magenta  a byte-slab ring slot, filled when the slot opens for
					//            a plan and before anything composes it. Prefill,
					//            writeback compose and the version copies are supposed
					//            to cover every byte a road then reads.
					//   cyan     a materialised source image, cleared at create and at
					//            the head of every rebuild-in-place, before the build
					//            draw. A sampled region the build never covered shows
					//            through.
					//   yellow   the per-pass snapshot scratch, cleared when it is
					//            fetched from the pool and before its CopyRect. The
					//            copy covers only the intersected source rect, so
					//            anything outside it is the pool's previous tenant.
					//
					// The companion to the key above and the same case: a proprietary
					// mobile driver rendering block-shaped garbage that does not repeat
					// run-to-run, where sync validation is clean. Strict memory asks
					// whether the read leaves the allocation; this asks whether the read
					// is INSIDE an allocation nobody wrote. It makes that mechanism
					// deterministic and self-identifying rather than random -- a
					// mechanism that reproduces as a fixed colour has been caught, and
					// garbage that stays random escaped every allocation named here.
					//
					// Deliberately NOT poisoned: the persistent target pool, whose
					// tenants legitimately carry prior-frame content, and the
					// host-visible stream buffers, where a fill would destroy the CPU
					// writes it landed on. It costs a full-surface write per allocation
					// per frame; it is an isolation lever, not a fix. Fail-closed: off
					// records nothing at all and is the executor byte for byte. Dev only.
					TileGpuPoisonAllocations : 1,
					// Give a view of GS memory that lands INSIDE a live surface's page
					// rectangle that surface, instead of a surface of its own, so the two
					// views share a pass key and the render pass they alternate in stops
					// ending between them.
					//
					// DEFAULT ON, and what it takes is the ZERO-OFFSET half only: a view is
					// admitted where it lands at (0, 0), which is the same base seen under
					// two PSMs of one swizzle family, and every displaced placement is
					// refused however legal the geometry
					// (kGSTileContainDisplacedViews, GSRendererTileGpu.h, which carries the
					// two measured reasons -- the container's writeback byte mask, and a
					// displaced view's reads no longer matching it). Legality is
					// gsTileContainView in the same header.
					//
					// Byte-identical to off on all 21 corpus dumps, five standing spot
					// hashes included. What it buys is the two views no longer stealing each
					// other's pages: -130 writeback ops and -130 seed ops a frame on the
					// Gran Turismo 4 Online Public Beta dump, which is -117.00 render passes
					// a drawn frame (-24.7%), and -1.00 on Dirge of Cerberus. Off is the
					// pre-containment arrangement, byte for byte: every view keeps its own
					// surface.
					TileGpuContainSurfaces : 1,
					// Serve a draw's COLOUR WRITE MASK in the fragment stage instead of on
					// the pipeline, so consecutive draws differing only in FRAME.FBMSK share
					// one pipeline and merge into one indirect call.
					//
					// The mask rides the blend key (GSTileGpuPassPlan::kNoWriteShift), and the
					// blend key is the run key -- so a game that builds a buffer one channel
					// group at a time gets a pipeline bind and an indirect call per draw.
					// Spider-Man 3 does exactly that: 2,841 of 2,992 adjacent in-pass draw
					// pairs are cut by the write mask and by nothing else, the masks rotating
					// r -> rg -> gb -> ba, and taking the mask out of the key collapses 3,624
					// indirect calls to 785 -- every 64x64 pass becomes one call. Measured on
					// the SD865: a call carrying one sub-draw costs 9.90 us, a sub-draw folded
					// into an existing call 2.66 us.
					//
					// PIXEL-INERT, and a difference between the arms is a DEFECT rather than a
					// trade: the fragment stage already applies FBMSK at BIT granularity for
					// the partial-mask road, and a whole-channel mask is a strict subset of
					// that. Order is preserved too -- Vulkan guarantees primitive order across
					// the entries of one vkCmdDrawIndexedIndirect -- so no draw moves.
					//
					// Preserving the unwritten channels in the shader needs the destination
					// READ, so the road takes only draws already doing one -- already
					// evaluating their own blend, or already merging their own FBMSK at bit
					// granularity. It therefore declares no new reader, opens no new declaring
					// pass and charges nothing new to the per-class budget; a draw it cannot
					// serve exactly keeps the pipeline mask and nothing about it moves.
					//
					// Default ON. The device A/B (Spider-Man 3, SD865, 2026-08-26) found it
					// INERT on Adreno at this shape -- -0.6%, because the per-class declaring
					// budget refuses the masked class there, so the population it serves is
					// a handful of draws -- and live wherever the budget does not tax (every
					// non-Adreno device, where it is byte-identical with the population
					// live). Nothing measured says off is faster anywhere: on costs nothing
					// where it is inert and merges runs where it is not. The real Adreno win
					// (~15 ms on that title, isolated by force arms) needs the masked class
					// DECLARED, which is the scoped-admission successor to this lever.
					TileGpuShaderWriteMask : 1,
					// Copy a 256-entry gathered palette out of its owner as ONE 16x16
					// region instead of four 8x8 blocks.
					//
					// The four blocks of a CSM1 32-bit palette are CBP..CBP+3, and where
					// CBP is 4-aligned they are a 2x2 block square — 16x16 texels at a
					// 16-aligned origin, inside one page. Measured on the SD865 (Adreno
					// 650 / Turnip, GT4 Online Public Beta): all 1,251 four-region groups
					// in the frame are exact squares, no exceptions, and per-region copy
					// cost there is FLAT at ~2.8 us whatever the region's size — an 8x8
					// region costs 2.73-2.85 us and a 64x32 one 1.35-1.46 us out of the
					// same image in the same frame. So four regions cost four times one,
					// and the merge is the palette's whole copy bill instead of a quarter
					// of it: 5,010 regions a frame become 1,256.
					//
					// PIXEL-INERT, and a difference between the arms is a DEFECT rather
					// than a trade: what moves is the ORDER the palette's words land in
					// the frame's stream, and the fragment stage reads them back through
					// the matching order (GSTileSwizzleForms::ClutEntryToMergedOffset,
					// pinned against GSClut's own loader). A CBP that is not 4-aligned is
					// not a square at all and keeps the four regions verbatim.
					//
					// Default ON. The device A/B (GT4 Online Public Beta, SD865, 3 reps,
					// 2026-08-26): frame 42.88 -> 35.69 ms with this alone (-16.8%) and
					// 30.61 ms with TileGpuClutMergePages as well (-28.6%), the region
					// counter exact at every arm and byte-identity holding on all 21 corpus
					// dumps. A title that gathers no palette never reaches the arm; the one
					// price it pays is the merged fetch's ~300 SPIR-V words in the widest
					// paletted variants (gs_tilegpu_shader_budget_tests holds them under the
					// a650 cliff), unmeasured on those titles and accepted.
					TileGpuClutMergeRegions : 1,
					// ...and where a RUN of such palettes tiles whole rows of one owner page, copy
					// the whole 64x32 page in one region and read them all out of it.
					// A no-op unless TileGpuClutMergeRegions is also on, and a separate
					// bit so the device A/B can bisect the two.
					//
					// A palette's 16x16 square is one eighth of a 64x32 owner page, so a
					// game that renders a bank of palettes into a strip of the frame
					// buffer and cycles it -- which is what GT4 does -- presents eight
					// squares tiling one page. Measured on the same capture: all 139 of
					// the frame's 32-region copy commands have their eight squares tiling
					// page (0,0) of one image exactly.
					//
					// The merge is a plan-build pass over the finished op array
					// (MergeClutCopiesIntoPages, after the runs are spliced): a CONSECUTIVE
					// run of merged squares from one owner, in one page, reserving
					// contiguous stream slots and covering WHOLE rows of the page's 4x2
					// square grid becomes one 64x16 or 64x32 region, and the draws reading
					// those palettes are re-pointed at their squares' origins inside it at
					// stride 64. Whole rows is forced, not tuned: n squares reserved n*256
					// words and a 64x16h region writes 1024h, which agree only at n == 4h;
					// a partial cover would write past the run into the next reservation
					// (gsTileGpuClutPageBand, its own test). It was NOT built where it was
					// first designed -- grouping at the source-overwrite site -- because
					// that site only ever sees one 256-entry record at a time (the gather
					// admits eight-bit palettes only at CSA 0, and a CSA-0 load restamps
					// every mirror slot); the eight-square runs a capture shows are eight
					// consecutive draws the executor batches into one command.
					//
					// Pixel-inert by the same argument as the square merge: the band is
					// exactly its squares' words, every one addressed by its own palette's
					// fetch. Default ON, on the same A/B as TileGpuClutMergeRegions: it is
					// the -16.8% -> -28.6% step on GT4 Online Public Beta, faster than
					// regions-only on both wall and GPU time.
					TileGpuClutMergePages : 1,
					// Gather a CLUT palette whose owner is a SIXTEEN-BIT colour target,
					// which the road refuses today.
					//
					// A CSM1 32-bit palette read out of a PSMCT16/PSMCT16S surface is a byte
					// REINTERPRETATION: the surface stores two 5551 cells to a guest word, so
					// a palette word is two texels — and they are texel (cx, cy) and
					// (cx + 8, cy), because columnTable16[y][x] is 2*columnTable32[y][x] and
					// columnTable16[y][x + 8] is that halfword plus one. So the copy goes out
					// twice as wide, the fetch reads `off` and `off + 8`, and each is packed
					// with gsTilePack5551 — which is GSLocalMemory::WriteFrame16 verbatim, and
					// therefore the same bytes the readback road would have stored.
					//
					// The pull road existed only because the OWNER PREDICATE refused 16-bit
					// owners: every road that reads an owner's texture reads it through CT32
					// block and column forms, so one predicate said "not CT32" and the whole
					// load fell back to a CPU readback. That is the entire refusal. Spider-Man
					// 3 loads 162 of them a frame — 100% of its CLUT refusals, all of which
					// would be served — and they are the head of the title's whole blocking
					// bill: 172 CLUT stalls poison m_cpu_read_pages, which then refuses 42
					// upload merges a frame, and the two together are 214 pool calls and
					// 54 ms of a 94 ms frame (SD865, standing suite round 20260826-0157).
					//
					// A 256-entry palette is four blocks at CBP..CBP+3, and where CBP is not
					// 4-aligned they are scattered rather than square. They still land as ONE
					// 32x16 tile, through the copy op's per-region destination offsets, so the
					// consumer has one word order and not two — which is not an optimization
					// here: every one of Spider-Man 3's 256-entry loads sits at CBP & 3 == 1
					// and none at 0, so the scattered case is the only case on the title the
					// lever exists for. A 16-entry palette is one 16x2 rect at stride 16.
					//
					// SHADER BUDGET. The fetch arm is its own variant bit
					// (kGSTileGpuTexelPalGather16) rather than a mode inside the 32-bit one,
					// so a pass gathering a 16-bit palette compiles it INSTEAD of modes 1-3.
					// It costs a flat 920-924 SPIR-V words — about eight Adreno 650 units — on
					// every paletted variant, and every variant this corpus plans fits with
					// room (Spider-Man 3 draws byte[IDX4] / byte[IDX8] / byte+source, 104 to
					// 113 units against a 123-unit cliff). What does NOT fit is the arm on a
					// pass that also samples a resident target directly: byte+target[IDX4] is
					// 118 units and byte+target+source[IDX4] is 123 with nothing left. So the
					// renderer refuses to put both in one pass and SPLITS the pass instead —
					// see gs_tilegpu_shader_budget_tests, which holds the whole plannable set
					// under the ceilings with that rule assumed. On Spider-Man 3 the split
					// never fires: 16 of its 43,747 draws a frame take the target road.
					//
					// Default TRUE. HONEST COVERAGE: one dump exercises it hard and one
					// barely. Per-frame psm-clause refusals over the 21-dump corpus are
					// Spider-Man 3 162.00, LEGO Star Wars 0.25 and zero everywhere else, so
					// nineteen dumps are identity by construction and prove nothing — but
					// Spider-Man 3 exercises it hard, since those palettes feed the
					// byte[IDX4]/byte[IDX8] variants that carry ~63% of its draws, and it is
					// byte-identical there. GT4's CLUT stalls are the multi/partial clause and
					// dirge's are a depth owner; neither is served by this and both are
					// separate designs.
					//
					// ⚠️ LEGO STAR WARS IS NOT BYTE-IDENTICAL, and the cause is NOT the merge.
					// RenderDoc shows the merge's bytes are exact. A dedicated investigation
					// refuted the old merge-defect story here; its "proof" was invalid because
					// TileGpuUploadSpillReadback ON silences the gather AND the merge
					// together, so it could not separate them. The real cause is a
					// PRE-EXISTING alpha-test-fold unsoundness this lever exposes: leaving the
					// palette on the GPU makes GSTileClutMirror::AnyUnsynced() true over the
					// pages the PSMCT32 FBW-1 transfer wrote into the PSMCT16 FBW-8 owner
					// (pages 389 and 391), so AccumulateDraw skips GetAlphaMinMax() for two
					// reflection draws (d134, d135). The fold then sees alpha bounds 0..255,
					// classifies the test "Varies", and the live test DISCARDS failing
					// fragments, which is unsound whenever AFAIL is not KEEP since RGB_ONLY
					// must still paint colour and drop only alpha/depth. Those two draws
					// vanishing is 48,590 of the 52,408 diverging pixels. Filed as a TileGpu
					// unsoundness, not a merge bug; the fix is a sound GPU "varies" road: an
					// in-pass destination-alpha read, or depth handled without a sync stall,
					// never sync-on-demand. The upload numbers this section used to cite (1.50
					// -> 0.00 upload stalls a frame, 0.25 -> 1.75 pages merged) still happen
					// once the CLUT pulls stop poisoning m_cpu_read_pages; they were never
					// evidence about correctness. Suite-quantified cost of shipping this ON:
					// worst-wrong-pixel +3.32pp SD865, +4.19pp rg477v, +3.33pp local, uniform
					// across all three roots.
					//
					// ⚠️ Device A/B DONE (SD865, 2026-08-30, record
					// devs/bmdhacks/perf/tilegpu-clut16-sm3-sd865-18aac48637/ in the
					// umbrella). Spider-Man 3 frame p50 92.54 -> 61.42 ms (-33.6%), GPU 54.47
					// -> 40.18 ms, GS-thread CPU 39.60 -> 28.97 ms, sync wait 23.28 -> 1.32
					// ms. Out-of-band wait is UNMOVED: 28.78 -> 28.60 ms (173 -> 52 calls at
					// 0.17 -> 0.55 ms each). Total blocking 55.16 -> 33.10 ms. The frame is
					// left CPU-thread-bound (29 + 33 ≈ 61); the remaining 52 pulls a frame are
					// 42 upload spills plus 10 multi/partial CLUT loads, the population round
					// C then attacked.
					TileGpuClut16Gather : 1,
					// Ask the CLUT gather's owner question about the palette's OWN BLOCKS
					// rather than about whole pages, and stop pulling a block nothing reads.
					//
					// The gather refuses a load when no single live target holds GPU-newest
					// truth for every plane of every page the palette sits in. A palette is
					// one to four blocks of a 32-block page, and the rest of that page is
					// nothing to do with it: a target that rendered the palette and then had
					// part of its page written back to the CPU fails the page question with
					// every byte the palette needs sitting in its texture. This asks
					// GSVramModel::SoleGpuOwnerOfBlocks instead — one owner over the blocks
					// the loader reads — which is the same all-or-nothing rule in the unit
					// the model already tracks truth in. Nothing about the copy geometry,
					// the stream layout or the shader moves; LocateClutBlocks already
					// addresses blocks.
					//
					// AND the second call is held. GSState invalidates TWO blocks for a
					// four-bit-index CSM1 palette (GSState.cpp's `blocks` loop) and the
					// loader reads sixteen entries — 64 bytes — out of the FIRST one, so the
					// second block is over-invalidation nothing reads. Today it refuses the
					// load and its bytes get pulled off the device; here its verdict is held
					// until TEX0 says whether the palette includes it, and for a sixteen-
					// entry palette it is simply dropped. That one block is most of the
					// prize: on Spider-Man 3 the block question serves 1.12 loads a frame
					// over the invalidated blocks and 50.00 over the read ones.
					//
					// Population, per drawn frame, from the census this shipped with
					// (multi/partial refusals that would be served, over the read blocks):
					// Spider-Man 3 50.00 of 50.00 (47.75 off a 16-bit owner), dirge 1.00 of
					// 1.00, yugioh 0.38 of 0.38. GT4 0.00 of 25.75, GT4 Online Public Beta
					// 0.00 of 2.62, MGS3 0.00 of 0.50 — their palettes are 256 entries over
					// four blocks of which only some are GPU truth, which is a stitch across
					// GPU and CPU bytes and no narrowing of the unit reaches it. What they
					// DO get is the held call: GT4 Online Public Beta's 2.62 CLUT stalls a
					// frame are entirely the over-invalidated block, and drop to 0.00.
					// Fifteen dumps have no multi/partial population at all and are identity
					// by construction.
					//
					// ⚠️ DEFAULT TRUE. The pre-registered SD865 decider fired at -17.1% with
					// gs_cpu FALLING (the device numbers are below, after the M2 table). The
					// M2 numbers that follow are kept as the record of why the M2 was not the
					// authority for this trade, not as a live warning: on the M2 the lever
					// removes every blocking wait it was built to remove and the frame still
					// got slower there, because of what the removal UNLOCKS. Spider-Man 3, M2
					// Max under Honeykrisp, per drawn frame, off -> on:
					//
					//   CLUT owner refusals   50.00 -> 0.00      (all of them)
					//   palettes gathered    168.00 -> 553.00
					//   pool calls the stalls issued 83.75 -> 0.00
					//   BLOCKING GPU WAITS    84.25 -> 0.50 /frame
					//     of which out-of-band 81.75 (24.69 ms) -> 0.00 (0.00 ms)
					//   upload sub-block stalls 33.75 -> 0.00
					//   readbacks (8-frame run)  673 -> 3
					//   ---- and the bill ----
					//   upload merge served   40.00 -> 173.25 pages
					//   render passes (run)     6951 -> 9026
					//   render pass area  430 Mpx -> 561 Mpx
					//   mid-frame flushes   109.25 -> 192.50
					//   GS thread CPU per draw  6.87 -> 11.58 us
					//   frame p50 (median of 3)  76.5 -> 92.3 ms
					//
					// ⚠️ The us-per-draw row is the one that drove the M2-era prediction, and
					// the prediction was wrong. The bill is not GPU time, it is GS THREAD
					// time -- +175 ms over an eight-frame run, ~22 ms a frame, building the
					// merge's writeback and seed ops, on the M2. The read at the time was "an
					// A77 will not do that work faster than an M2 Max does, so the round trips
					// cost more on a phone" -- the SD865 decider below says otherwise: gs_cpu
					// FELL on the device this bill was supposed to hit hardest.
					//
					// The bill is the UPLOAD MERGE's, not the gather's. A CLUT pull marks
					// its pages as CPU-wanted and that mark is what keeps the upload merge
					// off them (TileGpuMergeCpuReadWindow); serve the palette on the GPU and
					// the mark never happens, so the merge takes 4x the pages and its
					// writeback-plus-seed per page splits the frame into 2,075 more render
					// passes. That is the same coupling round C1 measured from the other
					// side, where lifting the mark cost 150 CLUT stalls a frame. Removing
					// the pulls is worth ~25 ms of device round trips and costs 2,075 passes
					// on a tiler, and the two do not have the same price on the two rigs --
					// which is exactly why this is a key and not a rewrite.
					//
					// The census's cheap early-out survives: sotc runs 1,788 CLUT loads a
					// frame that need nothing, none of them build a footprint, and its GS
					// thread CPU per draw is 16.35 -> 16.51 us with the key ON -- inside the
					// +2% the design set as the kill line, and its pass, draw and readback
					// counts do not move at all.
					//
					// Also measured, and not noise: Yu-Gi-Oh loses 11.6 ms on ONE frame of
					// eight (M2, 4.5 -> 16.1 ms). It gathers 0.38 palettes a frame and then
					// meets a load shape the sixteen-slot mirror cannot model, which makes
					// the CLUT RAM whole through SyncClutToCpu -- a drain the off arm never
					// paid because it had nothing on the device to sync. Its pass count,
					// draw count, readback count and blocking waits are otherwise identical
					// between the arms. This now ships: the cost is real and is priced by
					// the next standing suite, not waived away.
					// dirge, GT4, GT4 Online Public Beta and MGS3 are inside the rig's noise
					// (God of War II, whose counters are byte-identical on both arms, moves
					// 21.6 -> 24.4 ms between runs).
					//
					// ⚠️ SPIDER-MAN 3 IS NOT BYTE-IDENTICAL with this on (79e8a170 ->
					// 56bdab91; 1,192-1,914 pixels of 307,200 a frame, max channel delta 14,
					// alpha never). The gathered PALETTE WORDS are not the cause and that is
					// proved twice: a runtime oracle compared 1,500 live gathered records
					// against a real GSClut load of the same palette out of guest memory made
					// fresh and found zero wrong entries; and holding the readback constant
					// in both arms -- so stalls, merges and plan structure are identical and
					// only the road the consumers read the words by differs -- leaves 4
					// pixels in one frame of four. The rest follows the upload merge going
					// 40 -> 173 pages, which is the same road round C1 filed a byte defect
					// in on this same title. The other 20 corpus dumps are identical. This is
					// a FILED DEFECT on the upload-merge-composition road, not a reason to
					// hold the key back -- per the campaign's standing rule, a mover ships as
					// a filed defect, it is never a reason to keep a proven-faster lever off.
					// It ships ON with this key.
					//
					// ⚠️ Device A/B DONE (SD865, 2026-08-30, record
					// devs/bmdhacks/perf/tilegpu-roundc-sm3-sd865-1069742aed/ in the
					// umbrella). Spider-Man 3, OFF vs ON, per drawn frame: frame p50
					// 60.787 -> 50.412 ms (-17.1%, spread <0.5 ms across 3 reps/arm), gs_cpu
					// 29.54 -> 26.68 ms (CPU FELL -- the M2's predicted +~22 ms/frame of
					// merge-op building did NOT reproduce on the A77), total blocking
					// 32.53 -> 21.59 ms, out-of-band wait calls 52.08 -> 0, CLUT stalls
					// 10.00 -> 0, upload sub-block stalls 41.95 -> 0, readbacks 52.15 -> 0,
					// merge pages served 24.80 -> 189.85, mid-frame flushes 56.85 -> 211.30.
					// The pre-registered decider fired: this is what settles the default, not
					// the M2 table above it.
					TileGpuClutBlockGather : 1,
					// Do not read back the block GSState invalidates for a CLUT load that
					// the CLUT loader never reads.
					//
					// GSState::ApplyTEX0 calls InvalidateLocalMem once per block over a
					// four-block default, halved for a 16-bit palette and halved again for a
					// four-bit index — so a four-bit index with a 32-bit palette gets TWO
					// calls, and GSClut::WriteCLUT_T32_I4_CSM1 reads sixteen entries, 64
					// bytes, out of the FIRST of them. The second block is guest memory
					// nobody reads, and where a target holds it that is a submit and a fence
					// wait for nothing. gsTileGpuClutBlocksInvalidated and
					// gsTileGpuClutBlocksRead are the two counts, and the suite pins that
					// they differ on exactly one shape and by exactly one block.
					//
					// It cannot be decided where GSState asks, because TEX0 has not arrived
					// yet and the load could equally be an eight-bit index whose four calls
					// are all the palette's. So the second call is HELD: its verdict is
					// recorded, its readback is not taken, and PreClutLoad settles it once
					// the entry count is known — dropped where nothing reads it, rejoined
					// with its readback taken there where something does. Deferring a
					// readback is always safe: truth does not move, so any later reader of
					// those bytes still pulls them.
					//
					// SEPARATE KEY from TileGpuClutBlockGather on purpose. That one is an
					// admission question and ships off; this one only ever removes work, and
					// it is the whole of GT4 Online Public Beta's CLUT stall count: 2.62
					// stalls a frame, every one of them the over-invalidated block, and its
					// palette's OWN block is the CPU's and needs nothing. Per drawn frame,
					// M2 Max / Honeykrisp, off -> on: CLUT owner refusals 2.62 -> 0.00, pool
					// calls 3.62 -> 1.00, blocking GPU waits 4.12 -> 1.50, of which
					// out-of-band 2.62 at 3.87 ms -> zero. On the SD865 those pulls were
					// priced at 2.4-2.6 ms each, which is why this is not a rounding error
					// there even though the M2 frame time cannot resolve it.
					//
					// GT4 itself gets 0.87 of its 25.75 CLUT stalls a frame back the same
					// way, and it is the only other title that moves at all: Spider-Man 3,
					// dirge and Yu-Gi-Oh are unchanged to the digit, because their held
					// second block is one no target holds and its readback was already free.
					//
					// Default TRUE. All 21 corpus dumps byte-identical, and identical with
					// the key OFF as well -- it changes which bytes are PULLED, never which
					// bytes are read.
					//
					// ⚠️ It sits below the road's device probe, so a device that cannot serve
					// the CLUT gather at all does not hold either. That is the configuration
					// it was measured in and nothing is claimed beyond it.
					TileGpuClutHoldSecondCall : 1,
					// Submit the frame's recorded work MID-PLAN, at a pass boundary, on
					// the frames that read back — the non-blocking kick the Classic
					// renderer has had in DoRenderHW and the TileGpu executor has not.
					//
					// ExecuteTileGpuPassPlan records the whole plan into the frame's
					// command buffer and submits nothing until something forces it. So a
					// pull's source image has been touched by the recording (unsubmitted)
					// buffer, GetGpuTouchCounter() equals the current fence counter,
					// CopyFromCompletedTexture refuses, and the pull falls off the cheap
					// out-of-band road onto the DRAIN road: submit the whole recorded plan
					// and block on the frame fence. Measured on Spider-Man 3 (SD865,
					// standing suite round 20260826-0157): 51 of 206 pulls a frame take
					// that road at ~1.07 ms each — 54.5 ms of a 94 ms frame — against
					// ~37 us for an out-of-band round trip.
					//
					// The kick submits at a pass boundary so the GPU starts executing while
					// the GS thread is still recording; the drain COUNT barely moves
					// (Spider-Man 3 52 -> 47 drains a frame on the SD865, the same on the
					// M2) because the pull that follows a pass reads that very pass, so its
					// source is touched by the recording buffer whatever was kicked before
					// it — what the kick buys is a head start on the drain's PRICE, and the
					// out-of-band pulls behind it then queue behind the kicked submissions.
					// It never blocks — it fires only when the NEXT command buffer has
					// already retired, the gate Classic's comment records as declining
					// ~3,300 of ~3,400 offers on Rogue Galaxy. A kick that blocks is worse
					// than no kick.
					//
					// PIXEL-INERT by construction: what changes is WHEN recorded work is
					// submitted, never what is recorded. A byte difference between the arms
					// is a pre-existing ordering defect, not a trade.
					//
					// Default ON. Device A/B, SD865, 3 reps/arm, 2026-08-30 (dossier
					// changelog, record perf/tilegpu-readback-kick-sd865-92f0e375d8): gt4opb
					// 32.04 -> 27.35 ms (-14.6%), gt4 26.71 -> 23.29 (-12.8%), Spider-Man 3
					// 94.66 -> 91.53 (-3.3%); readbacks and blocking-wait counts identical
					// both arms on all three; GPU time flat. It is a WAIT-OVERLAP lever: it
					// wins where the pull chain is loose and cannot shorten Spider-Man 3's
					// chain, which is serial (GPU busy 54 ms, GS thread blocked 55 ms of a
					// 92 ms frame, zero overlap; ~50 pass->pull->pass dependencies a frame).
					// Costs that came with it: ~85 kicks a frame there = +5 ms of GS-thread
					// submit overhead, and a 3-4 ms ring wait; both are tuning (kick
					// threshold), queued behind removing the pulls themselves (the 16-bit
					// CLUT gather), which is the only thing that helps that title.
					TileGpuKickReadbackFrames : 1,
					// Serve an alpha test the plan could not decide EXACTLY, by splitting the
					// draw, instead of discarding the fragments that fail it.
					//
					// ⚠️ THE DEFECT. A raster fragment stage answers a live alpha test by
					// discarding the failing fragment. That is exact under AFAIL=KEEP and
					// wrong under the other three: RGB_ONLY paints the failing fragment's
					// colour and drops only its alpha and depth writes, FB_ONLY paints the
					// whole colour and drops the depth write, ZB_ONLY writes depth and drops
					// the colour. So "Varies" was never the conservative answer to "what is
					// this draw's alpha test" — it is a SECOND APPROXIMATION, and any caller
					// that widens an alpha bound it cannot compute walks into it. That is
					// what cost LEGO Star Wars both of its floor-reflection draws, 48,590 of
					// 52,408 diverging pixels, the day the CLUT gather started leaving
					// palettes on the device and the fold stopped being able to bound them.
					//
					// The exact realization needs NOTHING from the device — no dual-source
					// blending, no in-pass destination read — and both of this tree's other
					// renderers already ship it: Classic as `split_rgb_only`
					// (GSRendererHW.cpp) and the shared Tile lowering as `atst_split`
					// (GSTileDrawLowering.h). Under RGB_ONLY every fragment writes RGB and
					// only the passing ones write A and Z, so one pass with the test OFF
					// writing RGB and one with the test ON writing A and Z is exact — and
					// strictly better than splitting by pass/fail, which puts RGB in both
					// passes and composites overlapping primitives out of order. FB_ONLY is
					// the same split with the whole colour in the first pass. ZB_ONLY cannot
					// be split by channel (the depth its first pass writes is what the second
					// would test against), so it splits by FRAGMENT on the inverted
					// comparison. gsTileGpuPlanAlphaSplit is the one decision and
					// gs_tilegpu_alpha_test_fold_tests sweeps its whole domain against the
					// console's AFAIL table, per fragment alpha.
					//
					// ONE shape is refused and keeps the discard, with its own census line:
					// a ZTST=ALWAYS draw that writes depth. Both split shapes give one of
					// their passes no depth write, and with no depth TEST either that pass
					// carries no depth ATTACHMENT — which its partner needs, and which a
					// render pass cannot gain or lose in the middle. Zero draws a frame on
					// the whole corpus.
					//
					// The ORDER PROOF (Classic's independent_z / independent_rgb, plus DATE
					// which on this renderer can read the live pixel) LABELS the result
					// rather than gating it. It cannot be met on a triangle draw — the
					// overlap test answers YES or UNKNOWN for every one of them on this
					// corpus — and LEGO Star Wars' floor reflection is a triangle draw, so a
					// road that refused there would not fix the defect it was written for.
					// Whole-frame mean absolute error against Classic, four frames, splitting
					// the unproved draws vs refusing them: LEGO Star Wars 8.377 -> 1.764,
					// R&C UYA gameplay 5.088 -> 4.717, FlatOut 2 12.381 -> 11.140, R&C UYA
					// effects 7.561 -> 7.208, SotC 6.248 -> 6.190; dirge, God of War II and
					// GT4 smaller; AC3 0.3869 -> 0.3884.
					//
					// ⚠️ MGS3 GOES THE OTHER WAY AND IS A FILED DEFECT, NOT A REASON TO HOLD
					// THIS OFF: 5.455 -> 8.011. Its 44.5 FB_ONLY triangle draws a frame have
					// primitives that occlude each other, so the colour pass paints fragments
					// a later primitive of the same draw would have hidden. Two other
					// realizations were measured and are worse than this one on EVERY title
					// including MGS3 — Classic's pass/fail (MGS3 14.772, SotC 7.540, LEGO
					// 4.400) and the same channel split with the depth pass emitted first
					// (MGS3 15.390, LEGO 4.484). The next move on that population is a
					// per-draw rule, and the census's exact-versus-reordered line is what it
					// hangs off.
					//
					// COSTS AT MOST ONE EXTRA DRAW on the draws it serves, and no extra pass
					// except where the device asked for depth-uniform passes (Adreno) and the
					// two halves carry different depth variants. Nothing else moves: the
					// second draw is one more indirect command over the SAME index range, the
					// way the dual-source alpha companion already is.
					//
					// Default TRUE. The population is the corpus's varying non-KEEP AFAIL
					// draws whose failing fragments the discard would drop — 531 a frame on
					// R&C UYA's effects scene, 421 on its gameplay one, 124 SotC, 64 MGS3, 50
					// God of War II, 46 dirge, 18 FlatOut 2, 14.6 LEGO Star Wars, 5 GT4, 5
					// Ace Combat 5, 2 AC3, 2 GT4 Online Public Beta. Seven dumps have none
					// and are byte-identical by construction, and OutRun 2006's 19.9 varying
					// draws a frame are the shape whose two sides write the same channels, so
					// they never carried a test and are byte-identical too. OFF reproduces
					// the pre-split tree hash for hash on all 21.
					TileGpuAfailSplit : 1,
					// The alpha split's second half keys its render pass like its
					// principal, so the split cuts an indirect RUN and never a PASS.
					//
					// The two halves differ in exactly one pipeline bit — the depth
					// write-enable — and on a device that asked for depth-uniform
					// passes (Adreno) that bit is in the PASS key, so every split draw
					// opened a pass going in and another coming out. The difference
					// does not need to be there: GSTileGpuRunKey carries the depth mode
					// unconditionally, so it already cuts a run and binds the second
					// half's own pipeline whatever the pass key says. Under this key
					// both halves key on the DRAW's own depth write — the write the
					// draw lands in GS memory, which the two halves union back to — and
					// only the run cut remains.
					//
					// ⚠️ WHAT IT COSTS: a pass on Adreno now holds a two-draw depth-mode
					// excursion, which is the one thing depth-uniform keying exists to
					// avoid. That is the trade, and the alternative was measured: R&C
					// UYA's effects scene under forced-uniform keying with the split on
					// goes 454 -> 1496 render passes a frame and 80 -> 302 Mpx of
					// declared area. Which is why the per-frame depth predictor escaped
					// to MERGED keying on both R&C dumps the day the split landed — and
					// merged is itself a measured loss on Adreno, so escaping cost 5.14
					// ms a frame of ring backpressure. Dirge of Cerberus' 46 splits a
					// frame are too few to move the predictor's ratio, so it paid the
					// +50 passes and +11.5 Mpx directly.
					//
					// PIXEL-INERT, and by the same argument uniform keying rests on:
					// where a pass boundary falls decides nothing about pixel results —
					// a pass carrying two depth modes is what MERGED keying does on
					// every non-Adreno device all day. Uniform is a performance
					// preference (GSRendererTileGpu.h), not a correctness one. Both
					// arms are byte-identical on all 22 corpus dumps under the shipped
					// (merged) keying AND under TileGpuForceDepthUniformPasses, which
					// is the arm where the pass merge actually happens.
					//
					// ⚠️ THE M2 CANNOT SEE THIS KEY AT SHIPPED DEFAULTS, because
					// Honeykrisp asks for merged keying and the depth mode is then out
					// of the pass comparison anyway. Forcing uniform is what makes the
					// mechanism observable off-device, and it reproduces the device's
					// numbers to the pass — render passes a frame, key off -> on with
					// TileGpuForceDepthUniformPasses=true: R&C UYA effects 1496.25 ->
					// 454.25 (302.4 -> 80.4 Mpx of declared area), R&C UYA gameplay
					// 912.00 -> 83.00, MGS3 719.00 -> 472.00, SotC 787.25 -> 596.25,
					// dirge 1163.50 -> 1117.50, LEGO Star Wars 63.12 -> 49.50,
					// Stuntman 2756.80 -> 2685.40. Every one of those ON figures is the
					// pass structure the tree had before the split landed. What the
					// device reads at shipped defaults is the DECISION: the per-frame
					// depth predictor's verdict returns to 0 merged frames on R&C UYA
					// effects, R&C UYA gameplay and God of War II, and nothing else in
					// the corpus moves.
					//
					// PRE-REGISTERED DEVICE TARGET, so a later reading cannot be fitted
					// to whatever came back. SD865 p50, shipped tip -> this key on:
					// dirge 24.51 -> 22.9-23.2, R&C UYA effects 23.15 -> 22.9-23.2,
					// R&C UYA gameplay 12.10 -> 12.0-12.2, MGS3 15.25 and SotC 16.45
					// within spread, everything else flat. Ace Combat 5 (7.89) and
					// Yu-Gi-Oh (6.41) must NOT move at all.
					//
					// ⚠️ ONLY DIRGE IS PREDICTED TO RECOVER, AND THAT IS NOT THE PRICE
					// REPORT'S NUMBER. It predicted 3.5-5.0 ms off R&C UYA effects on
					// the reading that the merged state was most of that title's bill.
					// The device has since priced that state directly: the peer's
					// alpha-split A/B carried a third arm, split OFF plus
					// TileGpuForceDepthMergedPasses, and it came back at 16.229 ms
					// against 16.274 for split OFF under uniform keying. Merged costs
					// R&C UYA effects nothing. Its steady-state pass count and declared
					// area are the same on both arms already (466 passes and 85.8 Mpx
					// with the split, 469 and 86.5 without), so there are no split pass
					// breaks there for this key to remove -- the predictor had already
					// escaped them, and escaping was free. What remains is 735 extra
					// draw calls a frame of re-rasterized fill and the 5.21 ms of ring
					// backpressure that comes with it, and neither is this key's.
					//
					// Dirge is the title whose predictor never flipped, so it pays the
					// pass breaks directly and this key removes them: steady state 1165
					// -> ~1118 render passes a frame and 165.4 -> ~154.7 Mpx, against
					// only 48 extra draw calls, which is why nearly all of its +1.80 ms
					// is expected back.
					//
					// THE STRUCTURAL PREDICTIONS ARE THE DISCRIMINATING ONES and they
					// are billing-independent: both R&C dumps' depth predictor returns
					// to 0/40 merged frames, their two ~1500-pass warm-up frames go
					// away, and dirge's steady-state render_passes returns to ~1118.
					// Device timing numbers PENDING; the structure is what falsifies.
					//
					// Default TRUE. OFF is the shipped-tip road, byte for byte.
					TileGpuSplitSharesPassKey : 1,
					// Let ONE seed render pass repair a batch of the upload merge's pages,
					// instead of one render pass per page.
					//
					// A writeback is one compute dispatch with the page count in its Z, so
					// it costs the same whether it composes one page or forty. A SEED is a
					// full Vulkan render pass, and the merge emitted one PER PAGE for a
					// single structural reason: the blocks a seed may write rode in
					// `seed_blocks`, an OP-LEVEL push constant, while everything else about
					// a seed op was already per-op-general (the op carries a 512-bit page
					// mask precisely so one draw can serve any page subset). The merge is
					// the only caller that needs a DIFFERENT block mask per page — it is
					// repairing the blocks one surface holds after a CPU transfer landed in
					// them, and the rest of the page is the CPU's — so it emitted one op per
					// page to say so.
					//
					// The mask moves to the page ENTRY, which already carried an unused
					// `block_mask` field, and the seed shader reads it per page out of a
					// small per-page table beside the op's page mask. One op then names any
					// number of pages, each narrowed to its own blocks.
					//
					// PIXEL-INERT BY CONSTRUCTION and a difference between the arms is a
					// DEFECT, not a trade: the (page, block) pairs the seed writes are
					// identical, and GSRendererTileGpu::MergeSeedMaskFor states the two
					// roads' one difference as a single function so the equality can be
					// tested without a device (gs_tilegpu_upload_merge_tests). What changes
					// is how many render passes carry them, and — for free — how many times
					// the merge bumps the owner's surface version, which is what invalidates
					// every donor/source content token and every gathered palette id keyed on
					// it.
					//
					// ⚠️ MEASURED ON THE CORPUS, AND IT IS NOT WHAT THE DESIGN STUDY
					// PREDICTED. The merge's groups are ONE PAGE EACH on every title that
					// reaches the road — Spider-Man 3 173.25 groups / 173.25 pages a frame,
					// GT4 10.00 / 11.00, Dirge of Cerberus 3.62 / 3.62 — because a group is
					// per owner within ONE host->local transfer, and those transfers are
					// single-page. So this collapses 173 seed render passes to 173 on
					// today's road and the frame does not move. It ships ON anyway because
					// it is the ENABLER: batching the merge's seeds ACROSS a run of
					// transfers is what removes the passes, and that road cannot exist
					// until a seed op can carry a mask per page. The blocker for it is
					// named in the record — InvalidateVideoMem flushes the pending plan per
					// spilling transfer, so consecutive merges are in different plans by
					// construction.
					//
					// PRE-REGISTERED DEVICE TARGET, so that a later reading cannot be
					// fitted to whatever came back. Spider-Man 3 on the SD865:
					// gpu_ms_p50 50.603 -> ~40, frame_ms_p50 50.412 -> ~40 (the arm is
					// GPU-bound, so the two move together). ⚠️ THAT TARGET IS NOT THIS
					// KEY'S. It was registered against the study's stage 1 before the
					// corpus said the groups are one page; this key changes one render
					// pass on one dump and cannot reach it. The key that can is
					// TileGpuNarrowSeedPassArea below, and the device A/B must bisect
					// the two rather than reading them together. Device numbers PENDING.
					//
					// Default TRUE. OFF is the one-pass-per-page road, byte for byte.
					TileGpuMergeSeedBatch : 1,
					// A seed's render pass covers the PAGES IT SEEDS, not the whole
					// attachment.
					//
					// This is the pass-area clamp the DRAW passes have shipped since the
					// pass-structure census (gsTileGpuPassArea), applied at the one pass
					// that never got it. The seed is a full-target triangle whose fragments
					// discard outside the op's page set, and the executor already computes
					// the rectangle those pages occupy and sets it as the SCISSOR — but the
					// render AREA stayed the whole attachment. On an immediate-mode GPU that
					// costs nothing. On a TILER it is the whole cost: a render pass loads
					// every tile its area covers and stores every one back, drawn or not, so
					// a seed repairing ONE 64x32 guest page paid a load and a store of the
					// entire target.
					//
					// ⚠️ MEASURED ON THE CORPUS, and this is the number the seed diet was
					// actually looking for. Render-pass area per frame, seed passes against
					// the scissors those same passes set: Spider-Man 3 39.89 Mpx of
					// attachment for 3.63 Mpx of scissor, against 70.09 Mpx for the WHOLE
					// frame -- so 57% of everything the frame loads and stores is seed
					// passes, and 91% of that is area the scissor throws away. Dirge of
					// Cerberus 3.62 -> 0.94 of 143.79, GT4 1.61 -> 0.64 of 35.82, SotC 0.56
					// -> 0.10 of 9.71. Narrowing takes Spider-Man 3's frame from 70.09 to
					// 33.83 Mpx, a 52% cut, and it is 173 of its 331 seed passes a frame
					// that the upload merge emits one of per merged page.
					//
					// PIXEL-INERT, and by the draw passes' own argument rather than a new
					// one: the scissor already confined every write to this rectangle, so the
					// area is shrinking to the only region that was ever written; outside it
					// a LOAD/STORE attachment is neither loaded nor stored and keeps its
					// bytes; and the round-out margin is loaded and stored back unchanged
					// because no fragment reaches there. A mid-pass command-buffer flush
					// restarts the pass on the area it saved, which is this one.
					//
					// ⚠️ ONLY WHERE THE ATTACHMENT LOADS, which is gsTileGpuMayClampPassArea's
					// rule and is why that function is shared rather than restated. A CLEAR
					// must cover the whole attachment or the pages outside stay uninitialized
					// for a later pass that loads them — the reason the full area was here at
					// all — and a clamped DONT_CARE leaves that region holding whatever the
					// full-area form left undefined. Both keep the whole attachment, which is
					// what they already do, so neither changes.
					//
					// PRE-REGISTERED DEVICE TARGET, inherited from the seed diet this
					// key turned out to be: Spider-Man 3 on the SD865, gpu_ms_p50 50.603
					// -> ~40 and frame_ms_p50 50.412 -> ~40. The mechanism it rests on is
					// that the SD865 bills ~62.86 us of GPU per merged page and a merged
					// page is one full-attachment seed pass; this key leaves the pass and
					// removes 91% of its area. The M2 cannot test it -- Honeykrisp bills
					// this work to record time, which is a function of the pass COUNT, and
					// the count does not move (Spider-Man 3: 9026 render passes on both
					// arms, to the unit). ⚠️ RUN THE DEVICE HASH GRID BEFORE THE TIMING
					// A/B: partial render areas are content-preserving by spec and
					// byte-identical on 21 dumps here, but that is one driver, and
					// Turnip's render-area granularity also decides how much of the
					// -11.9% survives (Honeykrisp reports 1x1, so the round-out is free
					// on this box and will not be on Adreno). Device numbers PENDING.
					//
					// Default TRUE. OFF is the whole-attachment road, byte for byte.
					TileGpuNarrowSeedPassArea : 1,
					// The fast profile: shed an exactness class for its GPU-native
					// realization, gated per title by the perceptual comparator (as
					// good or better than Classic against the SW goldens). Umbrella
					// key; a TileExact* pin below holds one class exact underneath
					// it. Depth sits OUTSIDE the umbrella — its contract is
					// plane-exact integer and its keys pick the realization
					// explicitly. All default off: the exact profile is today's
					// behavior, byte-identical. Never user settings. A class wires
					// in when its fast leg lands, so every declared key has a
					// consumer: today the umbrella demotes the colour/fog walk to
					// the interpolator's varyings, kills the texture walk's two
					// unbounded per-fragment loops (the perspective float walk and
					// the scaled-sprite row replay), and admits perspective
					// triangles onto the sampler leg.
					TileFastShading : 1,
					// Pin the exact colour/fog walk under the umbrella.
					TileExactColour : 1,
					// Pin the exact texture-coordinate walks under the umbrella
					// (perspective draws floor again, sprite rows replay again).
					TileExactTexCoord : 1,
					// Pin the in-shader filter weights under the umbrella: the
					// sampler leg's linear draws go back to the 4-tap float-weight
					// shader filter instead of one hardware tap at the snapped
					// coordinate.
					TileExactTexFilter : 1,
					// Pin the exact alpha-test order under the umbrella: a dynamic
					// test whose fail mask differs on order-dependent depth floors
					// again instead of taking the two-pass split (the reorder
					// Classic ships unconditionally).
					TileExactAlphaTest : 1,
					// Pin the exact feedback floor under the umbrella: a textured
					// draw whose sampled CORE (vertex UV bbox shrunk one texel)
					// is page-disjoint from its own write footprint floors again
					// instead of rendering natively with the edge taps reading
					// pre-draw bytes (SotC's bloom downsample chain).
					TileExactFeedback : 1,
					// Pin the exact DATE floor under the umbrella: destination-
					// alpha-test draws keep the SW floor instead of the device's
					// stencil / one-barrier realization (which tests the target
					// as of draw start, Classic's own default-accuracy trade).
					TileExactDate : 1,
					// Depth from the closed plane form instead of the soft-float64
					// walk: the scanline's own gradients (double-formed), integer
					// wraparound plus an f32 fraction carry per fragment, within
					// one unit of the walk at comparator ties — the depth
					// contract's shipping realization (~15 ALU vs ~1,500×4).
					TileFastDepthPlane : 1,
					// Depth through the GPU interpolator and the zfloor/zclamp
					// recipe — Classic's realization. Attribution control ONLY:
					// measured mis-sorting surfaces by up to 162k of 2^24 on
					// OutRun's laddered strips, the class the contract forbids.
					TileFastDepthClassic : 1,
					UseBlitSwapChain : 1,
					DisableShaderCache : 1,
					DisableFramebufferFetch : 1,
					EnableAdrenoFramebufferFetch : 1,
					ForceMaliFramebufferFetch : 1,
					DisablePS2DepthQuantization : 1,
					DisableVertexShaderExpand : 1,
					SkipDuplicateFrames : 1,
					OsdShowSpeed : 1,
					OsdShowFPS : 1,
					OsdShowVPS : 1,
					OsdShowResolution : 1,
					OsdShowGSStats : 1,
					OsdShowCPU : 1,
					OsdShowGPU : 1,
					OsdShowGPUDebug : 1,
					OsdShowGPUStats : 1,
					OsdShowIndicators : 1,
					OsdShowFrameTimes : 1,
					OsdShowHardwareInfo : 1,
					OsdShowVersion : 1,
					OsdShowSettings : 1,
					OsdshowPatches : 1,
					OsdShowInputs : 1,
					OsdShowVideoCapture : 1,
					OsdShowInputRec : 1,
					OsdShowTextureReplacements : 1,
					OsdBoldText : 1,
					HWSpinGPUForReadbacks : 1,
					HWSpinCPUForReadbacks : 1,
					GPUPaletteConversion : 1,
					AutoFlushSW : 1,
					PreloadFrameWithGSData : 1,
					Mipmap : 1,
					HWMipmap : 1,
					HWAccurateAlphaTest : 1,
					HWAA1 : 1,
					HWROV : 1,
					HWROVLogging : 1,
					HWROVBarriersVK : 1,
					// Hold hardware draws back so consecutive draws to the same target
					// share one render pass (GSPassScheduler). Aimed at tiling GPUs,
					// where every pass boundary is a full tile load and store. Hot-
					// appliable: turning it off just stops deferring.
					CoalesceRenderPasses : 1,
					ManualUserHacks : 1,
					UserHacks_AlignSpriteX : 1,
					UserHacks_CPUFBConversion : 1,
					UserHacks_ReadTCOnClose : 1,
					UserHacks_DisableDepthSupport : 1,
					UserHacks_DisablePartialInvalidation : 1,
					UserHacks_DisableSafeFeatures : 1,
					UserHacks_DisableRenderFixes : 1,
					UserHacks_MergePPSprite : 1,
					UserHacks_ForceEvenSpritePosition : 1,
					UserHacks_NativePaletteDraw : 1,
					UserHacks_EstimateTextureRegion : 1,
					UserHacks_DrawBuffering : 1,
					FXAA : 1,
					ShadeBoost : 1,
					DumpGSData : 1,
					SaveRT : 1,
					SaveFrame : 1,
					SaveTexture : 1,
					SaveDepth : 1,
					SaveAlpha : 1,
					SaveInfo : 1,
					SaveTransferImages : 1,
					SaveDrawStats : 1,
					SaveFrameStats : 1,
					SaveHWConfig : 1,
					DumpReplaceableTextures : 1,
					DumpReplaceableMipmaps : 1,
					DumpTexturesWithFMVActive : 1,
					DumpDirectTextures : 1,
					DumpPaletteTextures : 1,
					LoadTextureReplacements : 1,
					LoadTextureReplacementsAsync : 1,
					PrecacheTextureReplacements : 1,
					EnableVideoCapture : 1,
					EnableVideoCaptureParameters : 1,
					VideoCaptureAutoResolution : 1,
					EnableAudioCapture : 1,
					EnableAudioCaptureParameters : 1,
					OrganizeSnapshotsByGame : 1,
					OrganizeVideoCaptureByGame : 1;
			};
		};

		int VsyncQueueSize = 2;

		float FramerateNTSC = DEFAULT_FRAME_RATE_NTSC;
		float FrameratePAL = DEFAULT_FRAME_RATE_PAL;

		AspectRatioType AspectRatio = DEFAULT_ASPECT_RATIO;
		FMVAspectRatioSwitchType FMVAspectRatioSwitch = DEFAULT_FMV_ASPECT_RATIO;
		DisplayRotation Rotation = DisplayRotation::Rot0;
		GSInterlaceMode InterlaceMode = DEFAULT_INTERLACE_MODE;
		GSPostBilinearMode LinearPresent = DEFAULT_BILINEAR_FILTERING_MODE;

		float StretchY = 100.0f;
		/// Width/height for AspectRatioType::Custom. Stored as a ratio, not W and H separately, so
		/// anything can be expressed (2.1666 for 19.5:9, 1.85 for a film ratio) without a second key.
		float CustomAspectRatio = 16.0f / 9.0f;
		int Crop[4] = {};

		float OsdScale = DEFAULT_OSD_SCALE;
		/// OSD text colour as 0xRRGGBB. 0 keeps the classic white, so existing installs and
		/// every non-Android frontend are untouched unless the user picks a colour.
		u32 OsdColor = 0;
		float OsdMargin = DEFAULT_OSD_MARGIN;
		std::string OsdFontPath;
		OsdOverlayPos OsdMessagesPos = DEFAULT_OSD_MESSAGE_POS;
		OsdOverlayPos OsdPerformancePos = DEFAULT_OSD_PERFORMANCE_POS;

		GSRendererType Renderer = DEFAULT_HW_RENDERER;
		GSHWRendererVariant HWRendererVariant = GSHWRendererVariant::Auto;
		float UpscaleMultiplier = DEFAULT_UPSCALE_MULTIPLIER;

		AccBlendLevel AccurateBlendingUnit = DEFAULT_BLENDING_ACCURACY;
		BiFiltering TextureFiltering = DEFAULT_TEXTURE_FILTERING_MODE;
		TexturePreloadingLevel TexturePreloading = TexturePreloadingLevel::Full;
		GSDumpCompressionMethod GSDumpCompression = GSDumpCompressionMethod::Zstandard;
		GSHardwareDownloadMode HWDownloadMode = GSHardwareDownloadMode::Enabled;
		GSCASMode CASMode = DEFAULT_CAS_MODE;
		GSUpscaler Upscaler = DEFAULT_UPSCALER;
		u8 Dithering = 2;
		u8 MaxAnisotropy = 0;
		u8 TVShader = 0;
		s16 GetSkipCountFunctionId = -1;
		s16 BeforeDrawFunctionId = -1;
		s16 MoveHandlerFunctionId = -1;
		int SkipDrawStart = 0;
		int SkipDrawEnd = 0;

		GSHWAutoFlushLevel UserHacks_AutoFlush = GSHWAutoFlushLevel::Disabled;
		GSHalfPixelOffset UserHacks_HalfPixelOffset = GSHalfPixelOffset::Off;
		s8 UserHacks_RoundSprite = 0;
		GSNativeScaling UserHacks_NativeScaling = GSNativeScaling::Off;
		s32 UserHacks_TCOffsetX = 0;
		s32 UserHacks_TCOffsetY = 0;
		u8 UserHacks_CPUSpriteRenderBW = 0;
		u8 UserHacks_CPUSpriteRenderLevel = 0;
		u8 UserHacks_CPUCLUTRender = 0;
		GSGPUTargetCLUTMode UserHacks_GPUTargetCLUTMode = GSGPUTargetCLUTMode::Disabled;
		GSTextureInRtMode UserHacks_TextureInsideRt = GSTextureInRtMode::Disabled;
		GSLimit24BitDepth UserHacks_Limit24BitDepth = GSLimit24BitDepth::Disabled;
		GSBilinearDirtyMode UserHacks_BilinearHack = GSBilinearDirtyMode::Automatic;
		TriFiltering TriFilter = DEFAULT_TRILINEAR_FILTERING_MODE;
		s8 OverrideTextureBarriers = -1;
		GSDepthFeedbackMode DepthFeedbackMode = GSDepthFeedbackMode::Auto;
		GSBackThreadMode BackThreadMode = GSBackThreadMode::Off;

		// RetroArch (.slangp) shader chain, applied at present after ShadeBoost/FXAA via
		// librashader. Disabled or an empty preset skips the chain entirely (zero cost),
		// and it's a no-op on renderers/builds without a librashader backend.
		bool ShaderChainEnabled = false;
		std::string ShaderChainPreset;

		// LSFG — Lossless Scaling frame generation, inserted into the Vulkan present path.
		// Off unless the user both enables it AND supplies their own Lossless.dll: the
		// interpolation shaders are read out of that file at runtime and nothing about them
		// ships with ARMSX2. Vulkan + Adreno 7xx and newer only, and compiled out entirely
		// in the play flavour, so every one of these is inert in a build without it.
		bool LsfgEnabled = false;
		u8 LsfgMultiplier = 2; // frames displayed per rendered frame: 2 = one interpolated
		std::string LsfgDllPath;
		// LSFG 3.1p, a lighter shader family than 3.1. Default on: this runs on a phone GPU
		// that is already presenting the game, and the cheaper pipeline is what makes the
		// feature pay for itself there. Falls back to 3.1 when the user's DLL predates 3.1p.
		bool LsfgPerformance = true;
		// Optical-flow resolution, as a percentage of the presented image (25..100). Lower is
		// cheaper and blurrier. Handed to the library as a DIVISOR — see GSLsfg.cpp.
		u8 LsfgFlowScale = 100;
		// Target OUTPUT rate in Hz for the adaptive pacer; 0 holds LsfgMultiplier fixed.
		//
		// A fixed multiplier is the wrong shape for a game that oscillates between 60 and 30fps
		// on a 60Hz panel: at x2 it presents 120 then 60, and every transition is visible as
		// judder. Given a target, the pacer varies the generation count instead — two
		// interpolated frames while the game runs at 30, one while it runs at 60 — so the
		// presented rate stays put while the rendered rate moves underneath it.
		//
		// u16 because 0..1000 covers every panel; 0 is the default so behaviour is unchanged
		// until the user opts in.
		u16 LsfgTargetRate = 0;

		u8 CAS_Sharpness = 50;
		// 0..100, shared by the upscalers. FSR1 maps it to AMD's "stops" scale and SGSR to its
		// own 0..2 edge sharpness, two percent per percent, so both reach their full range off
		// one control. FSR1's RCAS pass, 0..100. Mapped to AMD's "stops" scale in GSDevice::FSR1Upscale,
		// where 0 stops is maximum sharpening - it is not the same curve as CAS_Sharpness.
		u8 FSR_Sharpness = 50;

		// SGSR's own, deliberately NOT shared with FSR_Sharpness above.
		//
		// Qualcomm's edge sharpness runs 0..2 and FSR1's slider is natively 0..100, so the two
		// want different ranges. Reusing one field would mean an existing FSR configuration
		// silently means something else the moment SGSR is picked, and widening that field to
		// 200 would change what every FSR value already stored out there means. Neither is worth
		// saving one setting. 100 here is Qualcomm's default of 1.0.
		u8 SGSR_Sharpness = 100;
		u8 ShadeBoost_Brightness = DEFAULT_SHADEBOOST_BRIGHTNESS;
		u8 ShadeBoost_Contrast = DEFAULT_SHADEBOOST_CONTRAST;
		u8 ShadeBoost_Saturation = DEFAULT_SHADEBOOST_SATURATION;
		u8 ShadeBoost_Gamma = DEFAULT_SHADEBOOST_GAMMA;
		u8 PNGCompressionLevel = 1;

		u16 SWExtraThreads = 2;
		u16 SWExtraThreadsHeight = 4;

		int SaveDrawStart = 0;
		int SaveDrawCount = 5000;
		int SaveDrawBy = 1;
		int SaveFrameStart = 0;
		int SaveFrameCount = -1;
		int SaveFrameBy = 1;

		// Tile renderer bisect lever: after this many native draws in the session, every
		// draw floors (0 = no limit). A frame that changes when the Nth native draw is
		// allowed names the interaction the per-draw oracle cannot see -- the oracle
		// syncs every draw's inputs, so a stale-input or handoff defect is invisible to
		// it and only shows in a plain run. Never a user setting.
		int TileNativeDrawLimit = 0;

		// Override the device's answer to "how many draws may one TileGpu render pass
		// hold?" (GSDevice::TileGpuMaxPassDraws). Zero -- the default -- asks the
		// device, which is what ships: Adreno answers 64, every other vendor 0. A
		// NEGATIVE value forces no cap whatever the device says; a positive one pins
		// that cap on any device.
		//
		// A cap is a pure split: the pass closes and another with the same key opens,
		// same draws, same order, same attachments. It moves no pixel and it moves
		// plenty of frame time, which is the same hazard class as the two depth-policy
		// force keys above -- a run whose cap you cannot read off its log is a run whose
		// number means nothing. So the renderer names the effective cap and its source
		// in the emulog, and reads it ONCE at construction like every other pass-boundary
		// policy. Dev only; a pass-boundary policy is not a user setting.
		int TileGpuMaxPassDraws = 0;

		// How many descriptor sets the TileGpu source ring holds (the sets rule 3's
		// materialised textures are bound through, one written per plan). Zero -- the
		// default -- takes the built-in depth; a positive value forces that depth,
		// clamped to what the ring can be. Negative means nothing here and is treated
		// as zero: unlike a pass cap there is no "off" position, because a ring of no
		// sets is not a configuration.
		//
		// It is a pure resource count. The sets carry identical descriptors whatever
		// the depth is, so the frame cannot change -- what changes is how often the
		// ring comes round to a set an in-flight submission still reads, which stalls
		// the GS thread mid-plan (GSDeviceVK::GpuWaitCause::SourceSet). So this is a
		// wait-count lever, not a pixel one, and it exists as the A/B road back from
		// the built-in depth. Dev only.
		int TileGpuSourceSetRingDepth = 0;

		// Override the device's answer to "how many extra pipeline binds may one plan pay
		// for fragment SPECIALIZATION?" (GSDevice::TileGpuMaxSpecializationBinds). Zero --
		// the default -- asks the device, which is what ships: Adreno answers 300, every
		// other vendor 0 = no guard. A NEGATIVE value forces the guard off whatever the
		// device says; a positive one pins that budget on any device.
		//
		// Freezing a draw's GS state into its fragment program makes the program smaller
		// (247 of 251 corpus variants run wave128 against 2 of 17 before) and makes the
		// variant part of the indirect-run key, so a state change now costs a pipeline
		// bind where it used to cost nothing. On an SD865 that trade wins big on Shadow of
		// the Colossus (-20.5%) and Gran Turismo 4 (-7.6%) and LOSES on both Ratchet &
		// Clank scenes (+9.2%, +8.4%), with their GPU time rising -- and the separator is
		// how many binds the freezing ADDS: the losers pay 525 and 859 a frame, the winners
		// 141 and 150. Over the budget, the planner withholds the frozen state from the
		// passes that add the binds, which puts them back on exactly the program the
		// unspecialized arm compiles.
		//
		// Same hazard class as the pass cap above -- it moves no pixel and it moves frame
		// time -- so the renderer names the effective budget and its source in the emulog
		// and reads it ONCE at construction. Dev only.
		int TileGpuMaxSpecializationBinds = 0;

		// Which road TileGpu's As blend factor takes. Zero -- the default -- asks the device: a
		// second fragment output at index 1 where it offers dualSrcBlend, the alpha carrier where
		// it does not. A POSITIVE value forces the carrier roads anywhere; a negative one asks for
		// the second output, which a device without the feature still cannot give.
		//
		// The carrier roads are exact, so this is a byte-identity A/B, and the force value exists
		// to run it on a device that would otherwise never take them. The factor lives in
		// o_color.a, and the alpha byte it displaces is masked off, given back by the alpha blend
		// equation, or written by a companion draw over the same geometry (the fold-into-the-colour
		// road is deliberately gone -- gsTileGpuDualSrcRoad says why). Only the companion adds
		// anything to the frame, and over the corpus it is zero draws on five dumps and 354 on the
		// worst. Dev only.
		int TileGpuDualSrcRoad = 0;

		// How hard the TileGpu plan executor forces GPU-side ordering between the ops it records.
		// Zero -- the default, and the only thing that ships -- records nothing extra and is
		// byte-for-byte the executor as written. 1, 2 and 3 are a diagnostic ladder, each grade
		// containing the one below:
		//
		//   1  a full-scope pipeline barrier (ALL_COMMANDS -> ALL_COMMANDS, MEMORY_READ|WRITE both
		//      ways) after every op recorded outside a render pass, and after every pass.
		//   2  ...and the command buffer ends and submits there, continuing on a fresh one.
		//   3  ...and the submission's fence is waited on, so every op fully retires before the
		//      next records.
		//
		// It exists for ONE question, on r44p1 Mali only: TileGpu is run-to-run nondeterministic
		// there and byte-identical everywhere else, with sync validation clean, so the standing
		// theory is that the blob drops or weakens ordering edges inside a producer->consumer graph
		// this deep in one command buffer. Garbage that vanishes as the grade rises proves the
		// theory on silicon and gives the minimal-sync search a ceiling; garbage that survives
		// grade 3 kills it. Grade 3 is unusably slow by construction -- that is the point of it.
		//
		// ⚠️ Only grades 0 and 1 are PIXEL-INERT. 0 is inert by construction and 1 measured
		// byte-identical to it on MGS3, SotC and FlatOut 2 (M2 / Honeykrisp). Grades 2 and 3 move
		// MGS3 by at most 1 per channel on 6% of its pixels, always in the same direction, and
		// leave SotC and FlatOut 2 untouched. It is the SUBMIT that does it, not the state reset a
		// cut performs (measured separately), and grades 2 and 3 produce the identical image -- so
		// it is submission granularity and not a race our recording is losing. A delta that small
		// cannot manufacture or erase the block-sized garbage the lever is hunting, but a device
		// result at grade 2 or 3 has to be read knowing the arm is not byte-inert. Dev only.
		int TileGpuSerializeOps = 0;

		// WHICH of the executor's op boundaries gets the grade-1 barrier, as a bitmask over
		// GSTileGpuSerializeSite (GSDevice.h, which owns the numbering and is append-only so a mask
		// quoted in a device record keeps meaning what it meant). Zero -- the default -- records
		// nothing, exactly as grade 0 does; 0x7F is every site and is the same command stream as
		// grade 1.
		//
		// It exists because the blanket grade cannot name an edge. Grade 1 collapsing FlatOut 2 from
		// 79.7% run-to-run pixel diff to byte-identical says the hazard closes inside one command
		// buffer under barriers alone; it does not say at which of seven boundaries, and finding out
		// by re-running blankets is seven device rounds. A mask bisects it in three.
		//
		// Pixel-inert at every value, on the same evidence grade 1 is: the barrier orders work that
		// is already in dependency order, so a device where it changes an image is a device where the
		// dependency was not being honoured -- which is the finding, not a side effect. The BLANKET
		// wins a contradiction: with TileGpuSerializeOps non-zero this key is refused and says so,
		// rather than being merged into a grade, because an arm whose engagement you have to derive
		// from a precedence rule is an arm nobody will believe. Dev only.
		int TileGpuSerializeMask = 0;

		// The largest page footprint a container may reach to admit a contained view, in 8 KB guest
		// pages. Zero -- the default -- is the whole of GS memory, 512 pages, which is the only cap
		// the geometry itself imposes.
		//
		// A cap is needed because the container grows to the union of its views and the space
		// between them is a hole nothing ever drew: folding GT4's palette buffer 35 page rows past
		// the frame buffer turns a 640x448 image into a 640x1632 one, 4.2 MB of which most is
		// never written. Pages, not rows and not bytes, because the wrap clause the predicate
		// already enforces is in pages and a second unit would let the two disagree.
		//
		// Read only by gsTileContainView, so it decides nothing while TileGpuContainSurfaces is
		// off -- nor while the shipped rule refuses every displaced placement, since a view that
		// lands at (0, 0) is inside its container's own pages and grows it by nothing. It is the
		// displaced road's cap, waiting for that road.
		int TileGpuContainPageBudget = 0;

		// Draw REORDERING in the TileGpu pass planner: how many per-(colour, depth) runs of draws the
		// planner may hold open at once, emitting each contiguously instead of in guest order.
		//
		// Three states, the same shape as TileGpuMaxPassDraws and for the same reason -- "count what
		// it would do" and "do it" are different questions, and a bool cannot ask the second one at a
		// chosen width:
		//
		//   0  -- OFF, and what ships. No census, no reorder, no cost.
		//  <0  -- CENSUS ONLY. Run the admission model over |N| runs and report at teardown what it
		//         would have moved. Nothing moves; the plan is the plan.
		//  >0  -- REORDER, holding at most N runs open.
		//
		// What it is for: a pass ends whenever its attachments change, so a game that alternates
		// between a framebuffer and a scratch buffer ends one on every switch -- Dirge of Cerberus
		// plans about a thousand passes a frame that way, against sixty for the same draws. The draws
		// into the two buffers touch no GS page in common, so they can be grouped instead of
		// interleaved, and the passes collapse.
		//
		// ⚠️ Reordering is PIXEL-INERT by construction and a difference between the on and off arms is
		// a DEFECT, not a trade: a draw is moved only where the page model proves it shares no GS page
		// with anything it passes, and every decision about the draw -- its surfaces, its texel road,
		// its seeds, its palette -- was taken in guest order before the move.
		//
		// It is also a one-title lever on today's corpus. Every dump but Dirge measures between 0%
		// and -3% passes, because their alternations are data dependencies (a palette buffer rendered
		// and then sampled) that no reordering can remove.
		int TileGpuReorderRuns = 0;

		// How many video frames a CPU READ keeps a page off the TileGpu upload merge, so an upload
		// that spills into it takes the blocking road instead.
		//
		//    0   -- the clause never refuses. Delete-equivalent, and the control arm.
		//   1..N -- a page read within the last N frames is refused; older is merged.
		//  >=65535 -- the mark never expires: session-monotone, exactly the road that shipped before
		//            this key existed. The OFF arm of the A/B.
		//  negative -- meaningless here, treated as 0.
		//
		// ⚠️ This is a WAIT-COUNT HEURISTIC, not an invariant. Nothing about the merge needs it: it is
		// only ever asked about pages that are GPU truth and NOT synced, and serving one leaves them
		// that way -- pinned by AWriteServedOnTheGpuMovesNoTruth in
		// tests/ctest/core/gs/gs_tilegpu_upload_merge_tests.cpp, which asserts the page is still
		// unsynced and still returned by ReadbackNeeded after the merge takes it. So a later CPU read
		// still pulls, whatever this is set to, and the two spill roads are byte-identical by the
		// merge's own standing claim (see TileGpuUploadSpillReadback). The key trades one wait now
		// against several later, and nothing else.
		//
		// It exists because DELETING the clause is a measured regression on Dirge of Cerberus, which
		// reads its palettes out of the tail blocks of a framebuffer page every frame: upload stalls
		// 4.62 -> 0.88 a frame, CLUT stalls 2.00 -> 10.25, a net LOSS of 4.5 waits. So the answer is a
		// window, not a deletion -- keep the protection for pages the CPU is still reading, collect
		// the ones it read once and left.
		//
		// ⚠️ THE DEFAULT IS 65535 -- the mark never expires -- because EVERY FINITE WINDOW MEASURED
		// WORSE THAN THE ROAD IT REPLACES, on every title that reaches the clause, monotonically in
		// the knob. The key exists so the arms are one INI line apart, not because a window won.
		//
		// The un-poison and the CLUT pull road are fighting over THE SAME PAGES. Spider-Man 3's
		// spill pages are its palette pages: 50 CLUT loads a frame cannot be gathered (the owner
		// census's "multi/partial" bucket), and today they are free because the upload spill's drain
		// hands the page's truth back on the side. Take the drain away and each of them pays its own
		// round trip -- and the count then grows past 50, because leaving truth on the GPU splits
		// more page-owner sets (multi/partial 50.00 -> 200.25) and shatters the batching (mid-frame
		// flushes 109 -> 321). M2, Spider-Man 3, per drawn frame:
		//
		//   window   upload stalls   CLUT stalls   blocking waits   blocking ms   readbacks   p50 ms
		//    65535           33.75         50.00            84.25         28.82         673    77.49
		//        4           28.25        100.00           128.75         43.06        1029    99.01
		//        2           17.25        200.25           218.00         68.59        1743   142.73
		//        1            0.00        352.25           352.75        103.24        2821   171.95
		//        0            0.00        352.25           352.75        105.78        2821   167.86
		//
		// Dirge of Cerberus and yugioh, the other two dumps that reach the clause, move the same way
		// and smaller: total waits a frame 7.75 -> 9.62 -> 11.62 and 3.25 -> 4.00 -> 4.00 over the
		// same windows. Window 0 reproduces the header's pre-bitmap dirge numbers to the digit
		// (upload stalls 4.25 -> 0.88), which is the check that this arm really is the deletion.
		//
		// So the clause is not a poison to be lifted; it is economics that hold. THE ORDER IS
		// INVERTED: the multi/partial CLUT population has to be served on the GPU FIRST, and only
		// then is there anything to win here. Until then a window just moves the wait onto a road
		// that charges more for it.
		//
		// What the census does say, and it stands: 100% of the pages a lifted clause frees are
		// mergeable -- over the refused population the clause below this one would refuse 0.00 on
		// Spider-Man 3 and yugioh and 0.12 on dirge. The merge is not what stops them. Refusals by
		// the age of the mark that refused them, per drawn frame, at the monotone default:
		//
		//   refusals by mark age    this frame    1    2-3    4-15
		//     spiderman3   33.75          0.00  5.50  11.00  17.25
		//     dirge         4.25          1.12  0.50   1.00   1.62
		//     yugioh        0.38          0.00  0.00   0.12   0.25
		//
		// ⚠️ The age census is bounded by the corpus: a dump is 8 model frames, so no mark can be
		// older than 7 and the "older than 15" bucket cannot fire.
		//
		// ⚠️ A FINITE WINDOW ALSO MOVES SPIDER-MAN 3's PIXELS, and that is a filed defect in the GPU
		// merge road rather than a trade -- the two spill roads claim to be byte-identical (see
		// TileGpuUploadSpillReadback). 37 to 788 pixels of 307,200 a frame, max channel delta 14,
		// alpha never; the same pixels at windows 0, 1 and 2 despite serving 173 pages against 105,
		// so the population is bounded. At 65535 all 21 corpus dumps are byte-identical to the road
		// before this key existed.
		//
		// ⚠️ Device A/B pending at the time of writing. The numbers above are M2/lavapipe, but the
		// stall COUNTS are model-level and deterministic, so they transfer as counts whatever a pull
		// costs. The device record this came from is Spider-Man 3 on the SD865, where 42 of 52
		// out-of-band pulls a frame are upload spills this clause refused.
		int TileGpuMergeCpuReadWindow = 65535;

		s8 ExclusiveFullscreenControl = -1;
		GSScreenshotSize ScreenshotSize = GSScreenshotSize::WindowResolution;
		GSScreenshotFormat ScreenshotFormat = GSScreenshotFormat::PNG;
		int ScreenshotQuality = 90;

		std::string CaptureContainer = DEFAULT_CAPTURE_CONTAINER;
		std::string VideoCaptureCodec;
		std::string VideoCaptureFormat;
		std::string VideoCaptureParameters;
		std::string AudioCaptureCodec;
		std::string AudioCaptureParameters;
		int VideoCaptureBitrate = DEFAULT_VIDEO_CAPTURE_BITRATE;
		int VideoCaptureWidth = DEFAULT_VIDEO_CAPTURE_WIDTH;
		int VideoCaptureHeight = DEFAULT_VIDEO_CAPTURE_HEIGHT;
		int AudioCaptureBitrate = DEFAULT_AUDIO_CAPTURE_BITRATE;

		std::string Adapter;
		std::string AndroidGpuProfileOverride = "auto";
		std::string HWDumpDirectory;
		std::string SWDumpDirectory;

		/// Hacks the player set deliberately, one bit per GSUserHackOverride. Kept out of
		/// the bitfield union above on purpose: that packing is what OptionsAreEqual
		/// compares wholesale, and this is not a hack value, it is who owns one.
		u32 UserHackOverrides = 0;

		GSOptions();

		void LoadSave(SettingsWrapper& wrap);

		bool IsUserHackPinned(GSUserHackOverride hack) const
		{
			return (UserHackOverrides & (1u << static_cast<u32>(hack))) != 0;
		}

		void SetUserHackPinned(GSUserHackOverride hack, bool pinned)
		{
			const u32 bit = 1u << static_cast<u32>(hack);
			UserHackOverrides = pinned ? (UserHackOverrides | bit) : (UserHackOverrides & ~bit);
		}

		/// Sets user hack values to defaults when user hacks are not enabled. Hacks the
		/// player claimed survive, unless the caller is stripping for safety rather than
		/// preference, in which case pass false.
		void MaskUserHacks(bool respect_claims = true);

		/// Sets user hack values to defaults when upscaling is not enabled.
		void MaskUpscalingHacks();

		/// Returns true if any of the hardware renderers are selected.
		bool UseHardwareRenderer() const;

		/// Returns false if the compared to the old settings, we need to reopen GS.
		/// (i.e. renderer change, swap chain mode change, etc.)
		bool RestartOptionsAreEqual(const GSOptions& right) const;

		/// Whether changing this INI key forces a GS device teardown, i.e. whether it is
		/// one of the fields RestartOptionsAreEqual compares. Lets a caller mutating a
		/// setting by name report the cost without diffing two whole configs.
		static bool IsRestartOption(const char* ini_key);

		/// Returns false if any options need to be applied to the MTGS.
		bool OptionsAreEqual(const GSOptions& right) const;

		bool operator==(const GSOptions& right) const;
		bool operator!=(const GSOptions& right) const;

		// Should we dump this draw/frame?
		bool ShouldDump(u64 draw, int frame) const;
	};

	struct SPU2Options
	{
		enum class SPU2SyncMode : u8
		{
			Disabled,
			TimeStretch,
			Count
		};

		static constexpr s32 MAX_VOLUME = 200;
#ifdef __ANDROID__
		static constexpr AudioBackend DEFAULT_BACKEND = AudioBackend::Oboe;
#else
		static constexpr AudioBackend DEFAULT_BACKEND = AudioBackend::Cubeb;
#endif
		static constexpr SPU2SyncMode DEFAULT_SYNC_MODE = SPU2SyncMode::TimeStretch;

		static std::optional<SPU2SyncMode> ParseSyncMode(const char* str);
		static const char* GetSyncModeName(SPU2SyncMode backend);
		static const char* GetSyncModeDisplayName(SPU2SyncMode backend);

		BITFIELD32()
		bool
			DebugEnabled : 1,
			MsgToConsole : 1,
			MsgKeyOnOff : 1,
			MsgVoiceOff : 1,
			MsgDMA : 1,
			MsgAutoDMA : 1,
			MsgCache : 1,
			AccessLog : 1,
			DMALog : 1,
			WaveLog : 1,
			CoresDump : 1,
			MemDump : 1,
			RegDump : 1,
			VisualDebugEnabled : 1;
		BITFIELD_END

		u32 StandardVolume = 100;
		u32 FastForwardVolume = 100;
		bool OutputMuted = false;
		// Low-end Android lever: skip the SPU2 reverb pipeline in MixCore. Off by default.
		bool LightweightMode = false;

		AudioBackend Backend = DEFAULT_BACKEND;
		SPU2SyncMode SyncMode = DEFAULT_SYNC_MODE;
		AudioStreamParameters StreamParameters;

		std::string DriverName;
		std::string DeviceName;

		SPU2Options();

		void LoadSave(SettingsWrapper& wrap);

		bool IsTimeStretchEnabled() const { return (SyncMode == SPU2SyncMode::TimeStretch); }

		bool operator==(const SPU2Options& right) const;
		bool operator!=(const SPU2Options& right) const;
	};

	struct DEV9Options
	{
		enum struct NetApi : int
		{
			Unset = 0,
			PCAP_Bridged = 1,
			PCAP_Switched = 2,
			TAP = 3,
			Sockets = 4,
			LocalLink = 5,
		};
		static const char* NetApiNames[];

		enum struct DnsMode : int
		{
			Manual = 0,
			Auto = 1,
			Internal = 2,
		};
		static const char* DnsModeNames[];

		struct HostEntry
		{
			std::string Url;
			std::string Desc;
			u8 Address[4]{};
			bool Enabled;

			bool operator==(const HostEntry& right) const;
			bool operator!=(const HostEntry& right) const;
		};

		bool EthEnable{false};
		NetApi EthApi{NetApi::Unset};
		std::string EthDevice;
		bool EthLogDHCP{false};
		bool EthLogDNS{false};
		bool LocalLinkHost{false};
		std::string LocalLinkAddress;
		u32 LocalLinkPort{19072};
		u32 LocalLinkPeerId{1};
		std::string LocalLinkRoomCode;

		bool InterceptDHCP{false};
		u8 PS2IP[4]{};
		u8 Mask[4]{};
		u8 Gateway[4]{};
		u8 DNS1[4]{};
		u8 DNS2[4]{};
		bool AutoMask{true};
		bool AutoGateway{true};
		DnsMode ModeDNS1{DnsMode::Auto};
		DnsMode ModeDNS2{DnsMode::Auto};

		std::vector<HostEntry> EthHosts;

		bool HddEnable{false};
		std::string HddFile;

		DEV9Options();

		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const DEV9Options& right) const;
		bool operator!=(const DEV9Options& right) const;

	protected:
		static void LoadIPHelper(u8* field, const std::string& setting);
		static std::string SaveIPHelper(u8* field);
	};

	// ------------------------------------------------------------------------
	// NOTE: The GUI's GameFixes panel is dependent on the order of bits in this structure.
	struct GamefixOptions
	{
		BITFIELD32()
		bool
			// No reader: eeMulRound (FPU.cpp) and emitDefectiveFmul
			// (iFPUd-arm64.cpp) model the multiplier defect this patched one
			// product of. The bit stays because its GamefixId indexes
			// vu_capture's on-disk gamefix mask.
			FpuMulHack : 1, // Tales of Destiny hangs.
			GoemonTlbHack : 1, // Gomeon tlb miss hack. The game need to access unmapped virtual address. Instead to handle it as exception, tlb are preloaded at startup
			SoftwareRendererFMVHack : 1, // Switches to software renderer for FMVs
			SkipMPEGHack : 1, // Skips MPEG videos (Katamari and other games need this)
			OPHFlagHack : 1, // Bleach Blade Battlers
			EETimingHack : 1, // General purpose timing hack.
			InstantDMAHack : 1, // Instantly complete DMA's if possible, good for cache emulation problems.
			DMABusyHack : 1, // Denies writes to the DMAC when it's busy. This is correct behaviour but bad timing can cause problems.
			GIFFIFOHack : 1, // Enabled the GIF FIFO (more correct but slower)
			VIFFIFOHack : 1, // Pretends to fill the non-existant VIF FIFO Buffer.
			VIF1StallHack : 1, // Like above, processes FIFO data before the stall is allowed (to make sure data goes over).
			VuAddSubHack : 1, // Tri-ace games, they use an encryption algorithm that requires VU ADDI opcode to be bit-accurate.
			IbitHack : 1, // I bit hack. Needed to stop constant VU recompilation in some games
			VUSyncHack : 1, // Makes microVU run behind the EE to avoid VU register reading/writing sync issues. Useful for M-Bit games
			VUOverflowHack : 1, // Tries to simulate overflow flag checks (not really possible on x86 without soft floats)
			XgKickHack : 1, // Erementar Gerad, adds more delay to VU XGkick instructions. Corrects the color of some graphics, but breaks Tri-ace games and others.
			BlitInternalFPSHack : 1, // Disables privileged register write-based FPS detection.
			FullVU0SyncHack : 1; // Forces tight VU0 sync on every COP2 instruction.
		BITFIELD_END

		GamefixOptions();
		void LoadSave(SettingsWrapper& wrap);
		GamefixOptions& DisableAll();

		static const char* GetGameFixName(GamefixId id);

		bool Get(GamefixId id) const;
		void Set(GamefixId id, bool enabled = true);
		void Clear(GamefixId id) { Set(id, false); }

		bool operator==(const GamefixOptions& right) const;
		bool operator!=(const GamefixOptions& right) const;
	};

	// ------------------------------------------------------------------------
	struct SpeedhackOptions
	{
		static constexpr s8 MIN_EE_CYCLE_RATE = -3;
		static constexpr s8 MAX_EE_CYCLE_RATE = 3;
		static constexpr u8 MAX_EE_CYCLE_SKIP = 3;

		BITFIELD32()
		bool
			fastCDVD : 1, // enables fast CDVD access
			IntcStat : 1, // tells Pcsx2 to fast-forward through intc_stat waits.
			WaitLoop : 1, // enables constant loop detection and fast-forwarding
			vuFlagHack : 1, // microVU specific flag hack
			vuThread : 1, // Enable Threaded VU1
			vu1Instant : 1; // Enable Instant VU1 (Without MTVU only)
		BITFIELD_END

		s8 EECycleRate; // EE cycle rate selector (1.0, 1.5, 2.0)
		u8 EECycleSkip; // EE Cycle skip factor (0, 1, 2, or 3)

		SpeedhackOptions();
		void LoadSave(SettingsWrapper& conf);
		SpeedhackOptions& DisableAll();

		void Set(SpeedHack id, int value);

		bool operator==(const SpeedhackOptions& right) const;
		bool operator!=(const SpeedhackOptions& right) const;

		static const char* GetSpeedHackName(SpeedHack id);
		static std::optional<SpeedHack> ParseSpeedHackName(const std::string_view name);
	};

	// ------------------------------------------------------------------------
	struct DebugAnalysisOptions
	{

		static const char* RunConditionNames[];
		static const char* FunctionScanModeNames[];

		DebugAnalysisCondition RunCondition = DebugAnalysisCondition::IF_DEBUGGER_IS_OPEN;
		bool GenerateSymbolsForIRXExports = true;

		bool AutomaticallySelectSymbolsToClear = true;
		std::vector<DebugSymbolSource> SymbolSources;

		bool ImportSymbolsFromELF = true;
		bool ImportSymFileFromDefaultLocation = true;
		bool DemangleSymbols = true;
		bool DemangleParameters = true;
		std::vector<DebugExtraSymbolFile> ExtraSymbolFiles;

		DebugFunctionScanMode FunctionScanMode = DebugFunctionScanMode::SCAN_ELF;
		bool CustomFunctionScanRange = false;
		std::string FunctionScanStartAddress;
		std::string FunctionScanEndAddress;

		bool GenerateFunctionHashes = true;

		void LoadSave(SettingsWrapper& wrap);

		friend auto operator<=>(const DebugAnalysisOptions& lhs, const DebugAnalysisOptions& rhs) = default;
	};

	// ------------------------------------------------------------------------
	struct EmulationSpeedOptions
	{
		BITFIELD32()
		bool SyncToHostRefreshRate : 1;
		bool UseVSyncForTiming : 1;
		BITFIELD_END

		float NominalScalar{1.0f};
		float TurboScalar{2.0f};
		float SlomoScalar{0.5f};

		EmulationSpeedOptions();

		void LoadSave(SettingsWrapper& wrap);
		void SanityCheck();

		bool operator==(const EmulationSpeedOptions& right) const;
		bool operator!=(const EmulationSpeedOptions& right) const;
	};

	// ------------------------------------------------------------------------
	struct FilenameOptions
	{
		std::string Bios;

		FilenameOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const FilenameOptions& right) const;
		bool operator!=(const FilenameOptions& right) const;
	};

	// ------------------------------------------------------------------------
	struct USBOptions
	{
		static constexpr u32 NUM_PORTS = 2;

		struct Port
		{
			s32 DeviceType;
			u32 DeviceSubtype;

			bool operator==(const USBOptions::Port& right) const;
			bool operator!=(const USBOptions::Port& right) const;
		};

		std::array<Port, NUM_PORTS> Ports;

		USBOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const USBOptions& right) const;
		bool operator!=(const USBOptions& right) const;
	};

	// ------------------------------------------------------------------------
	struct PadOptions
	{
		static constexpr u32 NUM_PORTS = 8;

		struct Port
		{
			Pad::ControllerType Type;

			bool operator==(const PadOptions::Port& right) const;
			bool operator!=(const PadOptions::Port& right) const;
		};

		std::array<Port, NUM_PORTS> Ports;

		BITFIELD32()
		bool
			MultitapPort0_Enabled : 1,
			MultitapPort1_Enabled;
		BITFIELD_END

		PadOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool IsMultitapPortEnabled(u32 port) const
		{
			return (port == 0) ? MultitapPort0_Enabled : MultitapPort1_Enabled;
		}

		bool operator==(const PadOptions& right) const;
		bool operator!=(const PadOptions& right) const;
	};

	// ------------------------------------------------------------------------
	// Options struct for each memory card.
	//
	struct McdOptions
	{
		std::string Filename; // user-configured location of this memory card
		bool Enabled; // memory card enabled (if false, memcard will not show up in-game)
		MemoryCardType Type; // the memory card implementation that should be used
	};

	// ------------------------------------------------------------------------

	struct AchievementsOptions
	{
		static constexpr u32 MINIMUM_NOTIFICATION_DURATION = 3;
		static constexpr u32 MAXIMUM_NOTIFICATION_DURATION = 30;
		static constexpr u32 DEFAULT_NOTIFICATION_DURATION = 5;
		static constexpr u32 DEFAULT_LEADERBOARD_DURATION = 10;

		static const char* OverlayPositionNames[(size_t)AchievementOverlayPosition::MaxCount + 1];

		BITFIELD32()
		bool
			Enabled : 1,
			HardcoreMode : 1,
			EncoreMode : 1,
			SpectatorMode : 1,
			UnofficialTestMode : 1,
			Notifications : 1,
			LeaderboardNotifications : 1,
			SoundEffects : 1,
			InfoSound : 1,
			UnlockSound : 1,
			LBSubmitSound : 1,
			Overlays : 1,
			LBOverlays : 1;
		BITFIELD_END

		u32 NotificationsDuration = DEFAULT_NOTIFICATION_DURATION;
		u32 LeaderboardsDuration = DEFAULT_LEADERBOARD_DURATION;
		AchievementOverlayPosition OverlayPosition = AchievementOverlayPosition::BottomRight;
		OsdOverlayPos NotificationPosition = OsdOverlayPos::TopLeft;

		std::string InfoSoundName;
		std::string UnlockSoundName;
		std::string LBSubmitSoundName;

		AchievementsOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const AchievementsOptions& right) const;
		bool operator!=(const AchievementsOptions& right) const;
	};

	struct SavestateOptions
	{
		SavestateOptions();
		void LoadSave(SettingsWrapper& wrap);

		SavestateCompressionMethod CompressionType = SavestateCompressionMethod::Zstandard;
		SavestateCompressionLevel CompressionRatio = SavestateCompressionLevel::Medium;

		bool operator==(const SavestateOptions& right) const;
		bool operator!=(const SavestateOptions& right) const;
	};

	// ------------------------------------------------------------------------

	BITFIELD32()
	bool
		CdvdVerboseReads : 1, // enables cdvd read activity verbosely dumped to the console
		CdvdDumpBlocks : 1, // enables cdvd block dumping
		CdvdPrecache : 1, // enables cdvd precaching of compressed images
		EnablePatches : 1, // enables patch detection and application
		EnableCheats : 1, // enables cheat detection and application
		EnablePINE : 1, // enables inter-process communication
		EnableWideScreenPatches : 1,
		EnableNoInterlacingPatches : 1,
		EnableFastBoot : 1,
		EnableFastBootFastForward : 1,
		EnableThreadPinning : 1,
		// TODO - Vaser - where are these settings exposed in the Qt UI?
		EnableRecordingTools : 1,
		EnableGameFixes : 1, // enables automatic game fixes
		SaveStateOnShutdown : 1, // default value for saving state on shutdown
		EnableDiscordPresence : 1, // enables discord rich presence integration
		UseSavestateSelector : 1,
		InhibitScreensaver : 1,
		BackupSavestate : 1,
		ManuallySetRealTimeClock : 1, // passes user-set real-time clock information to cdvd at startup
		UseSystemLocaleFormat : 1, // presents OS time format instead of yyyy-MM-dd HH:mm:ss for manual RTC

		HostFs : 1,

		WarnAboutUnsafeSettings : 1;
	BITFIELD_END

	CpuOptions Cpu;
	GSOptions GS;
	SpeedhackOptions Speedhacks;
	GamefixOptions Gamefixes;
	ProfilerOptions Profiler;
	DebugAnalysisOptions DebuggerAnalysis;
	EmulationSpeedOptions EmulationSpeed;
	SavestateOptions Savestate;
	SPU2Options SPU2;
	DEV9Options DEV9;
	USBOptions USB;
	PadOptions Pad;

	TraceLogFilters Trace;

	FilenameOptions BaseFilenames;

	AchievementsOptions Achievements;

	// Memorycard options - first 2 are default slots, last 6 are multitap 1 and 2
	// slots (3 each)
	McdOptions Mcd[8];
	std::string GzipIsoIndexTemplate; // for quick-access index with gzipped ISO

	int PINESlot;

	int RtcYear;
	int RtcMonth;
	int RtcDay;
	int RtcHour;
	int RtcMinute;
	int RtcSecond;

	// Set at runtime, not loaded from config.
	std::string CurrentBlockdump;
	std::string CurrentIRX;
	std::string CurrentGameArgs;
	std::string CustomDataPath;
	AspectRatioType CurrentAspectRatio = AspectRatioType::RAuto4_3_3_2;
	// Fall back aspect ratio for games that have patches (when AspectRatioType::RAuto4_3_3_2) is active.
	float CurrentCustomAspectRatio = 0.f;
	bool IsPortableMode = false;

	Pcsx2Config();
	void LoadSave(SettingsWrapper& wrap);
	void LoadSaveCore(SettingsWrapper& wrap);
	void LoadSaveMemcards(SettingsWrapper& wrap);

	/// Reloads options affected by patches.
	void ReloadPatchAffectingOptions();

	std::string FullpathToBios() const;
	std::string FullpathToMcd(uint slot) const;

	bool operator==(const Pcsx2Config& right) const = delete;
	bool operator!=(const Pcsx2Config& right) const = delete;

	/// Copies runtime configuration settings (e.g. frame limiter state).
	void CopyRuntimeConfig(Pcsx2Config& cfg);

	/// Copies configuration from one file to another. Does not copy controller settings.
	/// Copies a configuration into a per-game settings file, writing only the values
	/// that deviate from what the game would run with anyway — stock defaults, and the
	/// game database's own opinion for `game_serial`. Everything else is left absent,
	/// because in a per-game file a key that is present is a key the player is taken to
	/// have claimed, and the database then stands aside for it.
	static void CopyConfiguration(SettingsInterface* dest_si, SettingsInterface& src_si, std::string_view game_serial);

	/// Clears all core keys from the specified interface.
	static void ClearConfiguration(SettingsInterface* dest_si);

	/// Removes keys that are not valid for per-game settings.
	static void ClearInvalidPerGameConfiguration(SettingsInterface* si);
};

extern Pcsx2Config EmuConfig;

namespace EmuFolders
{
	extern std::string AppRoot;
	extern std::string DataRoot;
	extern std::string Settings;
	extern std::string Bios;
	extern std::string Snapshots;
	extern std::string Savestates;
	extern std::string MemoryCards;
	extern std::string Logs;
	extern std::string Cheats;
	extern std::string Patches;
	extern std::string Resources;
	extern std::string UserResources;
	extern std::string Cache;
	extern std::string Covers;
	extern std::string GameSettings;
	extern std::string Textures;
	extern std::string InputProfiles;
	extern std::string Videos;
	extern std::string DebuggerLayouts;
	extern std::string DebuggerSettings;

	/// Initializes critical folders (AppRoot, DataRoot, Settings). Call once on startup.
	void SetAppRoot();
	bool SetResourcesDirectory();
	bool SetDataDirectory(Error* error);

	// Assumes that AppRoot and DataRoot have been initialized.
	void SetDefaults(SettingsInterface& si);
	void LoadConfig(SettingsInterface& si);
	bool EnsureFoldersExist();

	/// Opens the specified log file for writing.
	std::FILE* OpenLogFile(std::string_view name, const char* mode);

	/// Returns the path to a resource file, allowing the user to override it.
	std::string GetOverridableResourcePath(std::string_view name);
} // namespace EmuFolders

/////////////////////////////////////////////////////////////////////////////////////////
// Helper Macros for Reading Emu Configurations.
//

// ------------ CPU / Recompiler Options ---------------

// (ARM64 Phase 7.8) microVU0/1 are now ported, so REC_VU1/THREAD_VU1 track the config
// on both architectures — the old ARM64 hardcoded-false stub would make GetGSPacketSize
// take its `!REC_VU1` path and return the XGKICK packet size with the bit31 EOP flag set,
// which mVU_XGKICK_ then mis-reads as a multi-GB transfer (memcpy crash on the first kick).
#define REC_VU1 (EmuConfig.Cpu.Recompiler.EnableVU1)
#define THREAD_VU1 (REC_VU1 && EmuConfig.Speedhacks.vuThread)
#define INSTANT_VU1 (EmuConfig.Speedhacks.vu1Instant)
#define CHECK_EEREC (EmuConfig.Cpu.Recompiler.EnableEE)
#define CHECK_CACHE (EmuConfig.Cpu.Recompiler.EnableEECache)
#define CHECK_IOPREC (EmuConfig.Cpu.Recompiler.EnableIOP)
#define CHECK_FASTMEM (EmuConfig.Cpu.Recompiler.EnableEE && EmuConfig.Cpu.Recompiler.EnableFastmem)
#define CHECK_EXTRAMEM (memGetExtraMemMode())

//------------ SPECIAL GAME FIXES!!! ---------------
#define CHECK_VUADDSUBHACK (EmuConfig.Gamefixes.VuAddSubHack) // Special Fix for Tri-ace games, they use an encryption algorithm that requires VU addi opcode to be bit-accurate.
#define CHECK_XGKICKHACK (EmuConfig.Gamefixes.XgKickHack) // Special Fix for Erementar Gerad, adds more delay to VU XGkick instructions. Corrects the color of some graphics.
#define CHECK_EETIMINGHACK (EmuConfig.Gamefixes.EETimingHack) // Fix all scheduled events to happen in 1 cycle.
#define CHECK_INSTANTDMAHACK (EmuConfig.Gamefixes.InstantDMAHack) // Attempt to finish DMA's instantly, useful for games which rely on cache emulation.
#define CHECK_SKIPMPEGHACK (EmuConfig.Gamefixes.SkipMPEGHack) // Finds sceMpegIsEnd pattern to tell the game the mpeg is finished (Katamari and a lot of games need this)
#define CHECK_OPHFLAGHACK (EmuConfig.Gamefixes.OPHFlagHack) // Bleach Blade Battlers
#define CHECK_DMABUSYHACK (EmuConfig.Gamefixes.DMABusyHack) // Denies writes to the DMAC when it's busy. This is correct behaviour but bad timing can cause problems.
#define CHECK_VIFFIFOHACK (EmuConfig.Gamefixes.VIFFIFOHack) // Pretends to fill the non-existant VIF FIFO Buffer.
#define CHECK_VIF1STALLHACK (EmuConfig.Gamefixes.VIF1StallHack) // Like above, processes FIFO data before the stall is allowed (to make sure data goes over).
#define CHECK_GIFFIFOHACK (EmuConfig.Gamefixes.GIFFIFOHack) // Enabled the GIF FIFO (more correct but slower)
#define CHECK_VUOVERFLOWHACK (EmuConfig.Gamefixes.VUOverflowHack) // Special Fix for Superman Returns, they check for overflows on PS2 floats which we can't do without soft floats.
#define CHECK_FULLVU0SYNCHACK (EmuConfig.Gamefixes.FullVU0SyncHack)

//------------ Advanced Options!!! ---------------
#define CHECK_VU_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0Overflow : EmuConfig.Cpu.Recompiler.vu1Overflow)
#define CHECK_VU_EXTRA_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0ExtraOverflow : EmuConfig.Cpu.Recompiler.vu1ExtraOverflow) // If enabled, Operands are clamped before being used in the VU recs
#define CHECK_VU_SIGN_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0SignOverflow : EmuConfig.Cpu.Recompiler.vu1SignOverflow)
#define CHECK_VU_EXACT(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0ExactMode : EmuConfig.Cpu.Recompiler.vu1ExactMode) // GameDB vu0/vu1ClampMode 4: mode 3 plus the VU's own arithmetic and status flags -- the adder's guard mask, the divide unit's recurrence and the EFU's series, the multiplier's one-ULP deficit, and the FMAC's saturation ceiling with its MAC U and MAC O.

#define CHECK_FPU_OVERFLOW (EmuConfig.Cpu.Recompiler.fpuOverflow)
#define CHECK_FPU_EXTRA_OVERFLOW (EmuConfig.Cpu.Recompiler.fpuExtraOverflow) // If enabled, Operands are checked for infinities before being used in the FPU recs
#define CHECK_FPU_EXTRA_FLAGS 1 // Always enabled now // Sets D/I flags on FPU instructions
#define CHECK_FPU_FULL (EmuConfig.Cpu.Recompiler.fpuFullMode) // GameDB eeClampMode >= 3: the EE FPU's arithmetic is iFPUd's, computed in double over a relocated FPR file. Below it the single-precision fast path in iFPU-arm64.cpp runs.
#define CHECK_FPU_EXACT (EmuConfig.Cpu.Recompiler.fpuExactMode) // GameDB eeClampMode 4: mode 3 plus the rest of the EE multiplier's one-ULP deficit, at emitDefectiveFmul (iFPUd-arm64.cpp).
#define CHECK_FPU_GUARDED (EmuConfig.Cpu.Recompiler.fpuGuardedAddSub) // If enabled (default), add/sub emulate the PS2 FPU's missing mantissa guard bits on the single-precision fast path. Disable only for EE-heavy titles confirmed not to need it.

//------------ EE Recompiler defines - Comment to disable a recompiler ---------------

#define SHIFT_RECOMPILE // Speed majorly reduced if disabled
#define BRANCH_RECOMPILE // Speed extremely reduced if disabled - more then shift

// Disabling all the recompilers in this block is interesting, as it still runs at a reasonable rate.
// It also adds a few glitches. Really reminds me of the old Linux 64-bit version. --arcum42
#define ARITHMETICIMM_RECOMPILE
#define ARITHMETIC_RECOMPILE
#define MULTDIV_RECOMPILE
#define JUMP_RECOMPILE
#define LOADSTORE_RECOMPILE
#define MOVE_RECOMPILE
#define MMI_RECOMPILE
#define MMI0_RECOMPILE
#define MMI1_RECOMPILE
#define MMI2_RECOMPILE
#define MMI3_RECOMPILE
#define FPU_RECOMPILE
#define CP0_RECOMPILE
#define CP2_RECOMPILE

// You can't recompile ARITHMETICIMM without ARITHMETIC.
#ifndef ARITHMETIC_RECOMPILE
#undef ARITHMETICIMM_RECOMPILE
#endif

#define EE_CONST_PROP 1 // rec2 - enables constant propagation (faster)

// Change to 1 for console logs of SIF, GPU (PS1 mode) and MDEC (PS1 mode).
// These do spam a lot though!
#define PSX_EXTRALOGS 0

#undef BITFIELD32
#undef BITFIELD_END
