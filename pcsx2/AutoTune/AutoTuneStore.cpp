// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "AutoTune/AutoTuneStore.h"

#include "Host.h"

#include "common/SettingsInterface.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
	constexpr const char* kChangePrefix = "Change.";
	constexpr const char* kRejectedPrefix = "Rejected.";

	// Splits on a separator, keeping empty fields.
	std::vector<std::string> Split(const std::string& text, char sep)
	{
		std::vector<std::string> out;
		size_t start = 0;
		for (;;)
		{
			const size_t pos = text.find(sep, start);
			if (pos == std::string::npos)
			{
				out.push_back(text.substr(start));
				return out;
			}
			out.push_back(text.substr(start, pos - start));
			start = pos + 1;
		}
	}

	bool StartsWith(const std::string& text, const char* prefix)
	{
		return text.rfind(prefix, 0) == 0;
	}

	// A settings text as a number: booleans as 0 and 1, numbers as themselves.
	std::optional<double> ParseNumber(const std::string& text)
	{
		if (text == "true" || text == "on")
			return 1.0;
		if (text == "false" || text == "off")
			return 0.0;
		if (text.empty())
			return std::nullopt;
		char* end = nullptr;
		const double v = std::strtod(text.c_str(), &end);
		if (end != text.c_str() + text.size())
			return std::nullopt;
		return v;
	}

	std::string EncodeChange(const AutoTune::Change& c)
	{
		return fmt::format("{}|{}|{}|{}|{}|{:.4f}", AutoTuneLeverName(c.lever), c.section, c.key,
			c.original ? ("=" + *c.original) : std::string("~"), c.value, c.gain);
	}

	std::optional<AutoTune::Change> DecodeChange(const std::string& line)
	{
		const std::vector<std::string> f = Split(line, '|');
		if (f.size() != 6)
			return std::nullopt;
		const std::optional<AutoTuneLever> lever = AutoTuneLeverFromName(f[0].c_str());
		if (!lever || f[1].empty() || f[2].empty() || f[3].empty())
			return std::nullopt;

		AutoTune::Change c;
		c.lever = *lever;
		c.section = f[1];
		c.key = f[2];
		if (f[3][0] == '=')
			c.original = f[3].substr(1);
		else if (f[3] != "~")
			return std::nullopt;
		c.value = f[4];
		c.gain = ParseNumber(f[5]).value_or(0.0);
		return c;
	}

	std::string EncodeRejection(const AutoTuneRejection& r)
	{
		return fmt::format("{}@{:g}", AutoTuneLeverName(r.lever), r.value);
	}

	std::optional<AutoTuneRejection> DecodeRejection(const std::string& line)
	{
		const size_t at = line.find('@');
		if (at == std::string::npos)
			return std::nullopt;
		const std::optional<AutoTuneLever> lever = AutoTuneLeverFromName(line.substr(0, at).c_str());
		const std::optional<double> value = ParseNumber(line.substr(at + 1));
		if (!lever || !value)
			return std::nullopt;
		return AutoTuneRejection{*lever, *value};
	}

	bool SameRejection(const AutoTuneRejection& a, const AutoTuneRejection& b)
	{
		return a.lever == b.lever && std::fabs(a.value - b.value) < 1e-6;
	}

	// Rewrites the whole section from the two lists, or drops it when both are empty.
	void WriteRecord(SettingsInterface& si, const std::vector<AutoTune::Change>& changes,
		const std::vector<AutoTuneRejection>& rejected)
	{
		si.RemoveSection(AutoTune::kStoreSection);
		if (changes.empty() && rejected.empty())
			return;

		for (size_t i = 0; i < changes.size(); i++)
			si.SetStringValue(AutoTune::kStoreSection, fmt::format("{}{}", kChangePrefix, i).c_str(), EncodeChange(changes[i]).c_str());
		for (size_t i = 0; i < rejected.size(); i++)
			si.SetStringValue(AutoTune::kStoreSection, fmt::format("{}{}", kRejectedPrefix, i).c_str(), EncodeRejection(rejected[i]).c_str());
	}

	std::string DisplayValue(AutoTuneLever lever, const std::string& text)
	{
		const std::optional<double> v = ParseNumber(text);
		if (!v)
			return text;

		switch (lever)
		{
			case AutoTuneLever::Resolution:
				return fmt::format("{:g}x", *v);

			case AutoTuneLever::IopThread:
			case AutoTuneLever::EeGovernor:
			case AutoTuneLever::GsBackThread:
				return *v != 0.0 ? std::string(TRANSLATE_SV("AutoTune", "on")) : std::string(TRANSLATE_SV("AutoTune", "off"));

			case AutoTuneLever::DownloadMode:
			{
				switch (static_cast<int>(std::lround(*v)))
				{
					case AutoTuneDownloadMode::Accurate:
						return std::string(TRANSLATE_SV("AutoTune", "Accurate"));
					case AutoTuneDownloadMode::AccurateForceFull:
						return std::string(TRANSLATE_SV("AutoTune", "Accurate (Force Full)"));
					case AutoTuneDownloadMode::NoReadbacks:
						return std::string(TRANSLATE_SV("AutoTune", "No Readbacks"));
					case AutoTuneDownloadMode::Unsynchronized:
						return std::string(TRANSLATE_SV("AutoTune", "Unsynchronized"));
					case AutoTuneDownloadMode::Disabled:
						return std::string(TRANSLATE_SV("AutoTune", "Disabled"));
					case AutoTuneDownloadMode::Asynchronous:
						return std::string(TRANSLATE_SV("AutoTune", "Asynchronous"));
					default:
						return text;
				}
			}

			case AutoTuneLever::Blending:
			{
				switch (static_cast<int>(std::lround(*v)))
				{
					case 0:
						return std::string(TRANSLATE_SV("AutoTune", "Minimum"));
					case 1:
						return std::string(TRANSLATE_SV("AutoTune", "Basic"));
					case 2:
						return std::string(TRANSLATE_SV("AutoTune", "Medium"));
					case 3:
						return std::string(TRANSLATE_SV("AutoTune", "High"));
					case 4:
						return std::string(TRANSLATE_SV("AutoTune", "Full"));
					case 5:
						return std::string(TRANSLATE_SV("AutoTune", "Maximum"));
					default:
						return text;
				}
			}

			default:
				return text;
		}
	}

	std::string LeverTitle(AutoTuneLever lever)
	{
		switch (lever)
		{
			case AutoTuneLever::IopThread:
				return std::string(TRANSLATE_SV("AutoTune", "IOP thread"));
			case AutoTuneLever::EeGovernor:
				return std::string(TRANSLATE_SV("AutoTune", "EE cycle-rate governor"));
			case AutoTuneLever::Resolution:
				return std::string(TRANSLATE_SV("AutoTune", "Resolution"));
			case AutoTuneLever::GsBackThread:
				return std::string(TRANSLATE_SV("AutoTune", "GS back thread"));
			case AutoTuneLever::DownloadMode:
				return std::string(TRANSLATE_SV("AutoTune", "Hardware download mode"));
			case AutoTuneLever::Blending:
				return std::string(TRANSLATE_SV("AutoTune", "Blending accuracy"));
			default:
				return "?";
		}
	}
} // namespace

