// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSGPUProfilePrivate.h"

#include <array>
#include <cctype>

namespace GpuProfileDetail
{
namespace
{
struct AdrenoSpec
{
	u16 model;
	char suffix;
	MobileGpuArchitecture architecture;
	MobileGsTuning tuning;
};

constexpr MobileGsTuning T(u32 pool, u32 target_age, u32 texture_age, bool prefer_new = false)
{
	return MobileGsTuning{pool < 128, prefer_new, pool, target_age, pool, texture_age};
}

// Renderer names are stable (for example "Adreno (TM) 740"), while Snapdragon product names are not.
// Keep one entry per shipping renderer model so adjacent parts are not silently treated as equivalent.
static constexpr std::array<AdrenoSpec, 53> s_adreno_specs = {{
	{200, 0, MobileGpuArchitecture::Adreno2xx, T(48, 4, 4)},
	{203, 0, MobileGpuArchitecture::Adreno2xx, T(48, 4, 4)},
	{205, 0, MobileGpuArchitecture::Adreno2xx, T(48, 4, 4)},
	{220, 0, MobileGpuArchitecture::Adreno2xx, T(52, 4, 4)},
	{225, 0, MobileGpuArchitecture::Adreno2xx, T(52, 4, 4)},
	{302, 0, MobileGpuArchitecture::Adreno3xx, T(52, 4, 4)},
	{303, 0, MobileGpuArchitecture::Adreno3xx, T(52, 4, 4)},
	{304, 0, MobileGpuArchitecture::Adreno3xx, T(56, 5, 4)},
	{305, 0, MobileGpuArchitecture::Adreno3xx, T(56, 5, 4)},
	{306, 0, MobileGpuArchitecture::Adreno3xx, T(60, 5, 4)},
	{308, 0, MobileGpuArchitecture::Adreno3xx, T(60, 5, 4)},
	{320, 0, MobileGpuArchitecture::Adreno3xx, T(64, 5, 5)},
	{330, 0, MobileGpuArchitecture::Adreno3xx, T(68, 5, 5)},
	{405, 0, MobileGpuArchitecture::Adreno4xx, T(64, 5, 5)},
	{418, 0, MobileGpuArchitecture::Adreno4xx, T(68, 5, 5)},
	{420, 0, MobileGpuArchitecture::Adreno4xx, T(72, 6, 5)},
	{430, 0, MobileGpuArchitecture::Adreno4xx, T(80, 6, 5)},
	{505, 0, MobileGpuArchitecture::Adreno5xx, T(68, 5, 5)},
	{506, 0, MobileGpuArchitecture::Adreno5xx, T(72, 6, 5)},
	{508, 0, MobileGpuArchitecture::Adreno5xx, T(76, 6, 5)},
	{509, 0, MobileGpuArchitecture::Adreno5xx, T(80, 6, 5)},
	{510, 0, MobileGpuArchitecture::Adreno5xx, T(84, 6, 6)},
	{512, 0, MobileGpuArchitecture::Adreno5xx, T(88, 7, 6)},
	{530, 0, MobileGpuArchitecture::Adreno5xx, T(104, 8, 6)},
	{540, 0, MobileGpuArchitecture::Adreno5xx, T(112, 8, 7)},
	{605, 0, MobileGpuArchitecture::Adreno6xx, T(72, 6, 5)},
	{608, 0, MobileGpuArchitecture::Adreno6xx, T(76, 6, 5)},
	{610, 0, MobileGpuArchitecture::Adreno6xx, T(80, 6, 5)},
	{612, 0, MobileGpuArchitecture::Adreno6xx, T(84, 7, 5)},
	{613, 0, MobileGpuArchitecture::Adreno6xx, T(88, 7, 6)},
	{615, 0, MobileGpuArchitecture::Adreno6xx, T(92, 7, 6)},
	{616, 0, MobileGpuArchitecture::Adreno6xx, T(96, 8, 6)},
	{618, 0, MobileGpuArchitecture::Adreno6xx, T(100, 8, 6)},
	{619, 'l', MobileGpuArchitecture::Adreno6xx, T(88, 7, 6)},
	{619, 0, MobileGpuArchitecture::Adreno6xx, T(96, 8, 6)},
	{620, 0, MobileGpuArchitecture::Adreno6xx, T(112, 8, 7)},
	{630, 0, MobileGpuArchitecture::Adreno6xx, T(128, 9, 7, true)},
	{640, 0, MobileGpuArchitecture::Adreno6xx, T(140, 10, 8, true)},
	{642, 'l', MobileGpuArchitecture::Adreno6xx, T(112, 8, 7)},
	{642, 0, MobileGpuArchitecture::Adreno6xx, T(124, 9, 7)},
	{643, 0, MobileGpuArchitecture::Adreno6xx, T(128, 9, 7, true)},
	{644, 0, MobileGpuArchitecture::Adreno6xx, T(132, 9, 7, true)},
	{650, 0, MobileGpuArchitecture::Adreno6xx, T(144, 10, 8, true)},
	{660, 0, MobileGpuArchitecture::Adreno6xx, T(152, 11, 8, true)},
	{675, 0, MobileGpuArchitecture::Adreno6xx, T(152, 11, 8, true)},
	{680, 0, MobileGpuArchitecture::Adreno6xx, T(156, 11, 8, true)},
	{685, 0, MobileGpuArchitecture::Adreno6xx, T(156, 11, 8, true)},
	{690, 0, MobileGpuArchitecture::Adreno6xx, T(160, 12, 8, true)},
	{695, 0, MobileGpuArchitecture::Adreno6xx, T(160, 12, 8, true)},
	{702, 0, MobileGpuArchitecture::Adreno7xx, T(88, 7, 6)},
	{710, 0, MobileGpuArchitecture::Adreno7xx, T(104, 8, 7)},
	{720, 0, MobileGpuArchitecture::Adreno7xx, T(128, 9, 7, true)},
	// Snapdragon 7 Gen 4 and Dragonwing Q-7790 (Retroid Pocket Duo): an a7xx generation 1 part derived
	// from the 730, with fewer cores and caches and 1 MiB of GMEM like the 720. The pool sizes and ages
	// are memory-budget heuristics, so this row copies the 720's (same GMEM, unconstrained, same values
	// as the generic 7xx fallback).
	{722, 0, MobileGpuArchitecture::Adreno7xx, T(128, 9, 7, true)},
}};

// Later 7xx/8xx models are a different renderer generation, kept separate from the table above.
static constexpr std::array<AdrenoSpec, 10> s_recent_adreno_specs = {{
	{725, 0, MobileGpuArchitecture::Adreno7xx, T(140, 10, 8, true)},
	{730, 0, MobileGpuArchitecture::Adreno7xx, T(144, 10, 8, true)},
	{735, 0, MobileGpuArchitecture::Adreno7xx, T(152, 11, 8, true)},
	{740, 0, MobileGpuArchitecture::Adreno7xx, T(160, 12, 8, true)},
	{750, 0, MobileGpuArchitecture::Adreno7xx, T(160, 12, 8, true)},
	{810, 0, MobileGpuArchitecture::Adreno8xx, T(128, 9, 7, true)},
	{825, 0, MobileGpuArchitecture::Adreno8xx, T(148, 10, 8, true)},
	{829, 0, MobileGpuArchitecture::Adreno8xx, T(156, 11, 8, true)},
	{830, 0, MobileGpuArchitecture::Adreno8xx, T(160, 12, 8, true)},
	{840, 0, MobileGpuArchitecture::Adreno8xx, T(160, 12, 8, true)},
}};

// Qualcomm's "A" marketing names, which some drivers report as the renderer name in place of the
// model number ("Adreno (TM) A12"), and the model each one is. Only names identified on a device
// belong here. An unlisted one parses as its bare number, which no table row has, so on Vulkan the
// chip id gets to name the part (AdrenoModelFromChipId).
struct AdrenoMarketingName
{
	u16 number;
	u16 model;
};

static constexpr std::array<AdrenoMarketingName, 1> s_adreno_marketing_names = {{
	// Anbernic RG 55G1 (2026-10-23): the stock driver names it "Adreno (TM) A12" with Vulkan deviceID
	// 0x06010300, the 613's chip id, and Turnip on the same device names it "Adreno (TM) 613".
	{12, 613},
}};

static bool ParseAdrenoModel(std::string_view hints, u16* model, char* suffix)
{
	const size_t adreno = hints.find("adreno");
	if (adreno == std::string_view::npos)
		return false;

	size_t pos = adreno + 6;
	while (pos < hints.size() && !std::isdigit(static_cast<unsigned char>(hints[pos])))
		pos++;
	if (pos == hints.size())
		return false;

	// "a12": a marketing name, an 'a' starting a word directly before the number.
	const bool marketing_name = (pos >= adreno + 8) && hints[pos - 1] == 'a' &&
	                            !std::isalnum(static_cast<unsigned char>(hints[pos - 2]));

	u32 value = 0;
	while (pos < hints.size() && std::isdigit(static_cast<unsigned char>(hints[pos])))
	{
		value = value * 10 + static_cast<u32>(hints[pos++] - '0');
		if (value > 9999)
			return false;
	}

	*model = static_cast<u16>(value);
	*suffix = (pos < hints.size() && hints[pos] == 'l') ? 'l' : 0;

	if (marketing_name)
	{
		for (const AdrenoMarketingName& name : s_adreno_marketing_names)
		{
			if (name.number == value)
			{
				*model = name.model;
				*suffix = 0;
				break;
			}
		}
	}
	return true;
}

// The model a Qualcomm Vulkan deviceID spells, or 0 when it spells none. Both the Qualcomm driver and
// Turnip report the kernel's chip id there. Up to the 6xx parts that id is 0xCCMMmmPP: core, major and
// minor are the model's three digits and PP is a revision, so the Adreno 650 reports 0x06050001 and
// the 613 0x06010300.
//
// Only ids of that form, core 2 to 6, are decoded. From the 7xx parts on, the digits stop matching the
// model: Mesa's device table gives the 725 0x07030002 and the 730 0x07030001, and the later parts use
// a different layout altogether (the 740 is 0x43050a01, the 722 0x43020100). Even within 6xx two of
// Mesa's ids spell a number that is not the part's marketing name (0x06030500 reads 635 for an
// Adreno 643, 0x06060201 reads 662 for a 644); neither number is in the tables above, so the caller,
// which accepts only a listed model, ignores them.
static u16 AdrenoModelFromChipId(u32 vendor_id, u32 device_id)
{
	if (vendor_id != GpuVendorID::Qualcomm)
		return 0;

	const u32 core = (device_id >> 24) & 0xff;
	const u32 major = (device_id >> 16) & 0xff;
	const u32 minor = (device_id >> 8) & 0xff;
	if (core < 2 || core > 6 || major > 9 || minor > 9)
		return 0;

	return static_cast<u16>(core * 100 + major * 10 + minor);
}

static MobileGpuArchitecture ArchitectureForUnknownAdreno(u16 model)
{
	switch (model / 100)
	{
		case 2: return MobileGpuArchitecture::Adreno2xx;
		case 3: return MobileGpuArchitecture::Adreno3xx;
		case 4: return MobileGpuArchitecture::Adreno4xx;
		case 5: return MobileGpuArchitecture::Adreno5xx;
		case 6: return MobileGpuArchitecture::Adreno6xx;
		case 7: return MobileGpuArchitecture::Adreno7xx;
		case 8: return MobileGpuArchitecture::Adreno8xx;
		default: return MobileGpuArchitecture::Unknown;
	}
}

static MobileGsTuning FallbackTuningForAdreno(u16 model)
{
	switch (model / 100)
	{
		case 2: return T(48, 4, 4);
		case 3: return T(56, 5, 4);
		case 4: return T(68, 5, 5);
		case 5: return T(80, 6, 5);
		case 6: return T(96, 8, 6);
		case 7: return T(128, 9, 7, true);
		case 8: return T(144, 10, 8, true);
		default: return MakeConservativeMobileGsTuning();
	}
}

template <size_t N>
static const AdrenoSpec* FindAdrenoSpecIn(const std::array<AdrenoSpec, N>& specs, u16 model, char suffix)
{
	for (const AdrenoSpec& spec : specs)
	{
		if (spec.model == model && spec.suffix == suffix)
			return &spec;
	}
	return nullptr;
}

static const AdrenoSpec* FindAdrenoSpec(u16 model, char suffix)
{
	const AdrenoSpec* spec = FindAdrenoSpecIn(s_adreno_specs, model, suffix);
	return spec ? spec : FindAdrenoSpecIn(s_recent_adreno_specs, model, suffix);
}
} // namespace

bool LooksLikeAdreno(std::string_view lowered_hints)
{
	return ContainsAny(lowered_hints, {"adreno", "qualcomm", "qcom", "snapdragon"});
}

ResolvedGpuProfile ResolveAdrenoProfile(std::string_view lowered_hints, u32 vendor_id, u32 device_id)
{
	ResolvedGpuProfile resolved;
	resolved.gpu.name = "Unknown Adreno";
	resolved.tuning = MakeConservativeMobileGsTuning();

	// Snapdragon X laptop parts occasionally appear through shared Android/ANGLE code paths.
	if (ContainsAny(lowered_hints, {"adreno x2-85", "adreno x2 85"}))
	{
		resolved.gpu = {MobileGpuArchitecture::AdrenoX, 285, 0, true, "Adreno X2-85"};
		resolved.tuning = T(160, 12, 8, true);
		return resolved;
	}
	if (ContainsAny(lowered_hints, {"adreno x2-45", "adreno x2 45"}))
	{
		resolved.gpu = {MobileGpuArchitecture::AdrenoX, 245, 0, true, "Adreno X2-45"};
		resolved.tuning = T(160, 12, 8, true);
		return resolved;
	}
	if (ContainsAny(lowered_hints, {"adreno x1-85", "adreno x1 85"}))
	{
		resolved.gpu = {MobileGpuArchitecture::AdrenoX, 185, 0, true, "Adreno X1-85"};
		resolved.tuning = T(160, 12, 8, true);
		return resolved;
	}
	if (ContainsAny(lowered_hints, {"adreno x1-45", "adreno x1 45"}))
	{
		resolved.gpu = {MobileGpuArchitecture::AdrenoX, 145, 0, true, "Adreno X1-45"};
		resolved.tuning = T(152, 11, 8, true);
		return resolved;
	}

	u16 model = 0;
	char suffix = 0;
	const bool named = ParseAdrenoModel(lowered_hints, &model, &suffix);
	const AdrenoSpec* spec = named ? FindAdrenoSpec(model, suffix) : nullptr;

	// The name decides whenever it names a listed part. Otherwise a chip id that spells a listed part
	// names it instead: a renamed part, or one whose marketing name has no entry above.
	if (!spec)
	{
		const u16 chip_model = AdrenoModelFromChipId(vendor_id, device_id);
		if (const AdrenoSpec* chip_spec = chip_model ? FindAdrenoSpec(chip_model, 0) : nullptr)
		{
			spec = chip_spec;
			model = chip_model;
			suffix = 0;
		}
	}
	if (!named && !spec)
		return resolved;

	resolved.gpu.model_number = model;
	resolved.gpu.architecture = spec ? spec->architecture : ArchitectureForUnknownAdreno(model);
	resolved.gpu.recognized = (spec != nullptr);
	resolved.gpu.name = "Adreno " + std::to_string(model);
	if (suffix)
		resolved.gpu.name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(suffix))));
	resolved.tuning = spec ? spec->tuning : FallbackTuningForAdreno(model);
	return resolved;
}
} // namespace GpuProfileDetail
