#pragma once

#include "spatial/core/imodules/IProfilerModule.hpp"

class CemuVulkanGpuProfilerScope
{
  public:
	CemuVulkanGpuProfilerScope(const spatial::ProfilerMethodInfo& methodInfo, VkCommandBuffer commandBuffer)
		: m_context{reinterpret_cast<void*>(commandBuffer)}
	{
		if (spatial::modules::gProfilerConnected)
			spatial::modules::getProfiler()->beginGPUSample(methodInfo, &m_context, nullptr);
	}

	~CemuVulkanGpuProfilerScope()
	{
		if (m_context.tracyScope != nullptr)
			spatial::modules::getProfiler()->endGPUSample(&m_context);
	}

	CemuVulkanGpuProfilerScope(const CemuVulkanGpuProfilerScope&) = delete;
	CemuVulkanGpuProfilerScope& operator=(const CemuVulkanGpuProfilerScope&) = delete;

  private:
	spatial::VulkanGPUProfileZoneCtx m_context;
};

#define CEMU_VULKAN_GPU_PROFILE_SCOPE(name, commandBuffer)                                         \
	static auto SPATIAL_UNIQUE(cemu_gpu_method_) = spatial::modules::getProfiler()->createMethodInfo( \
		(name), "", __FILE__, __LINE__, SPATIAL_FUNCTION);                                         \
	CemuVulkanGpuProfilerScope SPATIAL_UNIQUE(cemu_gpu_scope_){                                    \
		SPATIAL_UNIQUE(cemu_gpu_method_), (commandBuffer)}
