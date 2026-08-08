#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteFrameGraphShadow.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "WindowSystem.h"

#include "spatial/profiler/Profiler.h"

#include <fmt/format.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <sstream>
#include <vector>

performanceMonitor_t performanceMonitor{};

namespace
{
	struct CommandStreamMetrics
	{
		// Guest submission counters cross from Espresso threads to LatteThread. All Host decode
		// counters are produced and consumed by LatteThread, so they intentionally avoid an
		// atomic operation for every PM4 packet and draw.
		std::atomic<uint64> guestSubmissions{};
		std::atomic<uint64> guestWords{};
		uint64 hostSubmissions{};
		uint64 hostWords{};
		std::array<uint64, static_cast<size_t>(LatteCommandPacketCategory::Count)> packetCounts{};
		std::array<uint64, static_cast<size_t>(LatteCommandPacketCategory::Count)> packetWords{};
		std::array<uint64, static_cast<size_t>(LatteCommandHostTimeCategory::Count)> hostTimeNs{};
		uint64 changedRegisterPackets{};
		uint64 changedRegisterPacketWords{};
		uint64 redundantRegisterPackets{};
		uint64 redundantRegisterPacketWords{};
		uint64 registerPayloadWords{};
		uint64 elidedRegisterStoreWords{};
		std::array<uint64, static_cast<size_t>(LatteCommandPacketCategory::Count)> changedRegisterPacketsByCategory{};
		std::array<uint64, static_cast<size_t>(LatteCommandPacketCategory::Count)> redundantRegisterPacketsByCategory{};
		std::array<uint64, static_cast<size_t>(LatteCommandPacketCategory::Count)> registerPayloadWordsByCategory{};
		std::array<uint64, static_cast<size_t>(LatteCommandPacketCategory::Count)> elidedRegisterStoreWordsByCategory{};
		uint64 drawPasses{};
		uint64 fullDraws{};
		uint64 fastDraws{};
		std::array<uint64, static_cast<size_t>(LatteDrawPassEndReason::Count)> drawPassEndReasons{};
		uint64 vulkanSubmitRecordedDrawPasses{};
		uint64 vulkanZeroGuestDrawSubmits{};
		std::array<uint64, static_cast<size_t>(LatteVulkanSubmitReason::Count)> vulkanSubmitReasons{};
		std::array<uint64, static_cast<size_t>(LatteVulkanSubmitReason::Count)> vulkanSubmitCpuNs{};
		std::array<uint64, static_cast<size_t>(LatteVulkanRenderPassEndReason::Count)> vulkanRenderPassEndReasons{};
		std::array<uint64, static_cast<size_t>(LatteVulkanRenderPassEndReason::Count)> vulkanRenderPassEndDraws{};
		uint64 vulkanOneDrawRenderPasses{};
		uint64 vulkanAtMostFourDrawRenderPasses{};
		uint64 vulkanDepthStoreOmittedPasses{};
		uint64 vulkanPixelSelfDependencySplits{};
		uint64 vulkanNonPixelSelfDependencySplits{};
		std::array<uint64, static_cast<size_t>(LatteBufferCacheUploadSource::Count)> bufferCacheUploadCalls{};
		std::array<uint64, static_cast<size_t>(LatteBufferCacheUploadSource::Count)> bufferCacheUploadBytes{};
		uint64 bufferCacheCopyCalls{};
		uint64 bufferCacheCopyBytes{};
		uint64 bufferCacheUploadBatchFlushes{};
		uint64 bufferCacheUploadBatchRegions{};
		uint64 bufferCacheUploadBatchCopyCommands{};
		uint64 vertexBufferBindCalls{};
		uint64 vertexBufferBindBytes{};
		uint64 vertexBufferBindMaxBytes{};
		uint64 uniformRingBankUploadCalls{};
		uint64 uniformRingBankUploadBytes{};
		uint64 uniformRingBankBindCalls{};
		uint64 uniformRingBankBindBytes{};
		uint64 uniformRingBankReuseCalls{};
		uint64 uniformRingBankReuseBytes{};
		std::array<uint64, static_cast<size_t>(LatteDirtyStateDomain::Count)> dirtyClassifiedWords{};
		std::array<uint64, static_cast<size_t>(LatteDirtyStateDomain::Count)> dirtyMarks{};
		std::array<uint64, static_cast<size_t>(LatteDirtyStateDomain::Count)> dirtyConsumes{};
		uint64 pipelineHashCalls{};
		std::array<uint64, static_cast<size_t>(LattePipelineLookupOutcome::Count)> pipelineLookups{};
		uint64 descriptorSnapshotHits{};
		uint64 descriptorHashCalls{};
		uint64 descriptorCacheHits{};
		uint64 descriptorCacheMisses{};
		std::array<uint64, static_cast<size_t>(LatteDynamicState::Count)> dynamicStateRequests{};
		std::array<uint64, static_cast<size_t>(LatteDynamicState::Count)> dynamicStateEmits{};
		std::atomic<uint64> guestSubmissionsTotal{};
		std::atomic<uint64> guestWordsTotal{};
		uint64 hostSubmissionsTotal{};
		uint64 hostWordsTotal{};
	};

	CommandStreamMetrics s_commandStreamMetrics;
	std::array<std::atomic<uint64>, 0x10000> s_contextDrawPassBreakCounts{};
	std::array<std::atomic<uint32>, 0x10000> s_contextDrawPassBreakEnds{};
	std::atomic<uint64> s_contextDrawPassBreakTotal{};
	std::array<std::atomic<uint64>, static_cast<size_t>(LatteCommandPacketCategory::Count)> s_registerChangedPacketTotals{};
	std::array<std::atomic<uint64>, static_cast<size_t>(LatteCommandPacketCategory::Count)> s_registerRedundantPacketTotals{};
	std::array<std::atomic<uint64>, static_cast<size_t>(LatteCommandPacketCategory::Count)> s_registerPayloadWordTotals{};
	std::array<std::atomic<uint64>, static_cast<size_t>(LatteCommandPacketCategory::Count)> s_registerElidedStoreWordTotals{};
	std::array<std::atomic<uint64>, static_cast<size_t>(LatteDirtyStateDomain::Count)> s_dirtyClassifiedWordTotals{};
	std::array<std::atomic<uint64>, static_cast<size_t>(LatteDirtyStateDomain::Count)> s_dirtyMarkTotals{};
	std::array<std::atomic<uint64>, static_cast<size_t>(LatteDirtyStateDomain::Count)> s_dirtyConsumeTotals{};
	std::atomic<uint64> s_pipelineHashCallTotal{};
	std::array<std::atomic<uint64>, static_cast<size_t>(LattePipelineLookupOutcome::Count)> s_pipelineLookupTotals{};
	std::atomic<uint64> s_descriptorHashCallTotal{};
	std::atomic<uint64> s_descriptorSnapshotHitTotal{};
	std::atomic<uint64> s_descriptorCacheHitTotal{};
	std::atomic<uint64> s_descriptorCacheMissTotal{};
	std::array<std::atomic<uint64>, static_cast<size_t>(LatteDynamicState::Count)> s_dynamicStateRequestTotals{};
	std::array<std::atomic<uint64>, static_cast<size_t>(LatteDynamicState::Count)> s_dynamicStateEmitTotals{};

	static constexpr std::array<const char*, static_cast<size_t>(LatteDirtyStateDomain::Count)> s_dirtyDomainNames = {
		"texture", "vertex_buffer", "vertex_uniform_buffer", "pixel_uniform_buffer",
		"geometry_uniform_buffer", "vertex_alu_constant", "pixel_alu_constant", "unclassified",
	};
	static constexpr std::array<const char*, static_cast<size_t>(LattePipelineLookupOutcome::Count)> s_pipelineLookupNames = {
		"transition_hit", "global_hit", "miss",
	};
	static constexpr std::array<const char*, static_cast<size_t>(LatteDynamicState::Count)> s_dynamicStateNames = {
		"blend_constants", "depth_bias",
	};

