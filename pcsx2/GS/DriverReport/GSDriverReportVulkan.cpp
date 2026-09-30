// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/DriverReport/GSDriverReportVulkan.h"
#include "GS/DriverReport/GSDriverReportClassify.h"
#include "GS/DriverReport/GSDriverReportProfile.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace GSDriverReport
{
	static std::string CStr(const char* s, size_t cap)
	{
		return std::string(s, strnlen(s, cap));
	}

	static std::string HexBytes(const uint8_t* p, size_t n)
	{
		std::string out;
		out.reserve(n * 2);
		static constexpr char digits[] = "0123456789abcdef";
		for (size_t i = 0; i < n; i++)
		{
			out.push_back(digits[p[i] >> 4]);
			out.push_back(digits[p[i] & 15]);
		}
		return out;
	}

	static std::string ConformanceString(const VkConformanceVersion& v)
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", v.major, v.minor, v.subminor, v.patch);
		return buf;
	}

#include "GS/DriverReport/GSDriverReportVulkanStructs.inl"

	static std::string ResultString(VkResult r)
	{
		switch (r)
		{
#define RESULT_CASE(x) \
	case x: \
		return #x;
			RESULT_CASE(VK_SUCCESS)
			RESULT_CASE(VK_NOT_READY)
			RESULT_CASE(VK_TIMEOUT)
			RESULT_CASE(VK_INCOMPLETE)
			RESULT_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
			RESULT_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
			RESULT_CASE(VK_ERROR_INITIALIZATION_FAILED)
			RESULT_CASE(VK_ERROR_DEVICE_LOST)
			RESULT_CASE(VK_ERROR_LAYER_NOT_PRESENT)
			RESULT_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
			RESULT_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
			RESULT_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
#undef RESULT_CASE
			default:
			{
				char buf[32];
				std::snprintf(buf, sizeof(buf), "VkResult %d", static_cast<int>(r));
				return buf;
			}
		}
	}

	const std::vector<const char*>& GetArmsx2DeviceExtensionWishlist()
	{
		// Mirrors GSDeviceVK::SelectDeviceExtensions. The running renderer reports what it really
		// asked for; this copy is only for the command-line tool.
		static const std::vector<const char*> list = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
			VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
			VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
			VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
			VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME,
			VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME,
			VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME,
			VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME,
			VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME,
			VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
			VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
			VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
			VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
			VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME,
			VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME,
			VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
		};
		return list;
	}

	void WriteVulkanCoreFeatures(JsonWriter& w, const VkPhysicalDeviceFeatures& features)
	{
		WriteStruct(w, features);
	}

	uint32_t QueryLoaderInstanceVersion()
	{
		// The module-level entry points VKLoader resolved from the library it opened, which is the
		// loader or, through libadrenotools, the custom driver itself.
		uint32_t version = VK_API_VERSION_1_0;
		PFN_vkEnumerateInstanceVersion enumerate_version = vkEnumerateInstanceVersion;
		if (!enumerate_version && vkGetInstanceProcAddr)
			enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
				vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
		if (enumerate_version && enumerate_version(&version) != VK_SUCCESS)
			version = VK_API_VERSION_1_0;
		return version;
	}

	namespace
	{
		struct ChainLink
		{
			const char* key;
			VkBaseOutStructure* s;
			void (*write)(JsonWriter&, const VkBaseOutStructure*);
		};

		template <typename T>
		void Link(std::vector<ChainLink>& chain, const char* key, T& s, VkStructureType type)
		{
			std::memset(&s, 0, sizeof(s));
			s.sType = type;
			chain.push_back({key, reinterpret_cast<VkBaseOutStructure*>(&s),
				[](JsonWriter& w, const VkBaseOutStructure* p) { WriteStruct(w, *reinterpret_cast<const T*>(p)); }});
		}

		void Connect(VkBaseOutStructure* head, std::vector<ChainLink>& chain)
		{
			head->pNext = chain.empty() ? nullptr : chain.front().s;
			for (size_t i = 0; i < chain.size(); i++)
				chain[i].s->pNext = (i + 1 < chain.size()) ? chain[i + 1].s : nullptr;
		}

		struct FormatName
		{
			VkFormat format;
			const char* name;
		};

		// The formats the Vulkan backend creates, samples, renders to or presents from.
		constexpr FormatName s_formats[] = {
#define F(x) {x, #x}
			F(VK_FORMAT_R8G8B8A8_UNORM),
			F(VK_FORMAT_R8G8B8A8_SRGB),
			F(VK_FORMAT_R8G8B8A8_UINT),
			F(VK_FORMAT_B8G8R8A8_UNORM),
			F(VK_FORMAT_B8G8R8A8_SRGB),
			F(VK_FORMAT_A2B10G10R10_UNORM_PACK32),
			F(VK_FORMAT_R16G16B16A16_UNORM),
			F(VK_FORMAT_R16G16B16A16_SFLOAT),
			F(VK_FORMAT_R32G32B32A32_SFLOAT),
			F(VK_FORMAT_R32G32_SFLOAT),
			F(VK_FORMAT_R32_SFLOAT),
			F(VK_FORMAT_R32_UINT),
			F(VK_FORMAT_R16G16_UINT),
			F(VK_FORMAT_R16_UINT),
			F(VK_FORMAT_R8G8_UNORM),
			F(VK_FORMAT_R8_UNORM),
			F(VK_FORMAT_R8G8B8_UNORM),
			F(VK_FORMAT_B8G8R8_UNORM),
			F(VK_FORMAT_D32_SFLOAT),
			F(VK_FORMAT_D32_SFLOAT_S8_UINT),
			F(VK_FORMAT_D24_UNORM_S8_UINT),
			F(VK_FORMAT_BC1_RGBA_UNORM_BLOCK),
			F(VK_FORMAT_BC2_UNORM_BLOCK),
			F(VK_FORMAT_BC3_UNORM_BLOCK),
			F(VK_FORMAT_BC7_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_4x4_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_5x4_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_5x5_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_6x5_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_6x6_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_8x5_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_8x6_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_8x8_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_10x5_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_10x6_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_10x8_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_10x10_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_12x10_UNORM_BLOCK),
			F(VK_FORMAT_ASTC_12x12_UNORM_BLOCK),
#undef F
		};

		struct FlagName
		{
			uint32_t bit;
			const char* name;
		};

		constexpr FlagName s_format_feature_names[] = {
			{VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "sampled"},
			{VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "storage"},
			{VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "color_attachment"},
			{VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, "blend"},
			{VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "depth_stencil"},
			{VK_FORMAT_FEATURE_BLIT_SRC_BIT, "blit_src"},
			{VK_FORMAT_FEATURE_BLIT_DST_BIT, "blit_dst"},
			{VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, "filter_linear"},
			{VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, "transfer_src"},
			{VK_FORMAT_FEATURE_TRANSFER_DST_BIT, "transfer_dst"},
			{VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, "vertex_buffer"},
		};

		constexpr FlagName s_memory_property_names[] = {
			{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "device_local"},
			{VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "host_visible"},
			{VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "host_coherent"},
			{VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "host_cached"},
			{VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "lazily_allocated"},
			{VK_MEMORY_PROPERTY_PROTECTED_BIT, "protected"},
		};

		constexpr FlagName s_queue_flag_names[] = {
			{VK_QUEUE_GRAPHICS_BIT, "graphics"},
			{VK_QUEUE_COMPUTE_BIT, "compute"},
			{VK_QUEUE_TRANSFER_BIT, "transfer"},
			{VK_QUEUE_SPARSE_BINDING_BIT, "sparse_binding"},
			{VK_QUEUE_PROTECTED_BIT, "protected"},
		};

		template <size_t N>
		void WriteFlagNames(JsonWriter& w, std::string_view key, uint32_t flags, const FlagName (&names)[N])
		{
			w.Key(key);
			w.BeginArray();
			for (const FlagName& n : names)
			{
				if (flags & n.bit)
					w.String(n.name);
			}
			w.EndArray();
		}

		const char* DeviceTypeName(VkPhysicalDeviceType t)
		{
			switch (t)
			{
				case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
					return "integrated_gpu";
				case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
					return "discrete_gpu";
				case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
					return "virtual_gpu";
				case VK_PHYSICAL_DEVICE_TYPE_CPU:
					return "cpu";
				default:
					return "other";
			}
		}

		template <typename T>
		T Load(VkInstance instance, const char* name)
		{
			return reinterpret_cast<T>(vkGetInstanceProcAddr(instance, name));
		}
	} // namespace

	static void WriteExtensionList(JsonWriter& w, const std::vector<VkExtensionProperties>& list)
	{
		w.BeginArray();
		for (const VkExtensionProperties& e : list)
		{
			w.BeginObject();
			w.KeyString("name", CStr(e.extensionName, sizeof(e.extensionName)));
			w.KeyUInt("spec_version", e.specVersion);
			w.EndObject();
		}
		w.EndArray();
	}

	static void WriteStringList(JsonWriter& w, const std::vector<std::string>& list)
	{
		w.BeginArray();
		for (const std::string& s : list)
			w.String(s);
		w.EndArray();
	}

	static void WritePhysicalDevice(JsonWriter& w, StepLog& steps, const std::string& prefix, VkInstance instance,
		VkPhysicalDevice pd, bool selected, const VulkanLiveFacts* live, VulkanDeviceSummary* summary)
	{
		const auto get_props = Load<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
		auto get_props2 = Load<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
		if (!get_props2)
			get_props2 = Load<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2KHR");
		auto get_features2 = Load<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2");
		if (!get_features2)
			get_features2 = Load<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2KHR");
		const auto get_features = Load<PFN_vkGetPhysicalDeviceFeatures>(instance, "vkGetPhysicalDeviceFeatures");
		const auto enum_dev_ext =
			Load<PFN_vkEnumerateDeviceExtensionProperties>(instance, "vkEnumerateDeviceExtensionProperties");
		const auto get_qf = Load<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
			instance, "vkGetPhysicalDeviceQueueFamilyProperties");
		const auto get_mem = Load<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties");
		auto get_mem2 = Load<PFN_vkGetPhysicalDeviceMemoryProperties2>(instance, "vkGetPhysicalDeviceMemoryProperties2");
		if (!get_mem2)
			get_mem2 = Load<PFN_vkGetPhysicalDeviceMemoryProperties2>(instance, "vkGetPhysicalDeviceMemoryProperties2KHR");
		const auto get_fmt =
			Load<PFN_vkGetPhysicalDeviceFormatProperties>(instance, "vkGetPhysicalDeviceFormatProperties");

		w.BeginObject();
		// "The running renderer uses this device"; null where no renderer is running.
		if (live)
			w.KeyBool("selected", selected);
		else
			w.KeyNull("selected");

		// Device extensions first: they decide which structs may be chained below.
		std::vector<VkExtensionProperties> extensions;
		steps.Run(prefix + ".device_extensions", [&](std::string& err) {
			if (!enum_dev_ext)
			{
				err = "vkEnumerateDeviceExtensionProperties unavailable";
				return false;
			}
			uint32_t count = 0;
			VkResult r = enum_dev_ext(pd, nullptr, &count, nullptr);
			if (r != VK_SUCCESS)
			{
				err = ResultString(r);
				return false;
			}
			extensions.resize(count);
			r = enum_dev_ext(pd, nullptr, &count, extensions.data());
			extensions.resize(count);
			if (r != VK_SUCCESS && r != VK_INCOMPLETE)
			{
				err = ResultString(r);
				return false;
			}
			return true;
		});
		std::set<std::string> ext_names;
		for (const VkExtensionProperties& e : extensions)
			ext_names.insert(CStr(e.extensionName, sizeof(e.extensionName)));
		const auto has = [&ext_names](const char* name) { return ext_names.count(name) != 0; };

		VkPhysicalDeviceProperties props = {};
		steps.Run(prefix + ".properties", [&](std::string& err) {
			if (!get_props)
			{
				err = "vkGetPhysicalDeviceProperties unavailable";
				return false;
			}
			get_props(pd, &props);
			return true;
		});
		const uint32_t api = props.apiVersion;
		const bool api12 = api >= VK_API_VERSION_1_2;
		const bool api13 = api >= VK_API_VERSION_1_3;
		const bool api14 = api >= VK_MAKE_API_VERSION(0, 1, 4, 0);

		w.Key("properties");
		w.BeginObject();
		w.KeyString("deviceName", CStr(props.deviceName, sizeof(props.deviceName)));
		w.KeyHex("vendorID", props.vendorID);
		w.KeyHex("deviceID", props.deviceID);
		w.KeyString("deviceType", DeviceTypeName(props.deviceType));
		w.KeyString("apiVersion", FormatApiVersion(props.apiVersion));
		w.KeyHex("apiVersionRaw", props.apiVersion);
		w.Key("driverVersion");
		w.BeginObject();
		w.KeyUInt("raw", props.driverVersion);
		w.KeyHex("hex", props.driverVersion);
		// The decode needs driverID, which is only known once the chain below has run; the plain
		// Vulkan packing is written here and the vendor-specific one beside the driver properties.
		w.KeyString("vk_make_version", FormatApiVersion(props.driverVersion));
		w.EndObject();
		w.KeyString("pipelineCacheUUID", HexBytes(props.pipelineCacheUUID, sizeof(props.pipelineCacheUUID)));
		w.Key("limits");
		WriteStruct(w, props.limits);
		w.Key("sparseProperties");
		WriteStruct(w, props.sparseProperties);
		w.EndObject();

		// Properties2 chain.
		VkPhysicalDeviceVulkan11Properties p11;
		VkPhysicalDeviceVulkan12Properties p12;
		VkPhysicalDeviceVulkan13Properties p13;
		VkPhysicalDeviceDriverProperties pdriver;
		VkPhysicalDeviceSubgroupProperties psubgroup;
		VkPhysicalDevicePushDescriptorPropertiesKHR ppush;
		VkPhysicalDeviceProvokingVertexPropertiesEXT pprov;
		VkPhysicalDeviceLineRasterizationPropertiesEXT pline;
		VkPhysicalDeviceRobustness2PropertiesEXT prob2;
		VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT pgpl;
		std::vector<ChainLink> pchain;
		if (api12)
		{
			Link(pchain, "vulkan11", p11, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES);
			Link(pchain, "vulkan12", p12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES);
		}
		else
		{
			Link(pchain, "subgroup", psubgroup, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES);
			if (has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME))
				Link(pchain, "driver", pdriver, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
		}
		if (api13)
			Link(pchain, "vulkan13", p13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES);
		if (has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
			Link(pchain, "push_descriptor", ppush, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR);
		if (has(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME))
			Link(pchain, "provoking_vertex", pprov, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT);
		if (has(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME) || has("VK_KHR_line_rasterization"))
			Link(pchain, "line_rasterization", pline, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES_EXT);
		if (has(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME))
			Link(pchain, "robustness2", prob2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT);
		if (has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME))
			Link(pchain, "graphics_pipeline_library", pgpl,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT);

		const bool props2_ok = steps.Run(prefix + ".properties2", [&](std::string& err) {
			if (!get_props2)
			{
				err = "vkGetPhysicalDeviceProperties2 unavailable";
				return false;
			}
			VkPhysicalDeviceProperties2 p2 = {};
			p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			Connect(reinterpret_cast<VkBaseOutStructure*>(&p2), pchain);
			get_props2(pd, &p2);
			return true;
		});

		// The driver identity, from whichever struct carried it.
		uint32_t driver_id = 0;
		std::string driver_name, driver_info, conformance;
		bool has_driver_props = false;
		if (props2_ok)
		{
			if (api12)
			{
				driver_id = static_cast<uint32_t>(p12.driverID);
				driver_name = CStr(p12.driverName, sizeof(p12.driverName));
				driver_info = CStr(p12.driverInfo, sizeof(p12.driverInfo));
				conformance = ConformanceString(p12.conformanceVersion);
				has_driver_props = true;
			}
			else if (has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME))
			{
				driver_id = static_cast<uint32_t>(pdriver.driverID);
				driver_name = CStr(pdriver.driverName, sizeof(pdriver.driverName));
				driver_info = CStr(pdriver.driverInfo, sizeof(pdriver.driverInfo));
				conformance = ConformanceString(pdriver.conformanceVersion);
				has_driver_props = true;
			}
		}

		w.Key("driver");
		w.BeginObject();
		w.KeyBool("available", has_driver_props);
		w.KeyUInt("driverID", driver_id);
		w.KeyString("driverName", driver_name);
		w.KeyString("driverInfo", driver_info);
		w.KeyString("conformanceVersion", conformance);
		{
			std::string scheme;
			const std::string decoded = DecodeDriverVersion(props.vendorID, driver_id, props.driverVersion, &scheme);
			w.KeyString("driverVersionDecoded", decoded);
			w.KeyString("driverVersionScheme", scheme);
		}
		w.KeyUInt("axfl_generation", ParseAxflGeneration(driver_info));
		w.EndObject();

		w.Key("properties2");
		w.BeginObject();
		if (props2_ok)
		{
			for (const ChainLink& l : pchain)
			{
				w.Key(l.key);
				l.write(w, l.s);
			}
		}
		w.EndObject();

		// Features: 1.0 on its own, then the Features2 chain.
		steps.Run(prefix + ".features", [&](std::string& err) {
			if (!get_features)
			{
				err = "vkGetPhysicalDeviceFeatures unavailable";
				return false;
			}
			VkPhysicalDeviceFeatures f = {};
			get_features(pd, &f);
			w.Key("features");
			WriteStruct(w, f);
			return true;
		});

		VkPhysicalDeviceVulkan11Features f11;
		VkPhysicalDeviceVulkan12Features f12;
		VkPhysicalDeviceVulkan13Features f13;
		VkPhysicalDeviceVulkan14Features f14;
		VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT f_interlock;
		VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT f_roaa;
		VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT f_loop;
		VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT f_loop_dyn;
		VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR f_local_read;
		VkPhysicalDeviceProvokingVertexFeaturesEXT f_prov;
		VkPhysicalDeviceLineRasterizationFeaturesEXT f_line;
		VkPhysicalDeviceRobustness2FeaturesEXT f_rob2;
		VkPhysicalDeviceFaultFeaturesEXT f_fault;
		VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT f_swapchain_m1;
		VkPhysicalDeviceVulkanMemoryModelFeatures f_memmodel;
		VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT f_gpl;
		VkPhysicalDeviceExtendedDynamicStateFeaturesEXT f_eds;
		VkPhysicalDeviceExtendedDynamicState2FeaturesEXT f_eds2;
		VkPhysicalDeviceDynamicRenderingFeatures f_dynrender;
		VkPhysicalDeviceImagelessFramebufferFeatures f_imageless;
		VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT f_restart;
		VkPhysicalDeviceCustomBorderColorFeaturesEXT f_border;
		VkPhysicalDeviceMaintenance4Features f_m4;
		VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures f_demote;
		VkPhysicalDeviceSynchronization2Features f_sync2;
		VkPhysicalDeviceTimelineSemaphoreFeatures f_timeline;
		VkPhysicalDeviceDescriptorIndexingFeatures f_descidx;
		VkPhysicalDeviceIndexTypeUint8FeaturesEXT f_uint8;

		// A struct promoted into a VulkanNNFeatures struct must not be chained beside it, so each
		// extension struct is chained only below the version that absorbed it, and only where the
		// device has the extension.
		std::vector<ChainLink> fchain;
		if (api12)
		{
			Link(fchain, "vulkan11", f11, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
			Link(fchain, "vulkan12", f12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
		}
		if (api13)
			Link(fchain, "vulkan13", f13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
		if (api14)
			Link(fchain, "vulkan14", f14, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES);
		if (has(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME))
			Link(fchain, "fragment_shader_interlock", f_interlock,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT);
		if (has(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME) ||
			has(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME))
			Link(fchain, "rasterization_order_attachment_access", f_roaa,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT);
		if (has(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME))
			Link(fchain, "attachment_feedback_loop_layout", f_loop,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT);
		if (has(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME))
			Link(fchain, "attachment_feedback_loop_dynamic_state", f_loop_dyn,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT);
		if (!api14 && has(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME))
			Link(fchain, "dynamic_rendering_local_read", f_local_read,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR);
		if (has(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME))
			Link(fchain, "provoking_vertex", f_prov, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT);
		if (!api14 && (has(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME) || has("VK_KHR_line_rasterization")))
			Link(fchain, "line_rasterization", f_line, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT);
		if (has(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME))
			Link(fchain, "robustness2", f_rob2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT);
		if (has(VK_EXT_DEVICE_FAULT_EXTENSION_NAME))
			Link(fchain, "device_fault", f_fault, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT);
		if (has(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) || has(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
			Link(fchain, "swapchain_maintenance1", f_swapchain_m1,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT);
		if (!api12 && has(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME))
			Link(fchain, "vulkan_memory_model", f_memmodel, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES);
		if (has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME))
			Link(fchain, "graphics_pipeline_library", f_gpl,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT);
		if (has(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME))
			Link(fchain, "extended_dynamic_state", f_eds,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT);
		if (has(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME))
			Link(fchain, "extended_dynamic_state2", f_eds2,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT);
		if (!api13 && has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
			Link(fchain, "dynamic_rendering", f_dynrender, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
		if (!api12 && has(VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME))
			Link(fchain, "imageless_framebuffer", f_imageless,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES);
		if (has(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME))
			Link(fchain, "primitive_topology_list_restart", f_restart,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT);
		if (has(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME))
			Link(fchain, "custom_border_color", f_border,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT);
		if (!api13 && has(VK_KHR_MAINTENANCE_4_EXTENSION_NAME))
			Link(fchain, "maintenance4", f_m4, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES);
		if (!api13 && has(VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME))
			Link(fchain, "shader_demote_to_helper_invocation", f_demote,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES);
		if (!api13 && has(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME))
			Link(fchain, "synchronization2", f_sync2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES);
		if (!api12 && has(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
			Link(fchain, "timeline_semaphore", f_timeline, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
		if (!api12 && has(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
			Link(fchain, "descriptor_indexing", f_descidx, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
		if (!api14 && (has(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME) || has("VK_KHR_index_type_uint8")))
			Link(fchain, "index_type_uint8", f_uint8, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT);

		const bool features2_ok = steps.Run(prefix + ".features2", [&](std::string& err) {
			if (!get_features2)
			{
				err = "vkGetPhysicalDeviceFeatures2 unavailable";
				return false;
			}
			VkPhysicalDeviceFeatures2 f2 = {};
			f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			Connect(reinterpret_cast<VkBaseOutStructure*>(&f2), fchain);
			get_features2(pd, &f2);
			return true;
		});
		w.Key("features2");
		w.BeginObject();
		if (features2_ok)
		{
			for (const ChainLink& l : fchain)
			{
				w.Key(l.key);
				l.write(w, l.s);
			}
		}
		w.EndObject();

		w.Key("extensions");
		WriteExtensionList(w, extensions);

		// ARMSX2's view of this device's extensions.
		w.Key("armsx2_extensions");
		w.BeginObject();
		if (live)
		{
			w.KeyBool("from_running_renderer", true);
			w.Key("enabled");
			WriteStringList(w, live->enabled_device_extensions);
			w.Key("wanted_but_missing");
			WriteStringList(w, live->missing_device_extensions);
		}
		else
		{
			w.KeyBool("from_running_renderer", false);
			w.Key("present");
			w.BeginArray();
			for (const char* n : GetArmsx2DeviceExtensionWishlist())
			{
				if (has(n))
					w.String(n);
			}
			w.EndArray();
			w.Key("wanted_but_missing");
			w.BeginArray();
			for (const char* n : GetArmsx2DeviceExtensionWishlist())
			{
				if (!has(n))
					w.String(n);
			}
			w.EndArray();
		}
		w.EndObject();

		steps.Run(prefix + ".queue_families", [&](std::string& err) {
			if (!get_qf)
			{
				err = "vkGetPhysicalDeviceQueueFamilyProperties unavailable";
				return false;
			}
			uint32_t count = 0;
			get_qf(pd, &count, nullptr);
			std::vector<VkQueueFamilyProperties> qf(count);
			get_qf(pd, &count, qf.data());
			w.Key("queue_families");
			w.BeginArray();
			for (const VkQueueFamilyProperties& q : qf)
			{
				w.BeginObject();
				w.KeyHex("flags", q.queueFlags);
				WriteFlagNames(w, "flag_names", q.queueFlags, s_queue_flag_names);
				w.KeyUInt("queueCount", q.queueCount);
				w.KeyUInt("timestampValidBits", q.timestampValidBits);
				w.Key("minImageTransferGranularity");
				w.BeginArray();
				w.UInt(q.minImageTransferGranularity.width);
				w.UInt(q.minImageTransferGranularity.height);
				w.UInt(q.minImageTransferGranularity.depth);
				w.EndArray();
				w.EndObject();
			}
			w.EndArray();
			return true;
		});

		steps.Run(prefix + ".memory", [&](std::string& err) {
			VkPhysicalDeviceMemoryProperties mem = {};
			VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {};
			budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
			const bool want_budget = has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) && get_mem2;
			if (want_budget)
			{
				VkPhysicalDeviceMemoryProperties2 mem2 = {};
				mem2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
				mem2.pNext = &budget;
				get_mem2(pd, &mem2);
				mem = mem2.memoryProperties;
			}
			else if (get_mem)
			{
				get_mem(pd, &mem);
			}
			else
			{
				err = "vkGetPhysicalDeviceMemoryProperties unavailable";
				return false;
			}
			w.Key("memory");
			w.BeginObject();
			w.Key("heaps");
			w.BeginArray();
			for (uint32_t i = 0; i < mem.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
			{
				w.BeginObject();
				w.KeyUInt("index", i);
				w.KeyUInt("size", mem.memoryHeaps[i].size);
				w.KeyDouble("size_mib", static_cast<double>(mem.memoryHeaps[i].size) / (1024.0 * 1024.0));
				w.KeyHex("flags", mem.memoryHeaps[i].flags);
				w.KeyBool("device_local", (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0);
				if (want_budget)
				{
					w.KeyUInt("budget", budget.heapBudget[i]);
					w.KeyUInt("usage", budget.heapUsage[i]);
				}
				w.EndObject();
			}
			w.EndArray();
			w.Key("types");
			w.BeginArray();
			for (uint32_t i = 0; i < mem.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++)
			{
				w.BeginObject();
				w.KeyUInt("index", i);
				w.KeyUInt("heap", mem.memoryTypes[i].heapIndex);
				w.KeyHex("flags", mem.memoryTypes[i].propertyFlags);
				WriteFlagNames(w, "flag_names", mem.memoryTypes[i].propertyFlags, s_memory_property_names);
				w.EndObject();
			}
			w.EndArray();
			w.EndObject();
			return true;
		});

		steps.Run(prefix + ".formats", [&](std::string& err) {
			if (!get_fmt)
			{
				err = "vkGetPhysicalDeviceFormatProperties unavailable";
				return false;
			}
			w.Key("formats");
			w.BeginObject();
			for (const FormatName& f : s_formats)
			{
				VkFormatProperties fp = {};
				get_fmt(pd, f.format, &fp);
				w.Key(f.name + 10); // drop "VK_FORMAT_"
				w.BeginObject();
				w.KeyHex("optimal", fp.optimalTilingFeatures);
				WriteFlagNames(w, "optimal_names", fp.optimalTilingFeatures, s_format_feature_names);
				w.KeyHex("linear", fp.linearTilingFeatures);
				w.KeyHex("buffer", fp.bufferFeatures);
				w.EndObject();
			}
			w.EndObject();
			return true;
		});

		if (summary)
		{
			summary->handle = pd;
			summary->selected = selected;
			summary->vendor_id = props.vendorID;
			summary->device_id = props.deviceID;
			summary->driver_version = props.driverVersion;
			summary->api_version = props.apiVersion;
			summary->max_draw_indirect_count = props.limits.maxDrawIndirectCount;
			summary->driver_id = driver_id;
			summary->has_driver_properties = has_driver_props;
			summary->device_name = CStr(props.deviceName, sizeof(props.deviceName));
			summary->driver_name = driver_name;
			summary->driver_info = driver_info;
		}

		// With no running renderer, say what the renderer's profile resolver would decide.
		if (!live)
		{
			steps.Run(prefix + ".profile", [&](std::string&) {
				MobileDriverContext ctx;
				ctx.api = MobileGpuApi::Vulkan;
				ctx.vendor_id = props.vendorID;
				ctx.device_id = props.deviceID;
				ctx.driver_version = props.driverVersion;
				ctx.api_version = props.apiVersion;
				ctx.max_draw_indirect_count = props.limits.maxDrawIndirectCount;
				if (has_driver_props)
				{
					ctx.driver_id = driver_id;
					ctx.driver_name = driver_name;
					ctx.driver_info = driver_info;
				}
				VulkanDeviceRules rules;
				const std::string name = CStr(props.deviceName, sizeof(props.deviceName));
				const GpuProfileSelection sel = ResolveVulkanProfile(ctx, name, &rules);
				ServedDriverFacts facts;
				facts.vendor_id = props.vendorID;
				facts.driver_id = driver_id;
				facts.driver_name = driver_name;
				facts.driver_info = driver_info;
				facts.device_name = name;
				w.Key("armsx2_profile");
				WriteGpuProfile(w, sel, &rules, ClassifyServedDriver(facts).answered);
				return true;
			});
		}

		w.EndObject();
	}

	void WriteVulkanInstance(JsonWriter& w, StepLog& steps, std::string_view step_prefix, VkInstance instance,
		VkPhysicalDevice selected, const VulkanLiveFacts* live, std::vector<VulkanDeviceSummary>* summaries)
	{
		const std::string prefix(step_prefix);
		w.BeginObject();

		const uint32_t loader_version = QueryLoaderInstanceVersion();
		w.KeyString("loader_instance_version", FormatApiVersion(loader_version));
		if (live)
			w.KeyString("instance_api_version_requested", FormatApiVersion(live->instance_api_version));

		steps.Run(prefix + ".instance_extensions", [&](std::string& err) {
			const PFN_vkEnumerateInstanceExtensionProperties fn = vkEnumerateInstanceExtensionProperties;
			if (!fn)
			{
				err = "vkEnumerateInstanceExtensionProperties unavailable";
				return false;
			}
			uint32_t count = 0;
			VkResult r = fn(nullptr, &count, nullptr);
			std::vector<VkExtensionProperties> list(count);
			if (r == VK_SUCCESS)
				r = fn(nullptr, &count, list.data());
			list.resize(count);
			w.Key("instance_extensions");
			WriteExtensionList(w, list);
			if (r != VK_SUCCESS && r != VK_INCOMPLETE)
			{
				err = ResultString(r);
				return false;
			}
			return true;
		});

		steps.Run(prefix + ".instance_layers", [&](std::string& err) {
			const PFN_vkEnumerateInstanceLayerProperties fn = vkEnumerateInstanceLayerProperties;
			if (!fn)
			{
				err = "vkEnumerateInstanceLayerProperties unavailable";
				return false;
			}
			uint32_t count = 0;
			VkResult r = fn(&count, nullptr);
			std::vector<VkLayerProperties> list(count);
			if (r == VK_SUCCESS)
				r = fn(&count, list.data());
			list.resize(count);
			w.Key("instance_layers");
			w.BeginArray();
			for (const VkLayerProperties& l : list)
			{
				w.BeginObject();
				w.KeyString("name", CStr(l.layerName, sizeof(l.layerName)));
				w.KeyString("spec_version", FormatApiVersion(l.specVersion));
				w.KeyUInt("implementation_version", l.implementationVersion);
				w.KeyString("description", CStr(l.description, sizeof(l.description)));
				w.EndObject();
			}
			w.EndArray();
			if (r != VK_SUCCESS && r != VK_INCOMPLETE)
			{
				err = ResultString(r);
				return false;
			}
			return true;
		});

		if (live)
		{
			w.Key("enabled_instance_extensions");
			WriteStringList(w, live->enabled_instance_extensions);
		}

		std::vector<VkPhysicalDevice> devices;
		steps.Run(prefix + ".enumerate_physical_devices", [&](std::string& err) {
			const auto fn = Load<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
			if (!fn)
			{
				err = "vkEnumeratePhysicalDevices unavailable";
				return false;
			}
			uint32_t count = 0;
			VkResult r = fn(instance, &count, nullptr);
			if (r != VK_SUCCESS)
			{
				err = ResultString(r);
				return false;
			}
			devices.resize(count);
			r = fn(instance, &count, devices.data());
			devices.resize(count);
			if (r != VK_SUCCESS && r != VK_INCOMPLETE)
			{
				err = ResultString(r);
				return false;
			}
			if (count == 0)
			{
				err = "no physical devices";
				return false;
			}
			return true;
		});
		w.KeyUInt("physical_device_count", devices.size());

		w.Key("physical_devices");
		w.BeginArray();
		for (size_t i = 0; i < devices.size(); i++)
		{
			VulkanDeviceSummary summary;
			WritePhysicalDevice(w, steps, prefix + ".device" + std::to_string(i), instance, devices[i],
				devices[i] == selected, live, &summary);
			if (summaries)
				summaries->push_back(std::move(summary));
		}
		w.EndArray();

		w.EndObject();
	}
} // namespace GSDriverReport
