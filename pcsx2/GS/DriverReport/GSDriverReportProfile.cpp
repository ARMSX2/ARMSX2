// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/DriverReport/GSDriverReportProfile.h"

namespace GSDriverReport
{
	static const char* ConfidenceName(DriverProfileConfidence c)
	{
		switch (c)
		{
			case DriverProfileConfidence::Vendor:
				return "vendor";
			case DriverProfileConfidence::Model:
				return "model";
			case DriverProfileConfidence::Driver:
				return "driver";
			case DriverProfileConfidence::DriverVersion:
				return "driver_version";
			case DriverProfileConfidence::Unknown:
			default:
				return "unknown";
		}
	}

	// The ids of the table rows set in `mask`, in table order.
	static void WriteRuleIds(JsonWriter& w, u64 mask)
	{
		w.BeginArray();
		for (u32 row = 0; row < GpuProfileDetector::DriverRuleCount(); row++)
		{
			if (mask & (u64{1} << row))
				w.String(GpuProfileDetector::DriverRuleId(row));
		}
		w.EndArray();
	}

	void WriteGpuProfile(JsonWriter& w, const GpuProfileSelection& selection, const VulkanDeviceRules* rules,
		std::string_view identified_driver)
	{
		w.BeginObject();
		w.KeyString("override", GpuProfileDetector::OverrideToConfigString(selection.override_mode));
		w.KeyString("runtime_profile", GpuProfileDetector::RuntimeProfileToString(selection.runtime_profile));
		w.KeyBool("is_mediatek_soc", selection.is_mediatek_soc);

		w.Key("gpu");
		w.BeginObject();
		w.KeyString("architecture", GpuProfileDetector::ArchitectureToString(selection.gpu.architecture));
		w.KeyUInt("model_number", selection.gpu.model_number);
		w.KeyUInt("core_count", selection.gpu.core_count);
		w.KeyBool("recognized", selection.gpu.recognized);
		w.KeyString("name", selection.gpu.name);
		w.EndObject();

		w.Key("gs_tuning");
		w.BeginObject();
		w.KeyBool("constrained", selection.gs_tuning.constrained);
		w.KeyBool("prefer_new_textures", selection.gs_tuning.prefer_new_textures);
		w.KeyUInt("pooled_targets", selection.gs_tuning.pooled_targets);
		w.KeyUInt("target_age", selection.gs_tuning.target_age);
		w.KeyUInt("pooled_textures", selection.gs_tuning.pooled_textures);
		w.KeyUInt("texture_age", selection.gs_tuning.texture_age);
		w.EndObject();

		const MobileDriverProfile& d = selection.driver;
		w.Key("driver");
		w.BeginObject();
		w.KeyString("api", GpuProfileDetector::ApiToString(d.api));
		w.KeyString("driver", GpuProfileDetector::DriverToString(d.driver));
		if (!identified_driver.empty())
			w.KeyString("identified_driver", identified_driver);
		w.KeyString("driver_name", d.driver_name);
		w.Key("version");
		w.BeginObject();
		w.KeyHex("raw", d.version.raw);
		w.KeyUInt("major", d.version.major);
		w.KeyUInt("minor", d.version.minor);
		w.KeyUInt("patch", d.version.patch);
		w.KeyUInt("build", d.version.build);
		w.KeyBool("known", d.version.known);
		w.KeyBool("legacy_hash", d.version.legacy_hash);
		w.EndObject();
		w.KeyUInt("database_version", MobileDriverProfile::DATABASE_VERSION);
		// The count is rows applied. exempt_rules is the part of matched_rules that was skipped.
		w.KeyUInt("matched_rule_count", d.matched_rule_count);
		w.Key("matched_rules");
		WriteRuleIds(w, d.matched_rules);
		w.Key("exempt_rules");
		WriteRuleIds(w, d.exempted_rules);
		w.KeyString("confidence", ConfidenceName(d.confidence));
		w.KeyBool("conservative_fallback", d.conservative_fallback);
		w.KeyHex("bugs_mask", d.bugs);
		w.Key("bugs");
		w.BeginArray();
		for (u8 i = 0; i < static_cast<u8>(DriverBug::Count); i++)
		{
			if (d.HasBug(static_cast<DriverBug>(i)))
				w.String(GpuProfileDetector::BugToString(static_cast<DriverBug>(i)));
		}
		w.EndArray();
		w.KeyHex("workarounds_mask", d.workarounds);
		w.Key("workarounds");
		w.BeginArray();
		for (u8 i = 0; i < static_cast<u8>(DriverWorkaround::Count); i++)
		{
			if (d.UsesWorkaround(static_cast<DriverWorkaround>(i)))
				w.String(GpuProfileDetector::WorkaroundToString(static_cast<DriverWorkaround>(i)));
		}
		w.EndArray();
		w.KeyHex("forced_bugs_mask", GpuProfileDetector::GetForcedBugs());
		w.KeyUInt("declared_loop_fix_generation", d.declared_loop_fix_generation);
		w.KeyBool("orders_declared_feedback_loop", d.orders_declared_feedback_loop);
		w.KeyBool("declared_loop_orders_overlap_on_request", d.declared_loop_orders_overlap_on_request);
		w.KeyBool("prefers_declared_loop_with_barriers", d.prefers_declared_loop_with_barriers);
		w.EndObject();

		if (rules)
		{
			w.Key("vulkan_device_rules");
			w.BeginObject();
			for (const VulkanDeviceRuleName& entry : VULKAN_DEVICE_RULE_NAMES)
				w.KeyBool(entry.name, rules->*entry.flag);
			w.EndObject();
		}

		w.KeyString("hints", selection.hints);
		w.EndObject();
	}

	GpuProfileSelection ResolveVulkanProfile(const MobileDriverContext& context, std::string_view device_name,
		VulkanDeviceRules* rules_out)
	{
		GpuProfileSelection selection = GpuProfileDetector::Resolve("auto", std::string_view(), device_name, context);
		if (rules_out)
			*rules_out = GpuProfileDetector::ResolveVulkanDeviceRules(selection, context, device_name);
		return selection;
	}
} // namespace GSDriverReport