	const char* GetRegisterDomainName(LatteCommandPacketCategory category)
	{
		switch (category)
		{
		case LatteCommandPacketCategory::RegisterContext:
			return "context";
		case LatteCommandPacketCategory::RegisterResource:
			return "resource";
		case LatteCommandPacketCategory::RegisterConstant:
			return "constant";
		case LatteCommandPacketCategory::RegisterSampler:
			return "sampler";
		case LatteCommandPacketCategory::RegisterConfig:
			return "config";
		default:
			return nullptr;
		}
	}

	uint64 Consume(std::atomic<uint64>& value)
	{
		return value.exchange(0, std::memory_order_relaxed);
	}

	uint64 Consume(uint64& value)
	{
		const uint64 consumedValue = value;
		value = 0;
		return consumedValue;
	}

	void PublishCommandStreamMetrics()
	{
		const uint64 guestSubmissions = Consume(s_commandStreamMetrics.guestSubmissions);
		const uint64 guestWords = Consume(s_commandStreamMetrics.guestWords);
		const uint64 hostSubmissions = Consume(s_commandStreamMetrics.hostSubmissions);
		const uint64 hostWords = Consume(s_commandStreamMetrics.hostWords);
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.guest.submissions_per_frame", guestSubmissions, "Cemu Command Stream", "submissions");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.guest.words_per_frame", guestWords, "Cemu Command Stream", "words");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.submissions_per_frame", hostSubmissions, "Cemu Command Stream", "submissions");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.words_per_frame", hostWords, "Cemu Command Stream", "words");

		const sint64 pendingSubmissions = static_cast<sint64>(s_commandStreamMetrics.guestSubmissionsTotal.load(std::memory_order_relaxed)) -
			static_cast<sint64>(s_commandStreamMetrics.hostSubmissionsTotal);
		const sint64 pendingWords = static_cast<sint64>(s_commandStreamMetrics.guestWordsTotal.load(std::memory_order_relaxed)) -
			static_cast<sint64>(s_commandStreamMetrics.hostWordsTotal);
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.queue.pending_submissions", pendingSubmissions, "Cemu Command Stream", "submissions");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.queue.pending_words", pendingWords, "Cemu Command Stream", "words");

		static constexpr std::array<const char*, static_cast<size_t>(LatteCommandPacketCategory::Count)> packetCountNames = {
			"cemu.command.host.draw_packets_per_frame",
			"cemu.command.host.context_packets_per_frame",
			"cemu.command.host.resource_packets_per_frame",
			"cemu.command.host.constant_packets_per_frame",
			"cemu.command.host.sampler_packets_per_frame",
			"cemu.command.host.config_packets_per_frame",
			"cemu.command.host.register_other_packets_per_frame",
			"cemu.command.host.indirect_packets_per_frame",
			"cemu.command.host.sync_packets_per_frame",
			"cemu.command.host.surface_packets_per_frame",
			"cemu.command.host.filler_packets_per_frame",
			"cemu.command.host.other_packets_per_frame",
		};
		static constexpr std::array<const char*, static_cast<size_t>(LatteCommandPacketCategory::Count)> packetWordNames = {
			"cemu.command.host.draw_packet_words_per_frame",
			"cemu.command.host.context_packet_words_per_frame",
			"cemu.command.host.resource_packet_words_per_frame",
			"cemu.command.host.constant_packet_words_per_frame",
			"cemu.command.host.sampler_packet_words_per_frame",
			"cemu.command.host.config_packet_words_per_frame",
			"cemu.command.host.register_other_packet_words_per_frame",
			"cemu.command.host.indirect_packet_words_per_frame",
			"cemu.command.host.sync_packet_words_per_frame",
			"cemu.command.host.surface_packet_words_per_frame",
			"cemu.command.host.filler_packet_words_per_frame",
			"cemu.command.host.other_packet_words_per_frame",
		};

		uint64 packetCount = 0;
		uint64 packetWords = 0;
		for (size_t index = 0; index < packetCountNames.size(); ++index)
		{
			const uint64 categoryPackets = Consume(s_commandStreamMetrics.packetCounts[index]);
			const uint64 categoryWords = Consume(s_commandStreamMetrics.packetWords[index]);
			packetCount += categoryPackets;
			packetWords += categoryWords;
			SPATIAL_PROFILER_COUNTER_SET(packetCountNames[index], categoryPackets, "Cemu Command Decode", "packets");
			SPATIAL_PROFILER_COUNTER_SET(packetWordNames[index], categoryWords, "Cemu Command Decode", "words");
		}
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.packets_per_frame", packetCount, "Cemu Command Decode", "packets");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.packet_words_per_frame", packetWords, "Cemu Command Decode", "words");

		static constexpr std::array<const char*, static_cast<size_t>(LatteCommandHostTimeCategory::Count)> hostTimeNames = {
			"cemu.command.host.consume_us_per_frame",
			"cemu.command.host.draw_translate_us_per_frame",
			"cemu.command.host.draw_sequence_begin_us_per_frame",
			"cemu.command.host.draw_sequence_end_us_per_frame",
			"cemu.command.host.sequence.shader_us_per_frame",
			"cemu.command.host.sequence.framebuffer_us_per_frame",
			"cemu.command.host.sequence.textures_us_per_frame",
			"cemu.command.host.sequence.apply_render_target_us_per_frame",
			"cemu.command.host.sequence.viewport_scissor_us_per_frame",
			"cemu.command.host.full_draw.prelude_us_per_frame",
			"cemu.command.host.full_draw.uniforms_us_per_frame",
			"cemu.command.host.full_draw.indices_us_per_frame",
			"cemu.command.host.full_draw.buffers_us_per_frame",
			"cemu.command.host.full_draw.pipeline_us_per_frame",
			"cemu.command.host.full_draw.descriptors_us_per_frame",
			"cemu.command.host.full_draw.host_state_us_per_frame",
			"cemu.command.host.full_draw.api_us_per_frame",
			"cemu.command.host.sequence_end.track_updates_us_per_frame",
			"cemu.command.host.sequence_end.readback_us_per_frame",
			"cemu.command.host.sequence_end.submit_us_per_frame",
		};
		for (size_t index = 0; index < hostTimeNames.size(); ++index)
			SPATIAL_PROFILER_COUNTER_SET(hostTimeNames[index], Consume(s_commandStreamMetrics.hostTimeNs[index]) / 1000, "Cemu Command CPU", "us");

		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.changed_set_register_packets_per_frame", Consume(s_commandStreamMetrics.changedRegisterPackets), "Cemu Command Decode", "packets");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.changed_set_register_packet_words_per_frame", Consume(s_commandStreamMetrics.changedRegisterPacketWords), "Cemu Command Decode", "words");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.redundant_set_register_packets_per_frame", Consume(s_commandStreamMetrics.redundantRegisterPackets), "Cemu Command Decode", "packets");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.redundant_set_register_packet_words_per_frame", Consume(s_commandStreamMetrics.redundantRegisterPacketWords), "Cemu Command Decode", "words");
		const uint64 registerPayloadWords = Consume(s_commandStreamMetrics.registerPayloadWords);
		const uint64 elidedRegisterStoreWords = Consume(s_commandStreamMetrics.elidedRegisterStoreWords);
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.register_payload_words_per_frame", registerPayloadWords, "Cemu Command Decode", "words");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.applied_register_store_words_per_frame",
			registerPayloadWords - std::min(registerPayloadWords, elidedRegisterStoreWords), "Cemu Command Decode", "words");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.elided_register_store_words_per_frame", elidedRegisterStoreWords, "Cemu Command Decode", "words");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.register_store_elision_milli_ratio_per_frame",
			registerPayloadWords == 0 ? 0 : (elidedRegisterStoreWords * 1000) / registerPayloadWords,
			"Cemu Command Decode", "milli_ratio");
		for (size_t categoryIndex = 0; categoryIndex < s_commandStreamMetrics.registerPayloadWordsByCategory.size(); ++categoryIndex)
		{
			const uint64 changedPackets = Consume(s_commandStreamMetrics.changedRegisterPacketsByCategory[categoryIndex]);
			const uint64 redundantPackets = Consume(s_commandStreamMetrics.redundantRegisterPacketsByCategory[categoryIndex]);
			const uint64 payloadWords = Consume(s_commandStreamMetrics.registerPayloadWordsByCategory[categoryIndex]);
			const uint64 elidedStores = Consume(s_commandStreamMetrics.elidedRegisterStoreWordsByCategory[categoryIndex]);
			if (changedPackets != 0)
				s_registerChangedPacketTotals[categoryIndex].fetch_add(changedPackets, std::memory_order_relaxed);
			if (redundantPackets != 0)
				s_registerRedundantPacketTotals[categoryIndex].fetch_add(redundantPackets, std::memory_order_relaxed);
			if (payloadWords != 0)
				s_registerPayloadWordTotals[categoryIndex].fetch_add(payloadWords, std::memory_order_relaxed);
			if (elidedStores != 0)
				s_registerElidedStoreWordTotals[categoryIndex].fetch_add(elidedStores, std::memory_order_relaxed);
		}

		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.draw_passes_per_frame", Consume(s_commandStreamMetrics.drawPasses), "Cemu Command Translate", "passes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.full_draws_per_frame", Consume(s_commandStreamMetrics.fullDraws), "Cemu Command Translate", "draws");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.fast_draws_per_frame", Consume(s_commandStreamMetrics.fastDraws), "Cemu Command Translate", "draws");

		static constexpr std::array<const char*, static_cast<size_t>(LatteDrawPassEndReason::Count)> drawPassEndNames = {
			"cemu.command.host.draw_pass_end_stream_per_frame",
			"cemu.command.host.draw_pass_end_resource_per_frame",
			"cemu.command.host.draw_pass_end_context_per_frame",
			"cemu.command.host.draw_pass_end_sampler_per_frame",
			"cemu.command.host.draw_pass_end_unsupported_per_frame",
			"cemu.command.host.draw_pass_end_streamout_per_frame",
			"cemu.command.host.draw_pass_end_explicit_per_frame",
		};
		for (size_t index = 0; index < drawPassEndNames.size(); ++index)
			SPATIAL_PROFILER_COUNTER_SET(drawPassEndNames[index], Consume(s_commandStreamMetrics.drawPassEndReasons[index]), "Cemu Command Translate", "passes");

		static constexpr std::array<const char*, static_cast<size_t>(LatteVulkanSubmitReason::Count)> submitReasonNames = {
			"threshold", "readback", "query", "idle", "flush", "completion_wait", "frame_boundary",
			"swapchain_acquire", "present", "texture_dump", "swapchain_recreate", "shutdown", "other",
		};
		uint64 vulkanSubmits{};
		std::array<uint64, static_cast<size_t>(LatteVulkanSubmitReason::Count)> submitReasonCounts{};
		for (size_t index = 0; index < submitReasonNames.size(); ++index)
		{
			submitReasonCounts[index] = Consume(s_commandStreamMetrics.vulkanSubmitReasons[index]);
			vulkanSubmits += submitReasonCounts[index];
			const std::string countName = fmt::format("cemu.command.host.vulkan_submit_reason.{}_per_frame", submitReasonNames[index]);
			const std::string cpuName = fmt::format("cemu.command.host.vulkan_submit_reason.{}_us_per_frame", submitReasonNames[index]);
			SPATIAL_PROFILER_COUNTER_SET(countName.c_str(), submitReasonCounts[index], "Cemu Command Submit Reason", "submits");
			SPATIAL_PROFILER_COUNTER_SET(cpuName.c_str(), Consume(s_commandStreamMetrics.vulkanSubmitCpuNs[index]) / 1000, "Cemu Command Submit Reason", "us");
		}
		const uint64 vulkanThresholdSubmits = submitReasonCounts[static_cast<size_t>(LatteVulkanSubmitReason::DrawThreshold)];
		const uint64 vulkanReadbackSubmits = submitReasonCounts[static_cast<size_t>(LatteVulkanSubmitReason::Readback)];
		const uint64 classifiedSubmits = vulkanThresholdSubmits + vulkanReadbackSubmits;
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_submits_per_frame", vulkanSubmits, "Cemu Command Submit", "submits");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_submit_recorded_draw_passes_per_frame", Consume(s_commandStreamMetrics.vulkanSubmitRecordedDrawPasses), "Cemu Command Submit", "passes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_zero_guest_draw_submits_per_frame", Consume(s_commandStreamMetrics.vulkanZeroGuestDrawSubmits), "Cemu Command Submit", "submits");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_threshold_submits_per_frame", vulkanThresholdSubmits, "Cemu Command Submit", "submits");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_readback_submits_per_frame", vulkanReadbackSubmits, "Cemu Command Submit", "submits");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_other_submits_per_frame", vulkanSubmits - std::min(vulkanSubmits, classifiedSubmits), "Cemu Command Submit", "submits");

		static constexpr std::array<const char*, static_cast<size_t>(LatteVulkanRenderPassEndReason::Count)> renderPassReasonNames = {
			"framebuffer_change", "self_dependency", "barrier", "query", "readback", "surface_copy",
			"submit", "imgui", "clear", "present", "texture_operation", "buffer_operation",
			"depth_store_upgrade", "other",
		};
		uint64 vulkanRenderPasses{};
		for (size_t index = 0; index < renderPassReasonNames.size(); ++index)
		{
			const uint64 passCount = Consume(s_commandStreamMetrics.vulkanRenderPassEndReasons[index]);
			vulkanRenderPasses += passCount;
			const std::string countName = fmt::format("cemu.command.host.vulkan_render_pass_end.{}_per_frame", renderPassReasonNames[index]);
			const std::string drawName = fmt::format("cemu.command.host.vulkan_render_pass_end.{}_draws_per_frame", renderPassReasonNames[index]);
			SPATIAL_PROFILER_COUNTER_SET(countName.c_str(), passCount, "Cemu Vulkan Render Pass", "passes");
			SPATIAL_PROFILER_COUNTER_SET(drawName.c_str(), Consume(s_commandStreamMetrics.vulkanRenderPassEndDraws[index]), "Cemu Vulkan Render Pass", "draws");
		}
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_guest_render_passes_per_frame", vulkanRenderPasses, "Cemu Vulkan Render Pass", "passes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_one_draw_render_passes_per_frame", Consume(s_commandStreamMetrics.vulkanOneDrawRenderPasses), "Cemu Vulkan Render Pass", "passes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_at_most_four_draw_render_passes_per_frame", Consume(s_commandStreamMetrics.vulkanAtMostFourDrawRenderPasses), "Cemu Vulkan Render Pass", "passes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_depth_store_omitted_passes_per_frame", Consume(s_commandStreamMetrics.vulkanDepthStoreOmittedPasses), "Cemu Vulkan Render Pass", "passes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_self_dependency.pixel_splits_per_frame", Consume(s_commandStreamMetrics.vulkanPixelSelfDependencySplits), "Cemu Vulkan Render Pass", "passes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vulkan_self_dependency.non_pixel_splits_per_frame", Consume(s_commandStreamMetrics.vulkanNonPixelSelfDependencySplits), "Cemu Vulkan Render Pass", "passes");

		static constexpr std::array<const char*, static_cast<size_t>(LatteBufferCacheUploadSource::Count)> bufferUploadSourceNames = {
			"vertex", "vertex_uniform", "geometry_uniform", "pixel_uniform", "other",
		};
		uint64 bufferUploadCalls{};
		uint64 bufferUploadBytes{};
		for (size_t index = 0; index < bufferUploadSourceNames.size(); ++index)
		{
			const uint64 calls = Consume(s_commandStreamMetrics.bufferCacheUploadCalls[index]);
			const uint64 bytes = Consume(s_commandStreamMetrics.bufferCacheUploadBytes[index]);
			bufferUploadCalls += calls;
			bufferUploadBytes += bytes;
			const std::string callName = fmt::format("cemu.command.host.buffer_cache_upload.{}_calls_per_frame", bufferUploadSourceNames[index]);
			const std::string byteName = fmt::format("cemu.command.host.buffer_cache_upload.{}_bytes_per_frame", bufferUploadSourceNames[index]);
			SPATIAL_PROFILER_COUNTER_SET(callName.c_str(), calls, "Cemu Buffer Cache", "calls");
			SPATIAL_PROFILER_COUNTER_SET(byteName.c_str(), bytes, "Cemu Buffer Cache", "bytes");
		}
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.buffer_cache_upload.calls_per_frame", bufferUploadCalls, "Cemu Buffer Cache", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.buffer_cache_upload.bytes_per_frame", bufferUploadBytes, "Cemu Buffer Cache", "bytes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.buffer_cache_upload.batch_flushes_per_frame", Consume(s_commandStreamMetrics.bufferCacheUploadBatchFlushes), "Cemu Buffer Cache", "batches");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.buffer_cache_upload.batch_regions_per_frame", Consume(s_commandStreamMetrics.bufferCacheUploadBatchRegions), "Cemu Buffer Cache", "regions");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.buffer_cache_upload.copy_commands_per_frame", Consume(s_commandStreamMetrics.bufferCacheUploadBatchCopyCommands), "Cemu Buffer Cache", "commands");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.buffer_cache_copy.calls_per_frame", Consume(s_commandStreamMetrics.bufferCacheCopyCalls), "Cemu Buffer Cache", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.buffer_cache_copy.bytes_per_frame", Consume(s_commandStreamMetrics.bufferCacheCopyBytes), "Cemu Buffer Cache", "bytes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vertex_buffer_bind.calls_per_frame", Consume(s_commandStreamMetrics.vertexBufferBindCalls), "Cemu Buffer Cache", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vertex_buffer_bind.bytes_per_frame", Consume(s_commandStreamMetrics.vertexBufferBindBytes), "Cemu Buffer Cache", "bytes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.vertex_buffer_bind.max_bytes_per_frame", Consume(s_commandStreamMetrics.vertexBufferBindMaxBytes), "Cemu Buffer Cache", "bytes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.uniform_ring_bank_upload.calls_per_frame", Consume(s_commandStreamMetrics.uniformRingBankUploadCalls), "Cemu Buffer Cache", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.uniform_ring_bank_upload.bytes_per_frame", Consume(s_commandStreamMetrics.uniformRingBankUploadBytes), "Cemu Buffer Cache", "bytes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.uniform_ring_bank_bind.calls_per_frame", Consume(s_commandStreamMetrics.uniformRingBankBindCalls), "Cemu Buffer Cache", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.uniform_ring_bank_bind.bytes_per_frame", Consume(s_commandStreamMetrics.uniformRingBankBindBytes), "Cemu Buffer Cache", "bytes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.uniform_ring_bank_reuse.calls_per_frame", Consume(s_commandStreamMetrics.uniformRingBankReuseCalls), "Cemu Buffer Cache", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.uniform_ring_bank_reuse.bytes_per_frame", Consume(s_commandStreamMetrics.uniformRingBankReuseBytes), "Cemu Buffer Cache", "bytes");

		static constexpr std::array<const char*, static_cast<size_t>(LatteDirtyStateDomain::Count)> dirtyClassifiedNames = {
			"cemu.command.host.dirty_classified.texture_words_per_frame",
			"cemu.command.host.dirty_classified.vertex_buffer_words_per_frame",
			"cemu.command.host.dirty_classified.vertex_uniform_buffer_words_per_frame",
			"cemu.command.host.dirty_classified.pixel_uniform_buffer_words_per_frame",
			"cemu.command.host.dirty_classified.geometry_uniform_buffer_words_per_frame",
			"cemu.command.host.dirty_classified.vertex_alu_constant_words_per_frame",
			"cemu.command.host.dirty_classified.pixel_alu_constant_words_per_frame",
			"cemu.command.host.dirty_unclassified_register_words_per_frame",
		};
		static constexpr std::array<const char*, static_cast<size_t>(LatteDirtyStateDomain::Count)> dirtyMarkNames = {
			"cemu.command.host.dirty_mark.texture_per_frame",
			"cemu.command.host.dirty_mark.vertex_buffer_per_frame",
			"cemu.command.host.dirty_mark.vertex_uniform_buffer_per_frame",
			"cemu.command.host.dirty_mark.pixel_uniform_buffer_per_frame",
			"cemu.command.host.dirty_mark.geometry_uniform_buffer_per_frame",
			"cemu.command.host.dirty_mark.vertex_alu_constant_per_frame",
			"cemu.command.host.dirty_mark.pixel_alu_constant_per_frame",
			"cemu.command.host.dirty_mark.unclassified_per_frame",
		};
		static constexpr std::array<const char*, static_cast<size_t>(LatteDirtyStateDomain::Count)> dirtyConsumeNames = {
			"cemu.command.host.dirty_consume.texture_per_frame",
			"cemu.command.host.dirty_consume.vertex_buffer_per_frame",
			"cemu.command.host.dirty_consume.vertex_uniform_buffer_per_frame",
			"cemu.command.host.dirty_consume.pixel_uniform_buffer_per_frame",
			"cemu.command.host.dirty_consume.geometry_uniform_buffer_per_frame",
			"cemu.command.host.dirty_consume.vertex_alu_constant_per_frame",
			"cemu.command.host.dirty_consume.pixel_alu_constant_per_frame",
			"cemu.command.host.dirty_consume.unclassified_per_frame",
		};
		for (size_t index = 0; index < dirtyClassifiedNames.size(); ++index)
		{
			const uint64 classifiedWords = Consume(s_commandStreamMetrics.dirtyClassifiedWords[index]);
			const uint64 marks = Consume(s_commandStreamMetrics.dirtyMarks[index]);
			const uint64 consumes = Consume(s_commandStreamMetrics.dirtyConsumes[index]);
			SPATIAL_PROFILER_COUNTER_SET(dirtyClassifiedNames[index], classifiedWords, "Cemu Dirty State", "words");
			SPATIAL_PROFILER_COUNTER_SET(dirtyMarkNames[index], marks, "Cemu Dirty State", "marks");
			SPATIAL_PROFILER_COUNTER_SET(dirtyConsumeNames[index], consumes, "Cemu Dirty State", "consumes");
			if (classifiedWords != 0)
				s_dirtyClassifiedWordTotals[index].fetch_add(classifiedWords, std::memory_order_relaxed);
			if (marks != 0)
				s_dirtyMarkTotals[index].fetch_add(marks, std::memory_order_relaxed);
			if (consumes != 0)
				s_dirtyConsumeTotals[index].fetch_add(consumes, std::memory_order_relaxed);
		}

		const uint64 pipelineHashCalls = Consume(s_commandStreamMetrics.pipelineHashCalls);
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.pipeline.hash_calls_per_frame", pipelineHashCalls, "Cemu Pipeline Cache", "calls");
		if (pipelineHashCalls != 0)
			s_pipelineHashCallTotal.fetch_add(pipelineHashCalls, std::memory_order_relaxed);
		static constexpr std::array<const char*, static_cast<size_t>(LattePipelineLookupOutcome::Count)> pipelineLookupCounterNames = {
			"cemu.command.host.pipeline.transition_hits_per_frame",
			"cemu.command.host.pipeline.global_hits_per_frame",
			"cemu.command.host.pipeline.misses_per_frame",
		};
		for (size_t index = 0; index < pipelineLookupCounterNames.size(); ++index)
		{
			const uint64 count = Consume(s_commandStreamMetrics.pipelineLookups[index]);
			SPATIAL_PROFILER_COUNTER_SET(pipelineLookupCounterNames[index], count, "Cemu Pipeline Cache", "lookups");
			if (count != 0)
				s_pipelineLookupTotals[index].fetch_add(count, std::memory_order_relaxed);
		}

		const uint64 descriptorSnapshotHits = Consume(s_commandStreamMetrics.descriptorSnapshotHits);
		const uint64 descriptorHashCalls = Consume(s_commandStreamMetrics.descriptorHashCalls);
		const uint64 descriptorCacheHits = Consume(s_commandStreamMetrics.descriptorCacheHits);
		const uint64 descriptorCacheMisses = Consume(s_commandStreamMetrics.descriptorCacheMisses);
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.descriptor.snapshot_hits_per_frame", descriptorSnapshotHits, "Cemu Descriptor Cache", "lookups");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.descriptor.hash_calls_per_frame", descriptorHashCalls, "Cemu Descriptor Cache", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.descriptor.cache_hits_per_frame", descriptorCacheHits, "Cemu Descriptor Cache", "lookups");
		SPATIAL_PROFILER_COUNTER_SET("cemu.command.host.descriptor.cache_misses_per_frame", descriptorCacheMisses, "Cemu Descriptor Cache", "lookups");
		if (descriptorSnapshotHits != 0)
			s_descriptorSnapshotHitTotal.fetch_add(descriptorSnapshotHits, std::memory_order_relaxed);
		if (descriptorHashCalls != 0)
			s_descriptorHashCallTotal.fetch_add(descriptorHashCalls, std::memory_order_relaxed);
		if (descriptorCacheHits != 0)
			s_descriptorCacheHitTotal.fetch_add(descriptorCacheHits, std::memory_order_relaxed);
		if (descriptorCacheMisses != 0)
			s_descriptorCacheMissTotal.fetch_add(descriptorCacheMisses, std::memory_order_relaxed);

		static constexpr std::array<const char*, static_cast<size_t>(LatteDynamicState::Count)> dynamicRequestNames = {
			"cemu.command.host.dynamic_state.blend_constants.requested_per_frame",
			"cemu.command.host.dynamic_state.depth_bias.requested_per_frame",
		};
		static constexpr std::array<const char*, static_cast<size_t>(LatteDynamicState::Count)> dynamicEmitNames = {
			"cemu.command.host.dynamic_state.blend_constants.emitted_per_frame",
			"cemu.command.host.dynamic_state.depth_bias.emitted_per_frame",
		};
		static constexpr std::array<const char*, static_cast<size_t>(LatteDynamicState::Count)> dynamicElideNames = {
			"cemu.command.host.dynamic_state.blend_constants.elided_per_frame",
			"cemu.command.host.dynamic_state.depth_bias.elided_per_frame",
		};
		for (size_t index = 0; index < dynamicRequestNames.size(); ++index)
		{
			const uint64 requests = Consume(s_commandStreamMetrics.dynamicStateRequests[index]);
			const uint64 emits = Consume(s_commandStreamMetrics.dynamicStateEmits[index]);
			const uint64 elided = requests - std::min(requests, emits);
			SPATIAL_PROFILER_COUNTER_SET(dynamicRequestNames[index], requests, "Cemu Dynamic State", "requests");
			SPATIAL_PROFILER_COUNTER_SET(dynamicEmitNames[index], emits, "Cemu Dynamic State", "commands");
			SPATIAL_PROFILER_COUNTER_SET(dynamicElideNames[index], elided, "Cemu Dynamic State", "commands");
			if (requests != 0)
				s_dynamicStateRequestTotals[index].fetch_add(requests, std::memory_order_relaxed);
			if (emits != 0)
				s_dynamicStateEmitTotals[index].fetch_add(emits, std::memory_order_relaxed);
		}
	}
}