AutoTune::LeverKey AutoTune::KeyForLever(AutoTuneLever lever)
{
	switch (lever)
	{
		case AutoTuneLever::IopThread:
			return {"EmuCore/Speedhacks", "iopThread"};
		case AutoTuneLever::EeGovernor:
			return {"EmuCore/Speedhacks", "DynamicEECycleRate"};
		case AutoTuneLever::Resolution:
			return {"EmuCore/GS", "upscale_multiplier"};
		case AutoTuneLever::GsBackThread:
			return {"EmuCore/GS", "GSBackThreadMode"};
		case AutoTuneLever::DownloadMode:
			return {"EmuCore/GS", "HWDownloadMode"};
		case AutoTuneLever::Blending:
			return {"EmuCore/GS", "accurate_blending_unit"};
		default:
			return {"", ""};
	}
}

bool AutoTune::IsLeverKey(const char* section, const char* key)
{
	for (int i = 0; i < static_cast<int>(AutoTuneLever::Count); i++)
	{
		const LeverKey k = KeyForLever(static_cast<AutoTuneLever>(i));
		if (std::strcmp(section, k.section) == 0 && std::strcmp(key, k.key) == 0)
			return true;
	}
	return false;
}

bool AutoTune::DisableForManualChange(SettingsInterface& game_si, const char* section, const char* key, bool enabled_globally)
{
	if (!IsLeverKey(section, key))
		return false;

	bool enabled = enabled_globally;
	game_si.GetBoolValue("EmuCore", "AutoTune", &enabled);
	if (!enabled)
		return false;

	game_si.SetBoolValue("EmuCore", "AutoTune", false);
	return true;
}

