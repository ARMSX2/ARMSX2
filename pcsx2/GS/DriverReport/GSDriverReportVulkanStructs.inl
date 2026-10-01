// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GENERATED from vulkan_core.h (VK_HEADER_VERSION 345) by a script that reads each struct's
// field list. One JSON writer per struct, every field, named as in the spec. Included only by
// GSDriverReportVulkan.cpp, which provides CStr, HexBytes and ConformanceString.

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceLimits& s)
{
	w.BeginObject();
	w.KeyUInt("maxImageDimension1D", static_cast<uint64_t>(s.maxImageDimension1D));
	w.KeyUInt("maxImageDimension2D", static_cast<uint64_t>(s.maxImageDimension2D));
	w.KeyUInt("maxImageDimension3D", static_cast<uint64_t>(s.maxImageDimension3D));
	w.KeyUInt("maxImageDimensionCube", static_cast<uint64_t>(s.maxImageDimensionCube));
	w.KeyUInt("maxImageArrayLayers", static_cast<uint64_t>(s.maxImageArrayLayers));
	w.KeyUInt("maxTexelBufferElements", static_cast<uint64_t>(s.maxTexelBufferElements));
	w.KeyUInt("maxUniformBufferRange", static_cast<uint64_t>(s.maxUniformBufferRange));
	w.KeyUInt("maxStorageBufferRange", static_cast<uint64_t>(s.maxStorageBufferRange));
	w.KeyUInt("maxPushConstantsSize", static_cast<uint64_t>(s.maxPushConstantsSize));
	w.KeyUInt("maxMemoryAllocationCount", static_cast<uint64_t>(s.maxMemoryAllocationCount));
	w.KeyUInt("maxSamplerAllocationCount", static_cast<uint64_t>(s.maxSamplerAllocationCount));
	w.KeyUInt("bufferImageGranularity", static_cast<uint64_t>(s.bufferImageGranularity));
	w.KeyUInt("sparseAddressSpaceSize", static_cast<uint64_t>(s.sparseAddressSpaceSize));
	w.KeyUInt("maxBoundDescriptorSets", static_cast<uint64_t>(s.maxBoundDescriptorSets));
	w.KeyUInt("maxPerStageDescriptorSamplers", static_cast<uint64_t>(s.maxPerStageDescriptorSamplers));
	w.KeyUInt("maxPerStageDescriptorUniformBuffers", static_cast<uint64_t>(s.maxPerStageDescriptorUniformBuffers));
	w.KeyUInt("maxPerStageDescriptorStorageBuffers", static_cast<uint64_t>(s.maxPerStageDescriptorStorageBuffers));
	w.KeyUInt("maxPerStageDescriptorSampledImages", static_cast<uint64_t>(s.maxPerStageDescriptorSampledImages));
	w.KeyUInt("maxPerStageDescriptorStorageImages", static_cast<uint64_t>(s.maxPerStageDescriptorStorageImages));
	w.KeyUInt("maxPerStageDescriptorInputAttachments", static_cast<uint64_t>(s.maxPerStageDescriptorInputAttachments));
	w.KeyUInt("maxPerStageResources", static_cast<uint64_t>(s.maxPerStageResources));
	w.KeyUInt("maxDescriptorSetSamplers", static_cast<uint64_t>(s.maxDescriptorSetSamplers));
	w.KeyUInt("maxDescriptorSetUniformBuffers", static_cast<uint64_t>(s.maxDescriptorSetUniformBuffers));
	w.KeyUInt("maxDescriptorSetUniformBuffersDynamic", static_cast<uint64_t>(s.maxDescriptorSetUniformBuffersDynamic));
	w.KeyUInt("maxDescriptorSetStorageBuffers", static_cast<uint64_t>(s.maxDescriptorSetStorageBuffers));
	w.KeyUInt("maxDescriptorSetStorageBuffersDynamic", static_cast<uint64_t>(s.maxDescriptorSetStorageBuffersDynamic));
	w.KeyUInt("maxDescriptorSetSampledImages", static_cast<uint64_t>(s.maxDescriptorSetSampledImages));
	w.KeyUInt("maxDescriptorSetStorageImages", static_cast<uint64_t>(s.maxDescriptorSetStorageImages));
	w.KeyUInt("maxDescriptorSetInputAttachments", static_cast<uint64_t>(s.maxDescriptorSetInputAttachments));
	w.KeyUInt("maxVertexInputAttributes", static_cast<uint64_t>(s.maxVertexInputAttributes));
	w.KeyUInt("maxVertexInputBindings", static_cast<uint64_t>(s.maxVertexInputBindings));
	w.KeyUInt("maxVertexInputAttributeOffset", static_cast<uint64_t>(s.maxVertexInputAttributeOffset));
	w.KeyUInt("maxVertexInputBindingStride", static_cast<uint64_t>(s.maxVertexInputBindingStride));
	w.KeyUInt("maxVertexOutputComponents", static_cast<uint64_t>(s.maxVertexOutputComponents));
	w.KeyUInt("maxTessellationGenerationLevel", static_cast<uint64_t>(s.maxTessellationGenerationLevel));
	w.KeyUInt("maxTessellationPatchSize", static_cast<uint64_t>(s.maxTessellationPatchSize));
	w.KeyUInt("maxTessellationControlPerVertexInputComponents", static_cast<uint64_t>(s.maxTessellationControlPerVertexInputComponents));
	w.KeyUInt("maxTessellationControlPerVertexOutputComponents", static_cast<uint64_t>(s.maxTessellationControlPerVertexOutputComponents));
	w.KeyUInt("maxTessellationControlPerPatchOutputComponents", static_cast<uint64_t>(s.maxTessellationControlPerPatchOutputComponents));
	w.KeyUInt("maxTessellationControlTotalOutputComponents", static_cast<uint64_t>(s.maxTessellationControlTotalOutputComponents));
	w.KeyUInt("maxTessellationEvaluationInputComponents", static_cast<uint64_t>(s.maxTessellationEvaluationInputComponents));
	w.KeyUInt("maxTessellationEvaluationOutputComponents", static_cast<uint64_t>(s.maxTessellationEvaluationOutputComponents));
	w.KeyUInt("maxGeometryShaderInvocations", static_cast<uint64_t>(s.maxGeometryShaderInvocations));
	w.KeyUInt("maxGeometryInputComponents", static_cast<uint64_t>(s.maxGeometryInputComponents));
	w.KeyUInt("maxGeometryOutputComponents", static_cast<uint64_t>(s.maxGeometryOutputComponents));
	w.KeyUInt("maxGeometryOutputVertices", static_cast<uint64_t>(s.maxGeometryOutputVertices));
	w.KeyUInt("maxGeometryTotalOutputComponents", static_cast<uint64_t>(s.maxGeometryTotalOutputComponents));
	w.KeyUInt("maxFragmentInputComponents", static_cast<uint64_t>(s.maxFragmentInputComponents));
	w.KeyUInt("maxFragmentOutputAttachments", static_cast<uint64_t>(s.maxFragmentOutputAttachments));
	w.KeyUInt("maxFragmentDualSrcAttachments", static_cast<uint64_t>(s.maxFragmentDualSrcAttachments));
	w.KeyUInt("maxFragmentCombinedOutputResources", static_cast<uint64_t>(s.maxFragmentCombinedOutputResources));
	w.KeyUInt("maxComputeSharedMemorySize", static_cast<uint64_t>(s.maxComputeSharedMemorySize));
	w.Key("maxComputeWorkGroupCount");
	w.BeginArray();
	for (const auto& v : s.maxComputeWorkGroupCount)
		w.UInt(static_cast<uint64_t>(v));
	w.EndArray();
	w.KeyUInt("maxComputeWorkGroupInvocations", static_cast<uint64_t>(s.maxComputeWorkGroupInvocations));
	w.Key("maxComputeWorkGroupSize");
	w.BeginArray();
	for (const auto& v : s.maxComputeWorkGroupSize)
		w.UInt(static_cast<uint64_t>(v));
	w.EndArray();
	w.KeyUInt("subPixelPrecisionBits", static_cast<uint64_t>(s.subPixelPrecisionBits));
	w.KeyUInt("subTexelPrecisionBits", static_cast<uint64_t>(s.subTexelPrecisionBits));
	w.KeyUInt("mipmapPrecisionBits", static_cast<uint64_t>(s.mipmapPrecisionBits));
	w.KeyUInt("maxDrawIndexedIndexValue", static_cast<uint64_t>(s.maxDrawIndexedIndexValue));
	w.KeyUInt("maxDrawIndirectCount", static_cast<uint64_t>(s.maxDrawIndirectCount));
	w.KeyDouble("maxSamplerLodBias", s.maxSamplerLodBias);
	w.KeyDouble("maxSamplerAnisotropy", s.maxSamplerAnisotropy);
	w.KeyUInt("maxViewports", static_cast<uint64_t>(s.maxViewports));
	w.Key("maxViewportDimensions");
	w.BeginArray();
	for (const auto& v : s.maxViewportDimensions)
		w.UInt(static_cast<uint64_t>(v));
	w.EndArray();
	w.Key("viewportBoundsRange");
	w.BeginArray();
	for (const auto& v : s.viewportBoundsRange)
		w.Double(v);
	w.EndArray();
	w.KeyUInt("viewportSubPixelBits", static_cast<uint64_t>(s.viewportSubPixelBits));
	w.KeyUInt("minMemoryMapAlignment", static_cast<uint64_t>(s.minMemoryMapAlignment));
	w.KeyUInt("minTexelBufferOffsetAlignment", static_cast<uint64_t>(s.minTexelBufferOffsetAlignment));
	w.KeyUInt("minUniformBufferOffsetAlignment", static_cast<uint64_t>(s.minUniformBufferOffsetAlignment));
	w.KeyUInt("minStorageBufferOffsetAlignment", static_cast<uint64_t>(s.minStorageBufferOffsetAlignment));
	w.KeyInt("minTexelOffset", s.minTexelOffset);
	w.KeyUInt("maxTexelOffset", static_cast<uint64_t>(s.maxTexelOffset));
	w.KeyInt("minTexelGatherOffset", s.minTexelGatherOffset);
	w.KeyUInt("maxTexelGatherOffset", static_cast<uint64_t>(s.maxTexelGatherOffset));
	w.KeyDouble("minInterpolationOffset", s.minInterpolationOffset);
	w.KeyDouble("maxInterpolationOffset", s.maxInterpolationOffset);
	w.KeyUInt("subPixelInterpolationOffsetBits", static_cast<uint64_t>(s.subPixelInterpolationOffsetBits));
	w.KeyUInt("maxFramebufferWidth", static_cast<uint64_t>(s.maxFramebufferWidth));
	w.KeyUInt("maxFramebufferHeight", static_cast<uint64_t>(s.maxFramebufferHeight));
	w.KeyUInt("maxFramebufferLayers", static_cast<uint64_t>(s.maxFramebufferLayers));
	w.KeyHex("framebufferColorSampleCounts", static_cast<uint64_t>(s.framebufferColorSampleCounts));
	w.KeyHex("framebufferDepthSampleCounts", static_cast<uint64_t>(s.framebufferDepthSampleCounts));
	w.KeyHex("framebufferStencilSampleCounts", static_cast<uint64_t>(s.framebufferStencilSampleCounts));
	w.KeyHex("framebufferNoAttachmentsSampleCounts", static_cast<uint64_t>(s.framebufferNoAttachmentsSampleCounts));
	w.KeyUInt("maxColorAttachments", static_cast<uint64_t>(s.maxColorAttachments));
	w.KeyHex("sampledImageColorSampleCounts", static_cast<uint64_t>(s.sampledImageColorSampleCounts));
	w.KeyHex("sampledImageIntegerSampleCounts", static_cast<uint64_t>(s.sampledImageIntegerSampleCounts));
	w.KeyHex("sampledImageDepthSampleCounts", static_cast<uint64_t>(s.sampledImageDepthSampleCounts));
	w.KeyHex("sampledImageStencilSampleCounts", static_cast<uint64_t>(s.sampledImageStencilSampleCounts));
	w.KeyHex("storageImageSampleCounts", static_cast<uint64_t>(s.storageImageSampleCounts));
	w.KeyUInt("maxSampleMaskWords", static_cast<uint64_t>(s.maxSampleMaskWords));
	w.KeyBool("timestampComputeAndGraphics", s.timestampComputeAndGraphics != VK_FALSE);
	w.KeyDouble("timestampPeriod", s.timestampPeriod);
	w.KeyUInt("maxClipDistances", static_cast<uint64_t>(s.maxClipDistances));
	w.KeyUInt("maxCullDistances", static_cast<uint64_t>(s.maxCullDistances));
	w.KeyUInt("maxCombinedClipAndCullDistances", static_cast<uint64_t>(s.maxCombinedClipAndCullDistances));
	w.KeyUInt("discreteQueuePriorities", static_cast<uint64_t>(s.discreteQueuePriorities));
	w.Key("pointSizeRange");
	w.BeginArray();
	for (const auto& v : s.pointSizeRange)
		w.Double(v);
	w.EndArray();
	w.Key("lineWidthRange");
	w.BeginArray();
	for (const auto& v : s.lineWidthRange)
		w.Double(v);
	w.EndArray();
	w.KeyDouble("pointSizeGranularity", s.pointSizeGranularity);
	w.KeyDouble("lineWidthGranularity", s.lineWidthGranularity);
	w.KeyBool("strictLines", s.strictLines != VK_FALSE);
	w.KeyBool("standardSampleLocations", s.standardSampleLocations != VK_FALSE);
	w.KeyUInt("optimalBufferCopyOffsetAlignment", static_cast<uint64_t>(s.optimalBufferCopyOffsetAlignment));
	w.KeyUInt("optimalBufferCopyRowPitchAlignment", static_cast<uint64_t>(s.optimalBufferCopyRowPitchAlignment));
	w.KeyUInt("nonCoherentAtomSize", static_cast<uint64_t>(s.nonCoherentAtomSize));
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceSparseProperties& s)
{
	w.BeginObject();
	w.KeyBool("residencyStandard2DBlockShape", s.residencyStandard2DBlockShape != VK_FALSE);
	w.KeyBool("residencyStandard2DMultisampleBlockShape", s.residencyStandard2DMultisampleBlockShape != VK_FALSE);
	w.KeyBool("residencyStandard3DBlockShape", s.residencyStandard3DBlockShape != VK_FALSE);
	w.KeyBool("residencyAlignedMipSize", s.residencyAlignedMipSize != VK_FALSE);
	w.KeyBool("residencyNonResidentStrict", s.residencyNonResidentStrict != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceFeatures& s)
{
	w.BeginObject();
	w.KeyBool("robustBufferAccess", s.robustBufferAccess != VK_FALSE);
	w.KeyBool("fullDrawIndexUint32", s.fullDrawIndexUint32 != VK_FALSE);
	w.KeyBool("imageCubeArray", s.imageCubeArray != VK_FALSE);
	w.KeyBool("independentBlend", s.independentBlend != VK_FALSE);
	w.KeyBool("geometryShader", s.geometryShader != VK_FALSE);
	w.KeyBool("tessellationShader", s.tessellationShader != VK_FALSE);
	w.KeyBool("sampleRateShading", s.sampleRateShading != VK_FALSE);
	w.KeyBool("dualSrcBlend", s.dualSrcBlend != VK_FALSE);
	w.KeyBool("logicOp", s.logicOp != VK_FALSE);
	w.KeyBool("multiDrawIndirect", s.multiDrawIndirect != VK_FALSE);
	w.KeyBool("drawIndirectFirstInstance", s.drawIndirectFirstInstance != VK_FALSE);
	w.KeyBool("depthClamp", s.depthClamp != VK_FALSE);
	w.KeyBool("depthBiasClamp", s.depthBiasClamp != VK_FALSE);
	w.KeyBool("fillModeNonSolid", s.fillModeNonSolid != VK_FALSE);
	w.KeyBool("depthBounds", s.depthBounds != VK_FALSE);
	w.KeyBool("wideLines", s.wideLines != VK_FALSE);
	w.KeyBool("largePoints", s.largePoints != VK_FALSE);
	w.KeyBool("alphaToOne", s.alphaToOne != VK_FALSE);
	w.KeyBool("multiViewport", s.multiViewport != VK_FALSE);
	w.KeyBool("samplerAnisotropy", s.samplerAnisotropy != VK_FALSE);
	w.KeyBool("textureCompressionETC2", s.textureCompressionETC2 != VK_FALSE);
	w.KeyBool("textureCompressionASTC_LDR", s.textureCompressionASTC_LDR != VK_FALSE);
	w.KeyBool("textureCompressionBC", s.textureCompressionBC != VK_FALSE);
	w.KeyBool("occlusionQueryPrecise", s.occlusionQueryPrecise != VK_FALSE);
	w.KeyBool("pipelineStatisticsQuery", s.pipelineStatisticsQuery != VK_FALSE);
	w.KeyBool("vertexPipelineStoresAndAtomics", s.vertexPipelineStoresAndAtomics != VK_FALSE);
	w.KeyBool("fragmentStoresAndAtomics", s.fragmentStoresAndAtomics != VK_FALSE);
	w.KeyBool("shaderTessellationAndGeometryPointSize", s.shaderTessellationAndGeometryPointSize != VK_FALSE);
	w.KeyBool("shaderImageGatherExtended", s.shaderImageGatherExtended != VK_FALSE);
	w.KeyBool("shaderStorageImageExtendedFormats", s.shaderStorageImageExtendedFormats != VK_FALSE);
	w.KeyBool("shaderStorageImageMultisample", s.shaderStorageImageMultisample != VK_FALSE);
	w.KeyBool("shaderStorageImageReadWithoutFormat", s.shaderStorageImageReadWithoutFormat != VK_FALSE);
	w.KeyBool("shaderStorageImageWriteWithoutFormat", s.shaderStorageImageWriteWithoutFormat != VK_FALSE);
	w.KeyBool("shaderUniformBufferArrayDynamicIndexing", s.shaderUniformBufferArrayDynamicIndexing != VK_FALSE);
	w.KeyBool("shaderSampledImageArrayDynamicIndexing", s.shaderSampledImageArrayDynamicIndexing != VK_FALSE);
	w.KeyBool("shaderStorageBufferArrayDynamicIndexing", s.shaderStorageBufferArrayDynamicIndexing != VK_FALSE);
	w.KeyBool("shaderStorageImageArrayDynamicIndexing", s.shaderStorageImageArrayDynamicIndexing != VK_FALSE);
	w.KeyBool("shaderClipDistance", s.shaderClipDistance != VK_FALSE);
	w.KeyBool("shaderCullDistance", s.shaderCullDistance != VK_FALSE);
	w.KeyBool("shaderFloat64", s.shaderFloat64 != VK_FALSE);
	w.KeyBool("shaderInt64", s.shaderInt64 != VK_FALSE);
	w.KeyBool("shaderInt16", s.shaderInt16 != VK_FALSE);
	w.KeyBool("shaderResourceResidency", s.shaderResourceResidency != VK_FALSE);
	w.KeyBool("shaderResourceMinLod", s.shaderResourceMinLod != VK_FALSE);
	w.KeyBool("sparseBinding", s.sparseBinding != VK_FALSE);
	w.KeyBool("sparseResidencyBuffer", s.sparseResidencyBuffer != VK_FALSE);
	w.KeyBool("sparseResidencyImage2D", s.sparseResidencyImage2D != VK_FALSE);
	w.KeyBool("sparseResidencyImage3D", s.sparseResidencyImage3D != VK_FALSE);
	w.KeyBool("sparseResidency2Samples", s.sparseResidency2Samples != VK_FALSE);
	w.KeyBool("sparseResidency4Samples", s.sparseResidency4Samples != VK_FALSE);
	w.KeyBool("sparseResidency8Samples", s.sparseResidency8Samples != VK_FALSE);
	w.KeyBool("sparseResidency16Samples", s.sparseResidency16Samples != VK_FALSE);
	w.KeyBool("sparseResidencyAliased", s.sparseResidencyAliased != VK_FALSE);
	w.KeyBool("variableMultisampleRate", s.variableMultisampleRate != VK_FALSE);
	w.KeyBool("inheritedQueries", s.inheritedQueries != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceVulkan11Features& s)
{
	w.BeginObject();
	w.KeyBool("storageBuffer16BitAccess", s.storageBuffer16BitAccess != VK_FALSE);
	w.KeyBool("uniformAndStorageBuffer16BitAccess", s.uniformAndStorageBuffer16BitAccess != VK_FALSE);
	w.KeyBool("storagePushConstant16", s.storagePushConstant16 != VK_FALSE);
	w.KeyBool("storageInputOutput16", s.storageInputOutput16 != VK_FALSE);
	w.KeyBool("multiview", s.multiview != VK_FALSE);
	w.KeyBool("multiviewGeometryShader", s.multiviewGeometryShader != VK_FALSE);
	w.KeyBool("multiviewTessellationShader", s.multiviewTessellationShader != VK_FALSE);
	w.KeyBool("variablePointersStorageBuffer", s.variablePointersStorageBuffer != VK_FALSE);
	w.KeyBool("variablePointers", s.variablePointers != VK_FALSE);
	w.KeyBool("protectedMemory", s.protectedMemory != VK_FALSE);
	w.KeyBool("samplerYcbcrConversion", s.samplerYcbcrConversion != VK_FALSE);
	w.KeyBool("shaderDrawParameters", s.shaderDrawParameters != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceVulkan12Features& s)
{
	w.BeginObject();
	w.KeyBool("samplerMirrorClampToEdge", s.samplerMirrorClampToEdge != VK_FALSE);
	w.KeyBool("drawIndirectCount", s.drawIndirectCount != VK_FALSE);
	w.KeyBool("storageBuffer8BitAccess", s.storageBuffer8BitAccess != VK_FALSE);
	w.KeyBool("uniformAndStorageBuffer8BitAccess", s.uniformAndStorageBuffer8BitAccess != VK_FALSE);
	w.KeyBool("storagePushConstant8", s.storagePushConstant8 != VK_FALSE);
	w.KeyBool("shaderBufferInt64Atomics", s.shaderBufferInt64Atomics != VK_FALSE);
	w.KeyBool("shaderSharedInt64Atomics", s.shaderSharedInt64Atomics != VK_FALSE);
	w.KeyBool("shaderFloat16", s.shaderFloat16 != VK_FALSE);
	w.KeyBool("shaderInt8", s.shaderInt8 != VK_FALSE);
	w.KeyBool("descriptorIndexing", s.descriptorIndexing != VK_FALSE);
	w.KeyBool("shaderInputAttachmentArrayDynamicIndexing", s.shaderInputAttachmentArrayDynamicIndexing != VK_FALSE);
	w.KeyBool("shaderUniformTexelBufferArrayDynamicIndexing", s.shaderUniformTexelBufferArrayDynamicIndexing != VK_FALSE);
	w.KeyBool("shaderStorageTexelBufferArrayDynamicIndexing", s.shaderStorageTexelBufferArrayDynamicIndexing != VK_FALSE);
	w.KeyBool("shaderUniformBufferArrayNonUniformIndexing", s.shaderUniformBufferArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderSampledImageArrayNonUniformIndexing", s.shaderSampledImageArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderStorageBufferArrayNonUniformIndexing", s.shaderStorageBufferArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderStorageImageArrayNonUniformIndexing", s.shaderStorageImageArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderInputAttachmentArrayNonUniformIndexing", s.shaderInputAttachmentArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderUniformTexelBufferArrayNonUniformIndexing", s.shaderUniformTexelBufferArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderStorageTexelBufferArrayNonUniformIndexing", s.shaderStorageTexelBufferArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("descriptorBindingUniformBufferUpdateAfterBind", s.descriptorBindingUniformBufferUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingSampledImageUpdateAfterBind", s.descriptorBindingSampledImageUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingStorageImageUpdateAfterBind", s.descriptorBindingStorageImageUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingStorageBufferUpdateAfterBind", s.descriptorBindingStorageBufferUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingUniformTexelBufferUpdateAfterBind", s.descriptorBindingUniformTexelBufferUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingStorageTexelBufferUpdateAfterBind", s.descriptorBindingStorageTexelBufferUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingUpdateUnusedWhilePending", s.descriptorBindingUpdateUnusedWhilePending != VK_FALSE);
	w.KeyBool("descriptorBindingPartiallyBound", s.descriptorBindingPartiallyBound != VK_FALSE);
	w.KeyBool("descriptorBindingVariableDescriptorCount", s.descriptorBindingVariableDescriptorCount != VK_FALSE);
	w.KeyBool("runtimeDescriptorArray", s.runtimeDescriptorArray != VK_FALSE);
	w.KeyBool("samplerFilterMinmax", s.samplerFilterMinmax != VK_FALSE);
	w.KeyBool("scalarBlockLayout", s.scalarBlockLayout != VK_FALSE);
	w.KeyBool("imagelessFramebuffer", s.imagelessFramebuffer != VK_FALSE);
	w.KeyBool("uniformBufferStandardLayout", s.uniformBufferStandardLayout != VK_FALSE);
	w.KeyBool("shaderSubgroupExtendedTypes", s.shaderSubgroupExtendedTypes != VK_FALSE);
	w.KeyBool("separateDepthStencilLayouts", s.separateDepthStencilLayouts != VK_FALSE);
	w.KeyBool("hostQueryReset", s.hostQueryReset != VK_FALSE);
	w.KeyBool("timelineSemaphore", s.timelineSemaphore != VK_FALSE);
	w.KeyBool("bufferDeviceAddress", s.bufferDeviceAddress != VK_FALSE);
	w.KeyBool("bufferDeviceAddressCaptureReplay", s.bufferDeviceAddressCaptureReplay != VK_FALSE);
	w.KeyBool("bufferDeviceAddressMultiDevice", s.bufferDeviceAddressMultiDevice != VK_FALSE);
	w.KeyBool("vulkanMemoryModel", s.vulkanMemoryModel != VK_FALSE);
	w.KeyBool("vulkanMemoryModelDeviceScope", s.vulkanMemoryModelDeviceScope != VK_FALSE);
	w.KeyBool("vulkanMemoryModelAvailabilityVisibilityChains", s.vulkanMemoryModelAvailabilityVisibilityChains != VK_FALSE);
	w.KeyBool("shaderOutputViewportIndex", s.shaderOutputViewportIndex != VK_FALSE);
	w.KeyBool("shaderOutputLayer", s.shaderOutputLayer != VK_FALSE);
	w.KeyBool("subgroupBroadcastDynamicId", s.subgroupBroadcastDynamicId != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceVulkan13Features& s)
{
	w.BeginObject();
	w.KeyBool("robustImageAccess", s.robustImageAccess != VK_FALSE);
	w.KeyBool("inlineUniformBlock", s.inlineUniformBlock != VK_FALSE);
	w.KeyBool("descriptorBindingInlineUniformBlockUpdateAfterBind", s.descriptorBindingInlineUniformBlockUpdateAfterBind != VK_FALSE);
	w.KeyBool("pipelineCreationCacheControl", s.pipelineCreationCacheControl != VK_FALSE);
	w.KeyBool("privateData", s.privateData != VK_FALSE);
	w.KeyBool("shaderDemoteToHelperInvocation", s.shaderDemoteToHelperInvocation != VK_FALSE);
	w.KeyBool("shaderTerminateInvocation", s.shaderTerminateInvocation != VK_FALSE);
	w.KeyBool("subgroupSizeControl", s.subgroupSizeControl != VK_FALSE);
	w.KeyBool("computeFullSubgroups", s.computeFullSubgroups != VK_FALSE);
	w.KeyBool("synchronization2", s.synchronization2 != VK_FALSE);
	w.KeyBool("textureCompressionASTC_HDR", s.textureCompressionASTC_HDR != VK_FALSE);
	w.KeyBool("shaderZeroInitializeWorkgroupMemory", s.shaderZeroInitializeWorkgroupMemory != VK_FALSE);
	w.KeyBool("dynamicRendering", s.dynamicRendering != VK_FALSE);
	w.KeyBool("shaderIntegerDotProduct", s.shaderIntegerDotProduct != VK_FALSE);
	w.KeyBool("maintenance4", s.maintenance4 != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceVulkan14Features& s)
{
	w.BeginObject();
	w.KeyBool("globalPriorityQuery", s.globalPriorityQuery != VK_FALSE);
	w.KeyBool("shaderSubgroupRotate", s.shaderSubgroupRotate != VK_FALSE);
	w.KeyBool("shaderSubgroupRotateClustered", s.shaderSubgroupRotateClustered != VK_FALSE);
	w.KeyBool("shaderFloatControls2", s.shaderFloatControls2 != VK_FALSE);
	w.KeyBool("shaderExpectAssume", s.shaderExpectAssume != VK_FALSE);
	w.KeyBool("rectangularLines", s.rectangularLines != VK_FALSE);
	w.KeyBool("bresenhamLines", s.bresenhamLines != VK_FALSE);
	w.KeyBool("smoothLines", s.smoothLines != VK_FALSE);
	w.KeyBool("stippledRectangularLines", s.stippledRectangularLines != VK_FALSE);
	w.KeyBool("stippledBresenhamLines", s.stippledBresenhamLines != VK_FALSE);
	w.KeyBool("stippledSmoothLines", s.stippledSmoothLines != VK_FALSE);
	w.KeyBool("vertexAttributeInstanceRateDivisor", s.vertexAttributeInstanceRateDivisor != VK_FALSE);
	w.KeyBool("vertexAttributeInstanceRateZeroDivisor", s.vertexAttributeInstanceRateZeroDivisor != VK_FALSE);
	w.KeyBool("indexTypeUint8", s.indexTypeUint8 != VK_FALSE);
	w.KeyBool("dynamicRenderingLocalRead", s.dynamicRenderingLocalRead != VK_FALSE);
	w.KeyBool("maintenance5", s.maintenance5 != VK_FALSE);
	w.KeyBool("maintenance6", s.maintenance6 != VK_FALSE);
	w.KeyBool("pipelineProtectedAccess", s.pipelineProtectedAccess != VK_FALSE);
	w.KeyBool("pipelineRobustness", s.pipelineRobustness != VK_FALSE);
	w.KeyBool("hostImageCopy", s.hostImageCopy != VK_FALSE);
	w.KeyBool("pushDescriptor", s.pushDescriptor != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceVulkan11Properties& s)
{
	w.BeginObject();
	w.KeyString("deviceUUID", HexBytes(s.deviceUUID, sizeof(s.deviceUUID)));
	w.KeyString("driverUUID", HexBytes(s.driverUUID, sizeof(s.driverUUID)));
	w.KeyString("deviceLUID", HexBytes(s.deviceLUID, sizeof(s.deviceLUID)));
	w.KeyUInt("deviceNodeMask", static_cast<uint64_t>(s.deviceNodeMask));
	w.KeyBool("deviceLUIDValid", s.deviceLUIDValid != VK_FALSE);
	w.KeyUInt("subgroupSize", static_cast<uint64_t>(s.subgroupSize));
	w.KeyHex("subgroupSupportedStages", static_cast<uint64_t>(s.subgroupSupportedStages));
	w.KeyHex("subgroupSupportedOperations", static_cast<uint64_t>(s.subgroupSupportedOperations));
	w.KeyBool("subgroupQuadOperationsInAllStages", s.subgroupQuadOperationsInAllStages != VK_FALSE);
	w.KeyInt("pointClippingBehavior", static_cast<int64_t>(s.pointClippingBehavior));
	w.KeyUInt("maxMultiviewViewCount", static_cast<uint64_t>(s.maxMultiviewViewCount));
	w.KeyUInt("maxMultiviewInstanceIndex", static_cast<uint64_t>(s.maxMultiviewInstanceIndex));
	w.KeyBool("protectedNoFault", s.protectedNoFault != VK_FALSE);
	w.KeyUInt("maxPerSetDescriptors", static_cast<uint64_t>(s.maxPerSetDescriptors));
	w.KeyUInt("maxMemoryAllocationSize", static_cast<uint64_t>(s.maxMemoryAllocationSize));
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceVulkan12Properties& s)
{
	w.BeginObject();
	w.KeyInt("driverID", static_cast<int64_t>(s.driverID));
	w.KeyString("driverName", CStr(s.driverName, sizeof(s.driverName)));
	w.KeyString("driverInfo", CStr(s.driverInfo, sizeof(s.driverInfo)));
	w.KeyString("conformanceVersion", ConformanceString(s.conformanceVersion));
	w.KeyInt("denormBehaviorIndependence", static_cast<int64_t>(s.denormBehaviorIndependence));
	w.KeyInt("roundingModeIndependence", static_cast<int64_t>(s.roundingModeIndependence));
	w.KeyBool("shaderSignedZeroInfNanPreserveFloat16", s.shaderSignedZeroInfNanPreserveFloat16 != VK_FALSE);
	w.KeyBool("shaderSignedZeroInfNanPreserveFloat32", s.shaderSignedZeroInfNanPreserveFloat32 != VK_FALSE);
	w.KeyBool("shaderSignedZeroInfNanPreserveFloat64", s.shaderSignedZeroInfNanPreserveFloat64 != VK_FALSE);
	w.KeyBool("shaderDenormPreserveFloat16", s.shaderDenormPreserveFloat16 != VK_FALSE);
	w.KeyBool("shaderDenormPreserveFloat32", s.shaderDenormPreserveFloat32 != VK_FALSE);
	w.KeyBool("shaderDenormPreserveFloat64", s.shaderDenormPreserveFloat64 != VK_FALSE);
	w.KeyBool("shaderDenormFlushToZeroFloat16", s.shaderDenormFlushToZeroFloat16 != VK_FALSE);
	w.KeyBool("shaderDenormFlushToZeroFloat32", s.shaderDenormFlushToZeroFloat32 != VK_FALSE);
	w.KeyBool("shaderDenormFlushToZeroFloat64", s.shaderDenormFlushToZeroFloat64 != VK_FALSE);
	w.KeyBool("shaderRoundingModeRTEFloat16", s.shaderRoundingModeRTEFloat16 != VK_FALSE);
	w.KeyBool("shaderRoundingModeRTEFloat32", s.shaderRoundingModeRTEFloat32 != VK_FALSE);
	w.KeyBool("shaderRoundingModeRTEFloat64", s.shaderRoundingModeRTEFloat64 != VK_FALSE);
	w.KeyBool("shaderRoundingModeRTZFloat16", s.shaderRoundingModeRTZFloat16 != VK_FALSE);
	w.KeyBool("shaderRoundingModeRTZFloat32", s.shaderRoundingModeRTZFloat32 != VK_FALSE);
	w.KeyBool("shaderRoundingModeRTZFloat64", s.shaderRoundingModeRTZFloat64 != VK_FALSE);
	w.KeyUInt("maxUpdateAfterBindDescriptorsInAllPools", static_cast<uint64_t>(s.maxUpdateAfterBindDescriptorsInAllPools));
	w.KeyBool("shaderUniformBufferArrayNonUniformIndexingNative", s.shaderUniformBufferArrayNonUniformIndexingNative != VK_FALSE);
	w.KeyBool("shaderSampledImageArrayNonUniformIndexingNative", s.shaderSampledImageArrayNonUniformIndexingNative != VK_FALSE);
	w.KeyBool("shaderStorageBufferArrayNonUniformIndexingNative", s.shaderStorageBufferArrayNonUniformIndexingNative != VK_FALSE);
	w.KeyBool("shaderStorageImageArrayNonUniformIndexingNative", s.shaderStorageImageArrayNonUniformIndexingNative != VK_FALSE);
	w.KeyBool("shaderInputAttachmentArrayNonUniformIndexingNative", s.shaderInputAttachmentArrayNonUniformIndexingNative != VK_FALSE);
	w.KeyBool("robustBufferAccessUpdateAfterBind", s.robustBufferAccessUpdateAfterBind != VK_FALSE);
	w.KeyBool("quadDivergentImplicitLod", s.quadDivergentImplicitLod != VK_FALSE);
	w.KeyUInt("maxPerStageDescriptorUpdateAfterBindSamplers", static_cast<uint64_t>(s.maxPerStageDescriptorUpdateAfterBindSamplers));
	w.KeyUInt("maxPerStageDescriptorUpdateAfterBindUniformBuffers", static_cast<uint64_t>(s.maxPerStageDescriptorUpdateAfterBindUniformBuffers));
	w.KeyUInt("maxPerStageDescriptorUpdateAfterBindStorageBuffers", static_cast<uint64_t>(s.maxPerStageDescriptorUpdateAfterBindStorageBuffers));
	w.KeyUInt("maxPerStageDescriptorUpdateAfterBindSampledImages", static_cast<uint64_t>(s.maxPerStageDescriptorUpdateAfterBindSampledImages));
	w.KeyUInt("maxPerStageDescriptorUpdateAfterBindStorageImages", static_cast<uint64_t>(s.maxPerStageDescriptorUpdateAfterBindStorageImages));
	w.KeyUInt("maxPerStageDescriptorUpdateAfterBindInputAttachments", static_cast<uint64_t>(s.maxPerStageDescriptorUpdateAfterBindInputAttachments));
	w.KeyUInt("maxPerStageUpdateAfterBindResources", static_cast<uint64_t>(s.maxPerStageUpdateAfterBindResources));
	w.KeyUInt("maxDescriptorSetUpdateAfterBindSamplers", static_cast<uint64_t>(s.maxDescriptorSetUpdateAfterBindSamplers));
	w.KeyUInt("maxDescriptorSetUpdateAfterBindUniformBuffers", static_cast<uint64_t>(s.maxDescriptorSetUpdateAfterBindUniformBuffers));
	w.KeyUInt("maxDescriptorSetUpdateAfterBindUniformBuffersDynamic", static_cast<uint64_t>(s.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic));
	w.KeyUInt("maxDescriptorSetUpdateAfterBindStorageBuffers", static_cast<uint64_t>(s.maxDescriptorSetUpdateAfterBindStorageBuffers));
	w.KeyUInt("maxDescriptorSetUpdateAfterBindStorageBuffersDynamic", static_cast<uint64_t>(s.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic));
	w.KeyUInt("maxDescriptorSetUpdateAfterBindSampledImages", static_cast<uint64_t>(s.maxDescriptorSetUpdateAfterBindSampledImages));
	w.KeyUInt("maxDescriptorSetUpdateAfterBindStorageImages", static_cast<uint64_t>(s.maxDescriptorSetUpdateAfterBindStorageImages));
	w.KeyUInt("maxDescriptorSetUpdateAfterBindInputAttachments", static_cast<uint64_t>(s.maxDescriptorSetUpdateAfterBindInputAttachments));
	w.KeyHex("supportedDepthResolveModes", static_cast<uint64_t>(s.supportedDepthResolveModes));
	w.KeyHex("supportedStencilResolveModes", static_cast<uint64_t>(s.supportedStencilResolveModes));
	w.KeyBool("independentResolveNone", s.independentResolveNone != VK_FALSE);
	w.KeyBool("independentResolve", s.independentResolve != VK_FALSE);
	w.KeyBool("filterMinmaxSingleComponentFormats", s.filterMinmaxSingleComponentFormats != VK_FALSE);
	w.KeyBool("filterMinmaxImageComponentMapping", s.filterMinmaxImageComponentMapping != VK_FALSE);
	w.KeyUInt("maxTimelineSemaphoreValueDifference", static_cast<uint64_t>(s.maxTimelineSemaphoreValueDifference));
	w.KeyHex("framebufferIntegerColorSampleCounts", static_cast<uint64_t>(s.framebufferIntegerColorSampleCounts));
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceVulkan13Properties& s)
{
	w.BeginObject();
	w.KeyUInt("minSubgroupSize", static_cast<uint64_t>(s.minSubgroupSize));
	w.KeyUInt("maxSubgroupSize", static_cast<uint64_t>(s.maxSubgroupSize));
	w.KeyUInt("maxComputeWorkgroupSubgroups", static_cast<uint64_t>(s.maxComputeWorkgroupSubgroups));
	w.KeyHex("requiredSubgroupSizeStages", static_cast<uint64_t>(s.requiredSubgroupSizeStages));
	w.KeyUInt("maxInlineUniformBlockSize", static_cast<uint64_t>(s.maxInlineUniformBlockSize));
	w.KeyUInt("maxPerStageDescriptorInlineUniformBlocks", static_cast<uint64_t>(s.maxPerStageDescriptorInlineUniformBlocks));
	w.KeyUInt("maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks", static_cast<uint64_t>(s.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks));
	w.KeyUInt("maxDescriptorSetInlineUniformBlocks", static_cast<uint64_t>(s.maxDescriptorSetInlineUniformBlocks));
	w.KeyUInt("maxDescriptorSetUpdateAfterBindInlineUniformBlocks", static_cast<uint64_t>(s.maxDescriptorSetUpdateAfterBindInlineUniformBlocks));
	w.KeyUInt("maxInlineUniformTotalSize", static_cast<uint64_t>(s.maxInlineUniformTotalSize));
	w.KeyBool("integerDotProduct8BitUnsignedAccelerated", s.integerDotProduct8BitUnsignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct8BitSignedAccelerated", s.integerDotProduct8BitSignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct8BitMixedSignednessAccelerated", s.integerDotProduct8BitMixedSignednessAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct4x8BitPackedUnsignedAccelerated", s.integerDotProduct4x8BitPackedUnsignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct4x8BitPackedSignedAccelerated", s.integerDotProduct4x8BitPackedSignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct4x8BitPackedMixedSignednessAccelerated", s.integerDotProduct4x8BitPackedMixedSignednessAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct16BitUnsignedAccelerated", s.integerDotProduct16BitUnsignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct16BitSignedAccelerated", s.integerDotProduct16BitSignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct16BitMixedSignednessAccelerated", s.integerDotProduct16BitMixedSignednessAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct32BitUnsignedAccelerated", s.integerDotProduct32BitUnsignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct32BitSignedAccelerated", s.integerDotProduct32BitSignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct32BitMixedSignednessAccelerated", s.integerDotProduct32BitMixedSignednessAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct64BitUnsignedAccelerated", s.integerDotProduct64BitUnsignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct64BitSignedAccelerated", s.integerDotProduct64BitSignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProduct64BitMixedSignednessAccelerated", s.integerDotProduct64BitMixedSignednessAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating8BitUnsignedAccelerated", s.integerDotProductAccumulatingSaturating8BitUnsignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating8BitSignedAccelerated", s.integerDotProductAccumulatingSaturating8BitSignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated", s.integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated", s.integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated", s.integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated", s.integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating16BitUnsignedAccelerated", s.integerDotProductAccumulatingSaturating16BitUnsignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating16BitSignedAccelerated", s.integerDotProductAccumulatingSaturating16BitSignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated", s.integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating32BitUnsignedAccelerated", s.integerDotProductAccumulatingSaturating32BitUnsignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating32BitSignedAccelerated", s.integerDotProductAccumulatingSaturating32BitSignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated", s.integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating64BitUnsignedAccelerated", s.integerDotProductAccumulatingSaturating64BitUnsignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating64BitSignedAccelerated", s.integerDotProductAccumulatingSaturating64BitSignedAccelerated != VK_FALSE);
	w.KeyBool("integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated", s.integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated != VK_FALSE);
	w.KeyUInt("storageTexelBufferOffsetAlignmentBytes", static_cast<uint64_t>(s.storageTexelBufferOffsetAlignmentBytes));
	w.KeyBool("storageTexelBufferOffsetSingleTexelAlignment", s.storageTexelBufferOffsetSingleTexelAlignment != VK_FALSE);
	w.KeyUInt("uniformTexelBufferOffsetAlignmentBytes", static_cast<uint64_t>(s.uniformTexelBufferOffsetAlignmentBytes));
	w.KeyBool("uniformTexelBufferOffsetSingleTexelAlignment", s.uniformTexelBufferOffsetSingleTexelAlignment != VK_FALSE);
	w.KeyUInt("maxBufferSize", static_cast<uint64_t>(s.maxBufferSize));
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceDriverProperties& s)
{
	w.BeginObject();
	w.KeyInt("driverID", static_cast<int64_t>(s.driverID));
	w.KeyString("driverName", CStr(s.driverName, sizeof(s.driverName)));
	w.KeyString("driverInfo", CStr(s.driverInfo, sizeof(s.driverInfo)));
	w.KeyString("conformanceVersion", ConformanceString(s.conformanceVersion));
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceSubgroupProperties& s)
{
	w.BeginObject();
	w.KeyUInt("subgroupSize", static_cast<uint64_t>(s.subgroupSize));
	w.KeyHex("supportedStages", static_cast<uint64_t>(s.supportedStages));
	w.KeyHex("supportedOperations", static_cast<uint64_t>(s.supportedOperations));
	w.KeyBool("quadOperationsInAllStages", s.quadOperationsInAllStages != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDevicePushDescriptorProperties& s)
{
	w.BeginObject();
	w.KeyUInt("maxPushDescriptors", static_cast<uint64_t>(s.maxPushDescriptors));
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceProvokingVertexPropertiesEXT& s)
{
	w.BeginObject();
	w.KeyBool("provokingVertexModePerPipeline", s.provokingVertexModePerPipeline != VK_FALSE);
	w.KeyBool("transformFeedbackPreservesTriangleFanProvokingVertex", s.transformFeedbackPreservesTriangleFanProvokingVertex != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceLineRasterizationProperties& s)
{
	w.BeginObject();
	w.KeyUInt("lineSubPixelPrecisionBits", static_cast<uint64_t>(s.lineSubPixelPrecisionBits));
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceRobustness2PropertiesKHR& s)
{
	w.BeginObject();
	w.KeyUInt("robustStorageBufferAccessSizeAlignment", static_cast<uint64_t>(s.robustStorageBufferAccessSizeAlignment));
	w.KeyUInt("robustUniformBufferAccessSizeAlignment", static_cast<uint64_t>(s.robustUniformBufferAccessSizeAlignment));
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT& s)
{
	w.BeginObject();
	w.KeyBool("graphicsPipelineLibraryFastLinking", s.graphicsPipelineLibraryFastLinking != VK_FALSE);
	w.KeyBool("graphicsPipelineLibraryIndependentInterpolationDecoration", s.graphicsPipelineLibraryIndependentInterpolationDecoration != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT& s)
{
	w.BeginObject();
	w.KeyBool("fragmentShaderSampleInterlock", s.fragmentShaderSampleInterlock != VK_FALSE);
	w.KeyBool("fragmentShaderPixelInterlock", s.fragmentShaderPixelInterlock != VK_FALSE);
	w.KeyBool("fragmentShaderShadingRateInterlock", s.fragmentShaderShadingRateInterlock != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT& s)
{
	w.BeginObject();
	w.KeyBool("rasterizationOrderColorAttachmentAccess", s.rasterizationOrderColorAttachmentAccess != VK_FALSE);
	w.KeyBool("rasterizationOrderDepthAttachmentAccess", s.rasterizationOrderDepthAttachmentAccess != VK_FALSE);
	w.KeyBool("rasterizationOrderStencilAttachmentAccess", s.rasterizationOrderStencilAttachmentAccess != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT& s)
{
	w.BeginObject();
	w.KeyBool("attachmentFeedbackLoopLayout", s.attachmentFeedbackLoopLayout != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT& s)
{
	w.BeginObject();
	w.KeyBool("attachmentFeedbackLoopDynamicState", s.attachmentFeedbackLoopDynamicState != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceDynamicRenderingLocalReadFeatures& s)
{
	w.BeginObject();
	w.KeyBool("dynamicRenderingLocalRead", s.dynamicRenderingLocalRead != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceProvokingVertexFeaturesEXT& s)
{
	w.BeginObject();
	w.KeyBool("provokingVertexLast", s.provokingVertexLast != VK_FALSE);
	w.KeyBool("transformFeedbackPreservesProvokingVertex", s.transformFeedbackPreservesProvokingVertex != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceLineRasterizationFeatures& s)
{
	w.BeginObject();
	w.KeyBool("rectangularLines", s.rectangularLines != VK_FALSE);
	w.KeyBool("bresenhamLines", s.bresenhamLines != VK_FALSE);
	w.KeyBool("smoothLines", s.smoothLines != VK_FALSE);
	w.KeyBool("stippledRectangularLines", s.stippledRectangularLines != VK_FALSE);
	w.KeyBool("stippledBresenhamLines", s.stippledBresenhamLines != VK_FALSE);
	w.KeyBool("stippledSmoothLines", s.stippledSmoothLines != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceRobustness2FeaturesKHR& s)
{
	w.BeginObject();
	w.KeyBool("robustBufferAccess2", s.robustBufferAccess2 != VK_FALSE);
	w.KeyBool("robustImageAccess2", s.robustImageAccess2 != VK_FALSE);
	w.KeyBool("nullDescriptor", s.nullDescriptor != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceFaultFeaturesEXT& s)
{
	w.BeginObject();
	w.KeyBool("deviceFault", s.deviceFault != VK_FALSE);
	w.KeyBool("deviceFaultVendorBinary", s.deviceFaultVendorBinary != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR& s)
{
	w.BeginObject();
	w.KeyBool("swapchainMaintenance1", s.swapchainMaintenance1 != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceVulkanMemoryModelFeatures& s)
{
	w.BeginObject();
	w.KeyBool("vulkanMemoryModel", s.vulkanMemoryModel != VK_FALSE);
	w.KeyBool("vulkanMemoryModelDeviceScope", s.vulkanMemoryModelDeviceScope != VK_FALSE);
	w.KeyBool("vulkanMemoryModelAvailabilityVisibilityChains", s.vulkanMemoryModelAvailabilityVisibilityChains != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT& s)
{
	w.BeginObject();
	w.KeyBool("graphicsPipelineLibrary", s.graphicsPipelineLibrary != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceExtendedDynamicStateFeaturesEXT& s)
{
	w.BeginObject();
	w.KeyBool("extendedDynamicState", s.extendedDynamicState != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceExtendedDynamicState2FeaturesEXT& s)
{
	w.BeginObject();
	w.KeyBool("extendedDynamicState2", s.extendedDynamicState2 != VK_FALSE);
	w.KeyBool("extendedDynamicState2LogicOp", s.extendedDynamicState2LogicOp != VK_FALSE);
	w.KeyBool("extendedDynamicState2PatchControlPoints", s.extendedDynamicState2PatchControlPoints != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceDynamicRenderingFeatures& s)
{
	w.BeginObject();
	w.KeyBool("dynamicRendering", s.dynamicRendering != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceImagelessFramebufferFeatures& s)
{
	w.BeginObject();
	w.KeyBool("imagelessFramebuffer", s.imagelessFramebuffer != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT& s)
{
	w.BeginObject();
	w.KeyBool("primitiveTopologyListRestart", s.primitiveTopologyListRestart != VK_FALSE);
	w.KeyBool("primitiveTopologyPatchListRestart", s.primitiveTopologyPatchListRestart != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceCustomBorderColorFeaturesEXT& s)
{
	w.BeginObject();
	w.KeyBool("customBorderColors", s.customBorderColors != VK_FALSE);
	w.KeyBool("customBorderColorWithoutFormat", s.customBorderColorWithoutFormat != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceMaintenance4Features& s)
{
	w.BeginObject();
	w.KeyBool("maintenance4", s.maintenance4 != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures& s)
{
	w.BeginObject();
	w.KeyBool("shaderDemoteToHelperInvocation", s.shaderDemoteToHelperInvocation != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceSynchronization2Features& s)
{
	w.BeginObject();
	w.KeyBool("synchronization2", s.synchronization2 != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceTimelineSemaphoreFeatures& s)
{
	w.BeginObject();
	w.KeyBool("timelineSemaphore", s.timelineSemaphore != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceDescriptorIndexingFeatures& s)
{
	w.BeginObject();
	w.KeyBool("shaderInputAttachmentArrayDynamicIndexing", s.shaderInputAttachmentArrayDynamicIndexing != VK_FALSE);
	w.KeyBool("shaderUniformTexelBufferArrayDynamicIndexing", s.shaderUniformTexelBufferArrayDynamicIndexing != VK_FALSE);
	w.KeyBool("shaderStorageTexelBufferArrayDynamicIndexing", s.shaderStorageTexelBufferArrayDynamicIndexing != VK_FALSE);
	w.KeyBool("shaderUniformBufferArrayNonUniformIndexing", s.shaderUniformBufferArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderSampledImageArrayNonUniformIndexing", s.shaderSampledImageArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderStorageBufferArrayNonUniformIndexing", s.shaderStorageBufferArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderStorageImageArrayNonUniformIndexing", s.shaderStorageImageArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderInputAttachmentArrayNonUniformIndexing", s.shaderInputAttachmentArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderUniformTexelBufferArrayNonUniformIndexing", s.shaderUniformTexelBufferArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("shaderStorageTexelBufferArrayNonUniformIndexing", s.shaderStorageTexelBufferArrayNonUniformIndexing != VK_FALSE);
	w.KeyBool("descriptorBindingUniformBufferUpdateAfterBind", s.descriptorBindingUniformBufferUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingSampledImageUpdateAfterBind", s.descriptorBindingSampledImageUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingStorageImageUpdateAfterBind", s.descriptorBindingStorageImageUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingStorageBufferUpdateAfterBind", s.descriptorBindingStorageBufferUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingUniformTexelBufferUpdateAfterBind", s.descriptorBindingUniformTexelBufferUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingStorageTexelBufferUpdateAfterBind", s.descriptorBindingStorageTexelBufferUpdateAfterBind != VK_FALSE);
	w.KeyBool("descriptorBindingUpdateUnusedWhilePending", s.descriptorBindingUpdateUnusedWhilePending != VK_FALSE);
	w.KeyBool("descriptorBindingPartiallyBound", s.descriptorBindingPartiallyBound != VK_FALSE);
	w.KeyBool("descriptorBindingVariableDescriptorCount", s.descriptorBindingVariableDescriptorCount != VK_FALSE);
	w.KeyBool("runtimeDescriptorArray", s.runtimeDescriptorArray != VK_FALSE);
	w.EndObject();
}

static void WriteStruct(JsonWriter& w, const VkPhysicalDeviceIndexTypeUint8Features& s)
{
	w.BeginObject();
	w.KeyBool("indexTypeUint8", s.indexTypeUint8 != VK_FALSE);
	w.EndObject();
}