void LattePerformanceMonitor_recordGuestCommandSubmission(uint32 words)
{
	s_commandStreamMetrics.guestSubmissions.fetch_add(1, std::memory_order_relaxed);
	s_commandStreamMetrics.guestWords.fetch_add(words, std::memory_order_relaxed);
	s_commandStreamMetrics.guestSubmissionsTotal.fetch_add(1, std::memory_order_relaxed);
	s_commandStreamMetrics.guestWordsTotal.fetch_add(words, std::memory_order_relaxed);
}

void LattePerformanceMonitor_recordHostCommandSubmission(uint32 words)
{
	s_commandStreamMetrics.hostSubmissions++;
	s_commandStreamMetrics.hostWords += words;
	s_commandStreamMetrics.hostSubmissionsTotal++;
	s_commandStreamMetrics.hostWordsTotal += words;
}

void LattePerformanceMonitor_recordHostCommandPacket(LatteCommandPacketCategory category, uint32 words)
{
	const size_t index = static_cast<size_t>(category);
	if (index >= s_commandStreamMetrics.packetCounts.size())
		return;
	s_commandStreamMetrics.packetCounts[index]++;
	s_commandStreamMetrics.packetWords[index] += words;
}

void LattePerformanceMonitor_recordHostCommandTime(LatteCommandHostTimeCategory category, uint64 nanoseconds)
{
	const size_t index = static_cast<size_t>(category);
	if (index >= s_commandStreamMetrics.hostTimeNs.size())
		return;
	s_commandStreamMetrics.hostTimeNs[index] += nanoseconds;
}