void AutoTune::WriteLeverValue(SettingsInterface& si, AutoTuneLever lever, double value)
{
	const LeverKey k = KeyForLever(lever);
	switch (lever)
	{
		case AutoTuneLever::IopThread:
		case AutoTuneLever::EeGovernor:
			si.SetBoolValue(k.section, k.key, value != 0.0);
			break;
		case AutoTuneLever::Resolution:
			si.SetFloatValue(k.section, k.key, static_cast<float>(value));
			break;
		case AutoTuneLever::GsBackThread:
			si.SetIntValue(k.section, k.key, value != 0.0 ? 1 : 0);
			break;
		case AutoTuneLever::DownloadMode:
		case AutoTuneLever::Blending:
			si.SetIntValue(k.section, k.key, static_cast<int>(std::lround(value)));
			break;
		default:
			break;
	}
}

std::vector<AutoTune::Change> AutoTune::ReadChanges(const SettingsInterface& si)
{
	std::vector<std::pair<size_t, Change>> found;
	for (const auto& [key, text] : si.GetKeyValueList(kStoreSection))
	{
		if (!StartsWith(key, kChangePrefix))
			continue;
		const std::optional<unsigned> index = StringUtil::FromChars<unsigned>(std::string_view(key).substr(std::char_traits<char>::length(kChangePrefix)));
		const std::optional<Change> change = DecodeChange(text);
		if (index && change)
			found.emplace_back(*index, *change);
	}
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

	std::vector<Change> out;
	out.reserve(found.size());
	for (auto& entry : found)
		out.push_back(std::move(entry.second));
	return out;
}

std::vector<AutoTuneRejection> AutoTune::ReadRejected(const SettingsInterface& si)
{
	std::vector<std::pair<size_t, AutoTuneRejection>> found;
	for (const auto& [key, text] : si.GetKeyValueList(kStoreSection))
	{
		if (!StartsWith(key, kRejectedPrefix))
			continue;
		const std::optional<unsigned> index = StringUtil::FromChars<unsigned>(std::string_view(key).substr(std::char_traits<char>::length(kRejectedPrefix)));
		const std::optional<AutoTuneRejection> rejection = DecodeRejection(text);
		if (index && rejection)
			found.emplace_back(*index, *rejection);
	}
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

	std::vector<AutoTuneRejection> out;
	out.reserve(found.size());
	for (const auto& entry : found)
		out.push_back(entry.second);
	return out;
}

AutoTune::Change AutoTune::Commit(SettingsInterface& si, AutoTuneLever lever, double value, double gain)
{
	const LeverKey k = KeyForLever(lever);
	std::vector<Change> changes = ReadChanges(si);
	const std::vector<AutoTuneRejection> rejected = ReadRejected(si);

	std::string current;
	const bool has_current = si.GetStringValue(k.section, k.key, &current);

	auto existing = std::find_if(changes.begin(), changes.end(), [&](const Change& c) {
		return c.section == k.section && c.key == k.key;
	});
	// A recorded change only counts while the key still holds what we wrote. If the player has changed it
	// since, what is there now is their value, and it is the one a reset must go back to.
	if (existing != changes.end() && !(has_current && SameValue(current, existing->value)))
	{
		changes.erase(existing);
		existing = changes.end();
	}

	std::optional<std::string> original;
	if (existing != changes.end())
		original = existing->original;
	else if (has_current)
		original = current;

	WriteLeverValue(si, lever, value);

	Change change;
	change.lever = lever;
	change.section = k.section;
	change.key = k.key;
	change.original = std::move(original);
	si.GetStringValue(k.section, k.key, &change.value);
	change.gain = gain;

	if (existing != changes.end())
	{
		change.gain = (1.0 + existing->gain) * (1.0 + gain) - 1.0;
		*existing = change;
	}
	else
	{
		changes.push_back(change);
	}

	WriteRecord(si, changes, rejected);
	return change;
}