void LattePerformanceMonitor_recordHostRegisterPacketOutcome(LatteCommandPacketCategory category,
	bool changed, uint32 words, uint32 elidedRegisterStores)
{
	const size_t categoryIndex = static_cast<size_t>(category);
	if (categoryIndex >= s_registerChangedPacketTotals.size() || GetRegisterDomainName(category) == nullptr)
		return;

	const uint32 payloadWords = words >= 2 ? words - 2 : 0;
	elidedRegisterStores = std::min(elidedRegisterStores, payloadWords);
	if (changed)
	{
		s_commandStreamMetrics.changedRegisterPackets++;
		s_commandStreamMetrics.changedRegisterPacketWords += words;
		s_commandStreamMetrics.changedRegisterPacketsByCategory[categoryIndex]++;
	}
	else
	{
		s_commandStreamMetrics.redundantRegisterPackets++;
		s_commandStreamMetrics.redundantRegisterPacketWords += words;
		s_commandStreamMetrics.redundantRegisterPacketsByCategory[categoryIndex]++;
	}
	s_commandStreamMetrics.registerPayloadWords += payloadWords;
	s_commandStreamMetrics.elidedRegisterStoreWords += elidedRegisterStores;
	s_commandStreamMetrics.registerPayloadWordsByCategory[categoryIndex] += payloadWords;
	s_commandStreamMetrics.elidedRegisterStoreWordsByCategory[categoryIndex] += elidedRegisterStores;
}

void LattePerformanceMonitor_recordHostDrawPass()
{
	s_commandStreamMetrics.drawPasses++;
}

void LattePerformanceMonitor_recordHostDraw(bool fastDraw)
{
	if (fastDraw)
		s_commandStreamMetrics.fastDraws++;
	else
		s_commandStreamMetrics.fullDraws++;
}

void LattePerformanceMonitor_recordHostDrawPassEnd(LatteDrawPassEndReason reason)
{
	const size_t index = static_cast<size_t>(reason);
	if (index >= s_commandStreamMetrics.drawPassEndReasons.size())
		return;
	s_commandStreamMetrics.drawPassEndReasons[index]++;
}

void LattePerformanceMonitor_recordHostContextDrawPassBreak(uint32 registerStart, uint32 registerEnd)
{
	if (registerStart >= s_contextDrawPassBreakCounts.size())
		return;
	s_contextDrawPassBreakCounts[registerStart].fetch_add(1, std::memory_order_relaxed);
	s_contextDrawPassBreakEnds[registerStart].store(registerEnd, std::memory_order_relaxed);
	s_contextDrawPassBreakTotal.fetch_add(1, std::memory_order_relaxed);
}

void LattePerformanceMonitor_recordHostVulkanSubmit(LatteVulkanSubmitReason reason,
	uint32 recordedDrawPasses, uint64 cpuNanoseconds)
{
	LatteFrameGraphShadow::RecordActualSubmit();
	const size_t index = static_cast<size_t>(reason);
	if (index >= s_commandStreamMetrics.vulkanSubmitReasons.size())
		return;
	s_commandStreamMetrics.vulkanSubmitReasons[index]++;
	s_commandStreamMetrics.vulkanSubmitCpuNs[index] += cpuNanoseconds;
	s_commandStreamMetrics.vulkanSubmitRecordedDrawPasses += recordedDrawPasses;
	s_commandStreamMetrics.vulkanZeroGuestDrawSubmits += recordedDrawPasses == 0;
}