void AutoTune::RecordRejected(SettingsInterface& si, const AutoTuneRejection& rejection)
{
	const std::vector<Change> changes = ReadChanges(si);
	std::vector<AutoTuneRejection> rejected = ReadRejected(si);
	if (std::any_of(rejected.begin(), rejected.end(), [&](const AutoTuneRejection& r) { return SameRejection(r, rejection); }))
		return;
	rejected.push_back(rejection);
	WriteRecord(si, changes, rejected);
}

AutoTune::ResetResult AutoTune::Reset(SettingsInterface& si)
{
	ResetResult result;
	const std::vector<Change> changes = ReadChanges(si);
	result.had_record = !changes.empty() || !ReadRejected(si).empty() || !si.GetKeyValueList(kStoreSection).empty();

	for (const Change& c : changes)
	{
		std::string current;
		const bool present = si.GetStringValue(c.section.c_str(), c.key.c_str(), &current);

		// Only a key that still holds what auto-tune wrote goes back. If the player has changed it since, or
		// removed it, it is theirs now. A missing key is flagged: a frontend that keeps the settings in its own
		// store can tell from there whether the player removed it.
		if (!present || !SameValue(current, c.value))
		{
			result.left_alone.push_back(LeftAlone{c, !present});
			continue;
		}

		Restored restored{c.section, c.key, c.original};
		if (c.original)
			si.SetStringValue(c.section.c_str(), c.key.c_str(), c.original->c_str());
		else
			si.DeleteValue(c.section.c_str(), c.key.c_str());
		result.restored.push_back(std::move(restored));
	}

	si.RemoveSection(kStoreSection);
	return result;
}

std::vector<std::string> AutoTune::Describe(const SettingsInterface& si)
{
	std::vector<std::string> lines;

	for (const Change& c : ReadChanges(si))
	{
		const std::string from = c.original ? DisplayValue(c.lever, *c.original) : std::string(TRANSLATE_SV("AutoTune", "default"));
		lines.push_back(fmt::format(TRANSLATE_FS("AutoTune", "{0}: {1} -> {2} ({3:+.0f}% speed)"), LeverTitle(c.lever), from,
			DisplayValue(c.lever, c.value), c.gain * 100.0));
	}

	for (const AutoTuneRejection& r : ReadRejected(si))
	{
		lines.push_back(fmt::format(TRANSLATE_FS("AutoTune", "Tried without benefit: {0} -> {1}"), LeverTitle(r.lever),
			DisplayValue(r.lever, fmt::format("{:g}", r.value))));
	}

	return lines;
}

bool AutoTune::SameValue(const std::string& a, const std::string& b)
{
	if (a == b)
		return true;
	const std::optional<double> x = ParseNumber(a);
	const std::optional<double> y = ParseNumber(b);
	return x && y && std::fabs(*x - *y) < 1e-6;
}

std::string AutoTune::LeverLabel(AutoTuneLever lever, double value)
{
	switch (lever)
	{
		case AutoTuneLever::Resolution:
			return fmt::format(TRANSLATE_FS("AutoTune", "resolution {}"), DisplayValue(lever, fmt::format("{:g}", value)));
		case AutoTuneLever::DownloadMode:
			return fmt::format(TRANSLATE_FS("AutoTune", "download mode {}"), DisplayValue(lever, fmt::format("{:g}", value)));
		case AutoTuneLever::Blending:
			return fmt::format(TRANSLATE_FS("AutoTune", "blending {}"), DisplayValue(lever, fmt::format("{:g}", value)));
		case AutoTuneLever::IopThread:
			return std::string(TRANSLATE_SV("AutoTune", "IOP thread"));
		case AutoTuneLever::EeGovernor:
			return std::string(TRANSLATE_SV("AutoTune", "EE governor"));
		case AutoTuneLever::GsBackThread:
			return std::string(TRANSLATE_SV("AutoTune", "GS back thread"));
		default:
			return "?";
	}
}