void LattePerformanceMonitor_recordHostVulkanRenderPassEnd(LatteVulkanRenderPassEndReason reason,
	uint32 drawCount)
{
	LatteFrameGraphShadow::RecordActualRenderPass();
	const size_t index = static_cast<size_t>(reason);
	if (index >= s_commandStreamMetrics.vulkanRenderPassEndReasons.size())
		return;
	s_commandStreamMetrics.vulkanRenderPassEndReasons[index]++;
	s_commandStreamMetrics.vulkanRenderPassEndDraws[index] += drawCount;
	s_commandStreamMetrics.vulkanOneDrawRenderPasses += drawCount == 1;
	s_commandStreamMetrics.vulkanAtMostFourDrawRenderPasses += drawCount <= 4;
}

void LattePerformanceMonitor_recordHostVulkanDepthStoreOmittedPass()
{
	s_commandStreamMetrics.vulkanDepthStoreOmittedPasses++;
}

void LattePerformanceMonitor_recordHostVulkanSelfDependencySplit(bool hasNonPixelDependency)
{
	if (hasNonPixelDependency)
		s_commandStreamMetrics.vulkanNonPixelSelfDependencySplits++;
	else
		s_commandStreamMetrics.vulkanPixelSelfDependencySplits++;
}

void LattePerformanceMonitor_recordHostBufferCacheUpload(LatteBufferCacheUploadSource source,
	uint32 bytes)
{
	const size_t index = static_cast<size_t>(source);
	if (index >= s_commandStreamMetrics.bufferCacheUploadCalls.size())
		return;
	s_commandStreamMetrics.bufferCacheUploadCalls[index]++;
	s_commandStreamMetrics.bufferCacheUploadBytes[index] += bytes;
}

void LattePerformanceMonitor_recordHostBufferCacheUploadBatch(uint32 regions,
	uint32 copyCommands)
{
	s_commandStreamMetrics.bufferCacheUploadBatchFlushes++;
	s_commandStreamMetrics.bufferCacheUploadBatchRegions += regions;
	s_commandStreamMetrics.bufferCacheUploadBatchCopyCommands += copyCommands;
}

void LattePerformanceMonitor_recordHostBufferCacheCopy(uint32 bytes)
{
	s_commandStreamMetrics.bufferCacheCopyCalls++;
	s_commandStreamMetrics.bufferCacheCopyBytes += bytes;
}

void LattePerformanceMonitor_recordHostVertexBufferBind(uint32 bytes)
{
	s_commandStreamMetrics.vertexBufferBindCalls++;
	s_commandStreamMetrics.vertexBufferBindBytes += bytes;
	s_commandStreamMetrics.vertexBufferBindMaxBytes =
		std::max<uint64>(s_commandStreamMetrics.vertexBufferBindMaxBytes, bytes);
}

void LattePerformanceMonitor_recordHostUniformRingBankUpload(uint32 bytes)
{
	s_commandStreamMetrics.uniformRingBankUploadCalls++;
	s_commandStreamMetrics.uniformRingBankUploadBytes += bytes;
}

void LattePerformanceMonitor_recordHostUniformRingBankBind(uint32 bytes, bool reused)
{
	s_commandStreamMetrics.uniformRingBankBindCalls++;
	s_commandStreamMetrics.uniformRingBankBindBytes += bytes;
	if (reused)
	{
		s_commandStreamMetrics.uniformRingBankReuseCalls++;
		s_commandStreamMetrics.uniformRingBankReuseBytes += bytes;
	}
}

void LattePerformanceMonitor_recordHostDirtyRegisterClassification(LatteDirtyStateDomain domain,
	uint32 words)
{
	const size_t index = static_cast<size_t>(domain);
	if (index >= s_commandStreamMetrics.dirtyClassifiedWords.size())
		return;
	s_commandStreamMetrics.dirtyClassifiedWords[index] += words;
}

void LattePerformanceMonitor_recordHostDirtyStateMark(LatteDirtyStateDomain domain, uint32 count)
{
	const size_t index = static_cast<size_t>(domain);
	if (index >= s_commandStreamMetrics.dirtyMarks.size())
		return;
	s_commandStreamMetrics.dirtyMarks[index] += count;
}

void LattePerformanceMonitor_recordHostDirtyStateConsume(LatteDirtyStateDomain domain, uint32 count)
{
	const size_t index = static_cast<size_t>(domain);
	if (index >= s_commandStreamMetrics.dirtyConsumes.size())
		return;
	s_commandStreamMetrics.dirtyConsumes[index] += count;
}

void LattePerformanceMonitor_recordHostPipelineHashCall()
{
	s_commandStreamMetrics.pipelineHashCalls++;
}

void LattePerformanceMonitor_recordHostPipelineLookup(LattePipelineLookupOutcome outcome)
{
	const size_t index = static_cast<size_t>(outcome);
	if (index >= s_commandStreamMetrics.pipelineLookups.size())
		return;
	s_commandStreamMetrics.pipelineLookups[index]++;
}

void LattePerformanceMonitor_recordHostDescriptorSnapshotHit()
{
	s_commandStreamMetrics.descriptorSnapshotHits++;
}

void LattePerformanceMonitor_recordHostDescriptorLookup(bool cacheHit)
{
	s_commandStreamMetrics.descriptorHashCalls++;
	if (cacheHit)
		s_commandStreamMetrics.descriptorCacheHits++;
	else
		s_commandStreamMetrics.descriptorCacheMisses++;
}

void LattePerformanceMonitor_recordHostDynamicState(LatteDynamicState state, bool emitted)
{
	const size_t index = static_cast<size_t>(state);
	if (index >= s_commandStreamMetrics.dynamicStateRequests.size())
		return;
	s_commandStreamMetrics.dynamicStateRequests[index]++;
	if (emitted)
		s_commandStreamMetrics.dynamicStateEmits[index]++;
}

std::string LattePerformanceMonitor_getCommandTranslationStatus()
{
	struct ContextBreakEntry
	{
		uint32 registerStart;
		uint32 registerEnd;
		uint64 count;
	};

	std::vector<ContextBreakEntry> entries;
	for (uint32 registerStart = 0; registerStart < s_contextDrawPassBreakCounts.size(); ++registerStart)
	{
		const uint64 count = s_contextDrawPassBreakCounts[registerStart].load(std::memory_order_relaxed);
		if (count == 0)
			continue;
		entries.push_back({
			registerStart,
			s_contextDrawPassBreakEnds[registerStart].load(std::memory_order_relaxed),
			count,
		});
	}
	std::sort(entries.begin(), entries.end(), [](const ContextBreakEntry& lhs, const ContextBreakEntry& rhs) {
		if (lhs.count != rhs.count)
			return lhs.count > rhs.count;
		return lhs.registerStart < rhs.registerStart;
	});

	std::ostringstream out;
	out << "command_translation_status:\n";
	uint64 registerChangedPackets = 0;
	uint64 registerRedundantPackets = 0;
	uint64 registerPayloadWords = 0;
	uint64 registerElidedStoreWords = 0;
	for (size_t categoryIndex = 0; categoryIndex < s_registerPayloadWordTotals.size(); ++categoryIndex)
	{
		const auto category = static_cast<LatteCommandPacketCategory>(categoryIndex);
		if (GetRegisterDomainName(category) == nullptr)
			continue;
		registerChangedPackets += s_registerChangedPacketTotals[categoryIndex].load(std::memory_order_relaxed);
		registerRedundantPackets += s_registerRedundantPacketTotals[categoryIndex].load(std::memory_order_relaxed);
		registerPayloadWords += s_registerPayloadWordTotals[categoryIndex].load(std::memory_order_relaxed);
		registerElidedStoreWords += s_registerElidedStoreWordTotals[categoryIndex].load(std::memory_order_relaxed);
	}
	const uint64 registerAppliedStoreWords = registerPayloadWords - std::min(registerPayloadWords, registerElidedStoreWords);
	out << "register_changed_packets=" << registerChangedPackets << "\n";
	out << "register_redundant_packets=" << registerRedundantPackets << "\n";
	out << "register_payload_words=" << registerPayloadWords << "\n";
	out << "register_applied_store_words=" << registerAppliedStoreWords << "\n";
	out << "register_elided_store_words=" << registerElidedStoreWords << "\n";
	out << "register_store_elision_milli_ratio=";
	out << (registerPayloadWords == 0 ? 0 : (registerElidedStoreWords * 1000) / registerPayloadWords) << "\n";
	for (size_t categoryIndex = 0; categoryIndex < s_registerPayloadWordTotals.size(); ++categoryIndex)
	{
		const auto category = static_cast<LatteCommandPacketCategory>(categoryIndex);
		const char* domainName = GetRegisterDomainName(category);
		if (domainName == nullptr)
			continue;
		const uint64 payloadWords = s_registerPayloadWordTotals[categoryIndex].load(std::memory_order_relaxed);
		const uint64 elidedStoreWords = s_registerElidedStoreWordTotals[categoryIndex].load(std::memory_order_relaxed);
		const uint64 appliedStoreWords = payloadWords - std::min(payloadWords, elidedStoreWords);
		out << "register_domain." << domainName;
		out << "=changed_packets:" << s_registerChangedPacketTotals[categoryIndex].load(std::memory_order_relaxed);
		out << ",redundant_packets:" << s_registerRedundantPacketTotals[categoryIndex].load(std::memory_order_relaxed);
		out << ",payload_words:" << payloadWords;
		out << ",applied_store_words:" << appliedStoreWords;
		out << ",elided_store_words:" << elidedStoreWords << "\n";
	}
	for (size_t index = 0; index < s_dirtyDomainNames.size(); ++index)
	{
		out << "dirty_domain." << s_dirtyDomainNames[index];
		out << "=classified_words:" << s_dirtyClassifiedWordTotals[index].load(std::memory_order_relaxed);
		out << ",marks:" << s_dirtyMarkTotals[index].load(std::memory_order_relaxed);
		out << ",consumes:" << s_dirtyConsumeTotals[index].load(std::memory_order_relaxed) << "\n";
	}
	out << "pipeline_hash_calls=" << s_pipelineHashCallTotal.load(std::memory_order_relaxed) << "\n";
	for (size_t index = 0; index < s_pipelineLookupNames.size(); ++index)
		out << "pipeline_lookup." << s_pipelineLookupNames[index] << "=" << s_pipelineLookupTotals[index].load(std::memory_order_relaxed) << "\n";
	out << "descriptor_snapshot_hits=" << s_descriptorSnapshotHitTotal.load(std::memory_order_relaxed) << "\n";
	out << "descriptor_hash_calls=" << s_descriptorHashCallTotal.load(std::memory_order_relaxed) << "\n";
	out << "descriptor_cache_hits=" << s_descriptorCacheHitTotal.load(std::memory_order_relaxed) << "\n";
	out << "descriptor_cache_misses=" << s_descriptorCacheMissTotal.load(std::memory_order_relaxed) << "\n";
	for (size_t index = 0; index < s_dynamicStateNames.size(); ++index)
	{
		const uint64 requests = s_dynamicStateRequestTotals[index].load(std::memory_order_relaxed);
		const uint64 emits = s_dynamicStateEmitTotals[index].load(std::memory_order_relaxed);
		out << "dynamic_state." << s_dynamicStateNames[index];
		out << "=requested:" << requests;
		out << ",emitted:" << emits;
		out << ",elided:" << requests - std::min(requests, emits) << "\n";
	}
	out << "context_break_total=" << s_contextDrawPassBreakTotal.load(std::memory_order_relaxed) << "\n";
	out << "context_break_unique_starts=" << entries.size() << "\n";
	const size_t topCount = std::min<size_t>(entries.size(), 16);
	out << "context_break_top_count=" << topCount << "\n";
	for (size_t index = 0; index < topCount; ++index)
	{
		const auto& entry = entries[index];
		out << "context_break[" << index << "]=";
		out << "start=0x" << std::hex << entry.registerStart;
		out << " end=0x" << entry.registerEnd;
		out << std::dec << " count=" << entry.count << "\n";
	}
	return out.str();
}

void LattePerformanceMonitor_resetCommandTranslationStatus()
{
	for (auto& count : s_contextDrawPassBreakCounts)
		count.store(0, std::memory_order_relaxed);
	for (auto& registerEnd : s_contextDrawPassBreakEnds)
		registerEnd.store(0, std::memory_order_relaxed);
	s_contextDrawPassBreakTotal.store(0, std::memory_order_relaxed);
	for (auto& count : s_registerChangedPacketTotals)
		count.store(0, std::memory_order_relaxed);
	for (auto& count : s_registerRedundantPacketTotals)
		count.store(0, std::memory_order_relaxed);
	for (auto& words : s_registerPayloadWordTotals)
		words.store(0, std::memory_order_relaxed);
	for (auto& words : s_registerElidedStoreWordTotals)
		words.store(0, std::memory_order_relaxed);
	for (auto& words : s_dirtyClassifiedWordTotals)
		words.store(0, std::memory_order_relaxed);
	for (auto& count : s_dirtyMarkTotals)
		count.store(0, std::memory_order_relaxed);
	for (auto& count : s_dirtyConsumeTotals)
		count.store(0, std::memory_order_relaxed);
	s_pipelineHashCallTotal.store(0, std::memory_order_relaxed);
	for (auto& count : s_pipelineLookupTotals)
		count.store(0, std::memory_order_relaxed);
	s_descriptorSnapshotHitTotal.store(0, std::memory_order_relaxed);
	s_descriptorHashCallTotal.store(0, std::memory_order_relaxed);
	s_descriptorCacheHitTotal.store(0, std::memory_order_relaxed);
	s_descriptorCacheMissTotal.store(0, std::memory_order_relaxed);
	for (auto& count : s_dynamicStateRequestTotals)
		count.store(0, std::memory_order_relaxed);
	for (auto& count : s_dynamicStateEmitTotals)
		count.store(0, std::memory_order_relaxed);
}

void LattePerformanceMonitor_frameEnd()
{
	SPATIAL_PROFILER_AUTO_SCOPE_NAME("latte.frame.end");
	LatteFrameGraphShadow::EndFrame();
	SPATIAL_PROFILER_FRAME_END();
	const auto guestCounters = coreinit::ConsumeGuestExecutionProfilerCounters();
	SPATIAL_PROFILER_COUNTER_SET("cemu.guest_quanta_per_frame", guestCounters.quanta, "Cemu Guest", "quanta");
	SPATIAL_PROFILER_COUNTER_SET("cemu.guest_jit_entries_per_frame", guestCounters.jitEntries, "Cemu Guest", "entries");
	SPATIAL_PROFILER_COUNTER_SET("cemu.guest_interpreter_instructions_per_frame", guestCounters.interpreterInstructions, "Cemu Guest", "instructions");
	PublishCommandStreamMetrics();
	// per-frame stats
	performanceMonitor.gpuTime_shaderCreate.frameFinished();
	performanceMonitor.gpuTime_frameTime.frameFinished();
	performanceMonitor.gpuTime_idleTime.frameFinished();
	performanceMonitor.gpuTime_fenceTime.frameFinished();

	performanceMonitor.gpuTime_dcStageTextures.frameFinished();
	performanceMonitor.gpuTime_dcStageVertexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageShaderAndUniformMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageIndexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageMRT.frameFinished();
	performanceMonitor.gpuTime_dcStageDrawcallAPI.frameFinished();
	performanceMonitor.gpuTime_waitForAsync.frameFinished();

	uint32 elapsedTime = GetTickCount() - performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate;
	if (elapsedTime >= 1000)
	{
		bool isFirstUpdate = performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate == 0;
		// sum up raw stats
		uint32 totalElapsedTime = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 totalElapsedTimeFPS = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 elapsedFrames = 0;
		uint32 elapsedFrames2S = 0; // elapsed frames for last two entries (seconds)
		uint64 skippedCycles = 0;
		uint64 vertexDataUploaded = 0;
		uint64 vertexDataCached = 0;
		uint64 uniformBankUploadedData = 0;
		uint64 uniformBankUploadedCount = 0;
		uint64 indexDataUploaded = 0;
		uint64 indexDataCached = 0;
		uint32 frameCounter = 0;
		uint32 drawCallCounter = 0;
		uint32 fastDrawCallCounter = 0;
		uint32 shaderBindCounter = 0;
		uint32 recompilerLeaveCount = 0;
		uint32 threadLeaveCount = 0;
		for (sint32 i = 0; i < PERFORMANCE_MONITOR_TRACK_CYCLES; i++)
		{
			elapsedFrames += performanceMonitor.cycle[i].frameCounter;
			skippedCycles += performanceMonitor.cycle[i].skippedCycles;
			vertexDataUploaded += performanceMonitor.cycle[i].vertexDataUploaded;
			vertexDataCached += performanceMonitor.cycle[i].vertexDataCached;
			uniformBankUploadedData += performanceMonitor.cycle[i].uniformBankUploadedData;
			uniformBankUploadedCount += performanceMonitor.cycle[i].uniformBankUploadedCount;
			indexDataUploaded += performanceMonitor.cycle[i].indexDataUploaded;
			indexDataCached += performanceMonitor.cycle[i].indexDataCached;
			frameCounter += performanceMonitor.cycle[i].frameCounter;
			drawCallCounter += performanceMonitor.cycle[i].drawCallCounter;
			fastDrawCallCounter += performanceMonitor.cycle[i].fastDrawCallCounter;
			shaderBindCounter += performanceMonitor.cycle[i].shaderBindCount;
			recompilerLeaveCount += performanceMonitor.cycle[i].recompilerLeaveCount;
			threadLeaveCount += performanceMonitor.cycle[i].threadLeaveCount;
		}
		elapsedFrames = std::max<uint32>(elapsedFrames, 1);
		elapsedFrames2S = performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 0) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S += performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S = std::max<uint32>(elapsedFrames2S, 1);
		// calculate stats
		uint64 passedCycles = PPCInterpreter_getMainCoreCycleCounter() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastCycleCount;
		passedCycles -= skippedCycles;
		uint64 vertexDataUploadPerFrame = (vertexDataUploaded / (uint64)elapsedFrames);
		vertexDataUploadPerFrame /= 1024ULL;
		uint64 vertexDataCachedPerFrame = (vertexDataCached / (uint64)elapsedFrames);
		vertexDataCachedPerFrame /= 1024ULL;
		uint64 uniformBankDataUploadedPerFrame = (uniformBankUploadedData / (uint64)elapsedFrames);
		uniformBankDataUploadedPerFrame /= 1024ULL;
		uint32 uniformBankCountUploadedPerFrame = (uint32)(uniformBankUploadedCount / (uint64)elapsedFrames);
		uint64 indexDataUploadPerFrame = (indexDataUploaded / (uint64)elapsedFrames);

		double fps = (double)elapsedFrames2S * 1000.0 / (double)totalElapsedTimeFPS;
		uint32 shaderBindsPerFrame = shaderBindCounter / elapsedFrames;
		passedCycles = passedCycles * 1000ULL / totalElapsedTime;
		uint32 rlps = (uint32)((uint64)recompilerLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		uint32 tlps = (uint32)((uint64)threadLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		// set stats
		performanceMonitor.stats.indexDataUploadPerFrame = indexDataUploadPerFrame;
		// next counter cycle
		sint32 nextCycleIndex = (performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES;
		performanceMonitor.cycle[nextCycleIndex].drawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].fastDrawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].frameCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].shaderBindCount = 0;
		performanceMonitor.cycle[nextCycleIndex].lastCycleCount = PPCInterpreter_getMainCoreCycleCounter();
		performanceMonitor.cycle[nextCycleIndex].skippedCycles = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedData = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedCount = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].recompilerLeaveCount = 0;
		performanceMonitor.cycle[nextCycleIndex].threadLeaveCount = 0;
		performanceMonitor.cycleIndex = nextCycleIndex;

		// next update in 1 second
		performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate = GetTickCount();

		if (isFirstUpdate)
		{
			LatteOverlay_updateStats(0.0, 0, 0);
			WindowSystem::UpdateWindowTitles(false, false, 0.0);
		}
		else
		{
			const uint32 drawsPerFrame = drawCallCounter / elapsedFrames;
			const uint32 fastDrawsPerFrame = fastDrawCallCounter / elapsedFrames;
			const uint32 fullDrawsPerFrame = drawsPerFrame - std::min(drawsPerFrame, fastDrawsPerFrame);
			LatteOverlay_updateStats(fps, drawsPerFrame, fastDrawsPerFrame);
			WindowSystem::UpdateWindowTitles(false, false, fps);
			SPATIAL_PROFILER_COUNTER_SET("cemu.fps_milli", static_cast<std::int64_t>(fps * 1000.0), "Cemu Frame", "milli-fps");
			if (fps > 0.0)
				SPATIAL_PROFILER_COUNTER_SET("cemu.frame_time_us", static_cast<std::int64_t>(1000000.0 / fps), "Cemu Frame", "us");
			SPATIAL_PROFILER_COUNTER_SET("cemu.draws_per_frame", drawsPerFrame, "Cemu GPU", "draws");
			SPATIAL_PROFILER_COUNTER_SET("cemu.full_draws_per_frame", fullDrawsPerFrame, "Cemu GPU", "draws");
			SPATIAL_PROFILER_COUNTER_SET("cemu.fast_draws_per_frame", fastDrawsPerFrame, "Cemu GPU", "draws");
			SPATIAL_PROFILER_COUNTER_SET("cemu.fast_draw_ratio_milli", drawsPerFrame == 0 ? 0 : (fastDrawsPerFrame * 1000ULL) / drawsPerFrame, "Cemu GPU", "milli-ratio");
		}
	}
}

void LattePerformanceMonitor_frameBegin()
{
	SPATIAL_PROFILER_FRAME_START();
	LatteFrameGraphShadow::BeginFrame();
	performanceMonitor.vk.numDrawBarriersPerFrame.reset();
	performanceMonitor.vk.numBeginRenderpassPerFrame.reset();
}
