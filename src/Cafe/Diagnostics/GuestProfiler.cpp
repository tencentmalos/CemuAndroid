#include "Cafe/Diagnostics/GuestProfiler.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "Cafe/OS/RPL/rpl.h"

#include "spatial/debugbus/DebugCommandRegistry.h"
#include "spatial/profiler/Profiler.h"

namespace
{
	constexpr size_t kSectionCount = 40;
	constexpr size_t kMaxNestedSpansPerSection = 16;
	constexpr size_t kMaxNestedGpuTags = 32;
	constexpr uint32 kInvalidGpuTagSection = UINT32_MAX;
	constexpr uint32 kGpuTagProfilerThreadId = 0xC0000001u;

	constexpr std::array<std::string_view, kSectionCount> kSectionNames{
		"RoomscaleResolve",
		"RoomscaleBeginHook",
		"WeaponHandHook",
		"WeaponAttackHook",
		"XRUpdateSpaces",
		"XRUpdateActions",
		"XRLocateViews",
		"Layer3DRender",
		"Layer2DRender",
		"ImGuiUpdate",
		"ImGuiRender",
		"ImGuiDrawAndCopy",
		"PPCSystemPreCalc",
		"PPCSystemStateMachine",
		"PPCSystemPostCalc",
		"PPCCalcPlacementMgr",
		"PPCPhysicsPostBgBaseProcMgr",
		"PPCActorUpdateJobs",
		"PPCGraphicsCalc",
		"PPCSystemTaskPreCalc",
		"PPCSystemTaskPostCalc",
		"PPCSystemTaskDrawTV",
		"PPCSystemTaskDrawDRC",
		"PPCSystemTaskPostDrawTV",
		"PPCSystemTaskPostDrawDRC",
		"PPCLayer3DDraw",
		"PPCLayer3DCalcView",
		"PPCLayer3DCalcViewGPU",
		"PPCLayer3DDrawBG",
		"PPCLayer3DDrawOpaque",
		"PPCLayer3DDrawXlu",
		"PPCLayer3DDrawPostEffects",
		"PPCLayer3DDrawFinalImage",
		"PPCActorJob0_1",
		"PPCActorJob0_2",
		"PPCActorJob1_1",
		"PPCActorJob1_2",
		"PPCActorJob2_1Ragdoll",
		"PPCActorJob2_2",
		"PPCActorJob4",
	};

	struct SectionStats
	{
		std::atomic<uint64> lastNs{};
		std::atomic<uint64> maxNs{};
		std::atomic<uint64> totalNs{};
		std::atomic<uint64> callCount{};
		std::atomic<uint64> gx2SubmissionCount{};
		std::atomic<uint64> gx2SubmittedWords{};
		std::atomic<uint32> lastGx2GuestLr{};
		std::atomic<uint64> maxGx2SubmissionWords{};
		std::atomic<uint32> maxGx2SubmissionGuestLr{};
		std::atomic<uint64> gpuFenceCount{};
		std::atomic<uint32> lastGpuFenceGuestLr{};
		std::atomic<uint64> gx2DrawDoneCount{};
		std::atomic<uint32> lastGx2DrawDoneGuestLr{};
		std::atomic<uint64> gx2SwapScanBuffersCount{};
		std::atomic<uint32> lastGx2SwapScanBuffersGuestLr{};
		std::atomic<uint64> gpuTagEmittedBeginCount{};
		std::atomic<uint64> gpuTagEmittedEndCount{};
		std::atomic<uint64> gpuTagConsumedBeginCount{};
		std::atomic<uint64> gpuTagConsumedEndCount{};
		std::atomic<uint64> gpuTagHostDurationNs{};
		std::atomic<uint64> gpuTagDrawCount{};
		std::atomic<uint64> gpuTagFastDrawCount{};
		std::atomic<uint64> gpuTagDrawFragmentCount{};
	};

	struct ActiveSpan
	{
		uint64 startNs{};
		uint32 generation{};
		spatial::profiler::ProfilerTagToken profilerTagToken{
			spatial::profiler::InvalidProfilerTagToken};
	};

	struct ActiveGpuTag
	{
		uint32 sectionId{};
		uint32 guestThreadId{};
		uint32 guestLr{};
		uint32 generation{};
		uint64 startNs{};
		spatial::profiler::ProfilerTagToken profilerTagToken{
			spatial::profiler::InvalidProfilerTagToken};
	};

	std::array<SectionStats, kSectionCount> s_stats;
	std::unordered_map<uint64, std::vector<ActiveSpan>> s_activeSpans;
	std::mutex s_activeSpansMutex;
	std::atomic_bool s_enabled{true};
	std::atomic<uint32> s_generation{1};
	std::atomic<uint64> s_invalidSectionCount{};
	std::atomic<uint64> s_unmatchedEndCount{};
	std::atomic<uint64> s_spanOverflowCount{};
	std::atomic<uint64> s_timelineTagBeginCount{};
	std::atomic<uint64> s_timelineTagEndCount{};
	std::atomic<uint64> s_gx2SubmissionCount{};
	std::atomic<uint64> s_gx2SubmittedWords{};
	std::atomic<uint64> s_unattributedGx2SubmissionCount{};
	std::atomic<uint64> s_unattributedGx2SubmittedWords{};
	std::atomic<uint64> s_maxUnattributedGx2SubmissionWords{};
	std::atomic<uint32> s_maxUnattributedGx2SubmissionGuestLr{};
	std::atomic<uint64> s_gpuFenceCount{};
	std::atomic<uint64> s_unattributedGpuFenceCount{};
	std::atomic<uint32> s_lastUnattributedGpuFenceGuestLr{};
	std::atomic<uint64> s_gx2DrawDoneCount{};
	std::atomic<uint64> s_unattributedGx2DrawDoneCount{};
	std::atomic<uint32> s_lastUnattributedGx2DrawDoneGuestLr{};
	std::atomic<uint64> s_gx2SwapScanBuffersCount{};
	std::atomic<uint64> s_unattributedGx2SwapScanBuffersCount{};
	std::atomic<uint32> s_lastUnattributedGx2SwapScanBuffersGuestLr{};
	std::unordered_set<uint32> s_namedGuestThreads;
	std::vector<ActiveGpuTag> s_activeGpuTags;
	std::mutex s_activeGpuTagsMutex;
	std::atomic<uint32> s_activeGpuTagSection{kInvalidGpuTagSection};
	std::atomic<uint64> s_gpuTagEmitNoCommandBufferCount{};
	std::atomic<uint64> s_gpuTagEmitDisplayListSkipCount{};
	std::atomic<uint64> s_gpuTagInvalidSectionCount{};
	std::atomic<uint64> s_gpuTagStackOverflowCount{};
	std::atomic<uint64> s_gpuTagUnmatchedEndCount{};
	std::atomic<uint64> s_gpuTagStalePacketCount{};

	const std::array<std::string, kSectionCount> s_lastCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.last_us", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_maxCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.max_us", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_callCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.calls", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_gx2SubmitCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.gx2_submits", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_gx2WordCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.gx2_words", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_gx2GuestLrCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.gx2_last_lr", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_gpuFenceCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.gpu_fences", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_gpuFenceGuestLrCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.gpu_fence_last_lr", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_gx2DrawDoneCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.gx2_draw_done", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_gx2DrawDoneGuestLrCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.gx2_draw_done_last_lr", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_gx2SwapScanBuffersCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.gx2_swap_scan_buffers", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_gx2SwapScanBuffersGuestLrCounterNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("cemu.guest.mod.{}.gx2_swap_scan_buffers_last_lr", kSectionNames[index]);
		return names;
	}();

	const std::array<std::string, kSectionCount> s_gpuTagNames = [] {
		std::array<std::string, kSectionCount> names;
		for (size_t index = 0; index < names.size(); ++index)
			names[index] = fmt::format("GpuCommand/{}", kSectionNames[index]);
		return names;
	}();

	uint64 GetTimestampNs()
	{
		return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	void UpdateMax(std::atomic<uint64>& target, uint64 value)
	{
		uint64 current = target.load(std::memory_order_relaxed);
		while (current < value && !target.compare_exchange_weak(current, value, std::memory_order_relaxed))
		{
		}
	}

	void UpdateMaxWithGuestLr(std::atomic<uint64>& target, std::atomic<uint32>& targetGuestLr, uint64 value, uint32 guestLr)
	{
		uint64 current = target.load(std::memory_order_relaxed);
		while (current < value)
		{
			if (target.compare_exchange_weak(current, value, std::memory_order_relaxed))
			{
				targetGuestLr.store(guestLr, std::memory_order_relaxed);
				return;
			}
		}
	}

	uint32 GetGuestThreadId(PPCInterpreter_t* hCPU)
	{
		if (OSThread_t* thread = coreinit::OSGetCurrentThread())
			return memory_getVirtualOffsetFromPointer(thread);
		return 0xFFFFFF00u | PPCInterpreter_getCoreIndex(hCPU);
	}

	uint64 GetSpanKey(uint32 guestThreadId, uint32 sectionId)
	{
		return (static_cast<uint64>(guestThreadId) << 32) | sectionId;
	}

	uint32 GetProfilerThreadId(uint32 guestThreadId)
	{
		return 0x80000000u | (guestThreadId & 0x7FFFFFFFu);
	}

	bool IsGpuSemanticSection(uint32 sectionId)
	{
		return sectionId >= 21 && sectionId <= 32;
	}

	bool EmitGpuTag(PPCInterpreter_t* hCPU, uint32 sectionId, bool begin)
	{
		if (!hCPU || !s_enabled.load(std::memory_order_relaxed))
			return false;
		if (sectionId >= kSectionCount)
		{
			s_gpuTagInvalidSectionCount.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		const auto result = GX2::GX2EmitGuestGpuTag(begin, sectionId,
			GetGuestThreadId(hCPU), hCPU->spr.LR,
			s_generation.load(std::memory_order_relaxed));
		if (result == GX2::GuestGpuTagEmitResult::NoCommandBuffer)
		{
			s_gpuTagEmitNoCommandBufferCount.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (result == GX2::GuestGpuTagEmitResult::DisplayList)
		{
			s_gpuTagEmitDisplayListSkipCount.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		auto& stats = s_stats[sectionId];
		if (begin)
			stats.gpuTagEmittedBeginCount.fetch_add(1, std::memory_order_relaxed);
		else
			stats.gpuTagEmittedEndCount.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void NameGuestProfilerThread(uint32 guestThreadId)
	{
		if (!s_namedGuestThreads.emplace(guestThreadId).second)
			return;
		const std::string name = fmt::format("Cemu Guest 0x{:08X}", guestThreadId);
		spatial::profiler::ProfilerSetThreadIdName(GetProfilerThreadId(guestThreadId), name.c_str());
	}

	std::vector<uint32> GetActiveSectionIds(PPCInterpreter_t* hCPU)
	{
		std::vector<uint32> sectionIds;
		if (!hCPU || !s_enabled.load(std::memory_order_relaxed))
			return sectionIds;

		const uint32 guestThreadId = GetGuestThreadId(hCPU);
		std::scoped_lock lock{s_activeSpansMutex};
		for (uint32 sectionId = 0; sectionId < kSectionCount; ++sectionId)
		{
			const auto spanIt = s_activeSpans.find(GetSpanKey(guestThreadId, sectionId));
			if (spanIt != s_activeSpans.end() && !spanIt->second.empty())
				sectionIds.push_back(sectionId);
		}
		return sectionIds;
	}

	void BeginSection(PPCInterpreter_t* hCPU, uint32 sectionId)
	{
		if (!s_enabled.load(std::memory_order_relaxed))
			return;
		if (sectionId >= kSectionCount)
		{
			s_invalidSectionCount.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		const uint32 guestThreadId = GetGuestThreadId(hCPU);
		const uint64 key = GetSpanKey(guestThreadId, sectionId);
		std::scoped_lock lock{s_activeSpansMutex};
		auto& spans = s_activeSpans[key];
		if (spans.size() >= kMaxNestedSpansPerSection)
		{
			s_spanOverflowCount.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const uint64 startNs = GetTimestampNs();
		NameGuestProfilerThread(guestThreadId);
		const auto profilerTagToken = spatial::profiler::ProfilerTagBegin(
			kSectionNames[sectionId].data(), __FILE__, "GuestProfiler", 0,
			GetProfilerThreadId(guestThreadId), PPCInterpreter_getCoreIndex(hCPU));
		if (profilerTagToken != spatial::profiler::InvalidProfilerTagToken)
			s_timelineTagBeginCount.fetch_add(1, std::memory_order_relaxed);
		spans.push_back(ActiveSpan{
			.startNs = startNs,
			.generation = s_generation.load(std::memory_order_relaxed),
			.profilerTagToken = profilerTagToken,
		});
	}

	void EndSection(PPCInterpreter_t* hCPU, uint32 sectionId)
	{
		if (!s_enabled.load(std::memory_order_relaxed))
			return;
		if (sectionId >= kSectionCount)
		{
			s_invalidSectionCount.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		ActiveSpan activeSpan;
		const uint64 key = GetSpanKey(GetGuestThreadId(hCPU), sectionId);
		{
			std::scoped_lock lock{s_activeSpansMutex};
			auto spanIt = s_activeSpans.find(key);
			if (spanIt == s_activeSpans.end() || spanIt->second.empty())
			{
				s_unmatchedEndCount.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			activeSpan = spanIt->second.back();
			spanIt->second.pop_back();
			if (spanIt->second.empty())
				s_activeSpans.erase(spanIt);
		}
		const uint64 endNs = GetTimestampNs();
		spatial::profiler::ProfilerTagEnd(activeSpan.profilerTagToken);
		if (activeSpan.profilerTagToken != spatial::profiler::InvalidProfilerTagToken)
			s_timelineTagEndCount.fetch_add(1, std::memory_order_relaxed);

		if (activeSpan.generation != s_generation.load(std::memory_order_relaxed))
			return;
		const uint64 durationNs = endNs > activeSpan.startNs ? endNs - activeSpan.startNs : 0;
		auto& stats = s_stats[sectionId];
		stats.lastNs.store(durationNs, std::memory_order_relaxed);
		stats.totalNs.fetch_add(durationNs, std::memory_order_relaxed);
		const uint64 callCount = stats.callCount.fetch_add(1, std::memory_order_relaxed) + 1;
		UpdateMax(stats.maxNs, durationNs);

		SPATIAL_PROFILER_COUNTER_SET(s_lastCounterNames[sectionId].c_str(), durationNs / 1000, "Cemu Guest Mod", "us");
		SPATIAL_PROFILER_COUNTER_SET(s_maxCounterNames[sectionId].c_str(), stats.maxNs.load(std::memory_order_relaxed) / 1000, "Cemu Guest Mod", "us");
		SPATIAL_PROFILER_COUNTER_SET(s_callCounterNames[sectionId].c_str(), callCount, "Cemu Guest Mod", "calls");
	}

	void HookProfileSectionBegin(PPCInterpreter_t* hCPU)
	{
		const uint32 sectionId = hCPU->gpr[3];
		BeginSection(hCPU, sectionId);
		if (IsGpuSemanticSection(sectionId))
			EmitGpuTag(hCPU, sectionId, true);
		osLib_returnFromFunction(hCPU, 0);
	}

	void HookProfileSectionEnd(PPCInterpreter_t* hCPU)
	{
		const uint32 sectionId = hCPU->gpr[3];
		if (IsGpuSemanticSection(sectionId))
			EmitGpuTag(hCPU, sectionId, false);
		EndSection(hCPU, sectionId);
		osLib_returnFromFunction(hCPU, 0);
	}

	void HookGpuTagBegin(PPCInterpreter_t* hCPU)
	{
		osLib_returnFromFunction(hCPU, EmitGpuTag(hCPU, hCPU->gpr[3], true) ? 1 : 0);
	}

	void HookGpuTagEnd(PPCInterpreter_t* hCPU)
	{
		osLib_returnFromFunction(hCPU, EmitGpuTag(hCPU, hCPU->gpr[3], false) ? 1 : 0);
	}

	void CloseActiveSpans()
	{
		std::vector<ActiveSpan> activeSpans;
		{
			std::scoped_lock lock{s_activeSpansMutex};
			for (const auto& [key, spans] : s_activeSpans)
				activeSpans.insert(activeSpans.end(), spans.begin(), spans.end());
			s_activeSpans.clear();
		}

		std::ranges::sort(activeSpans, std::greater{}, &ActiveSpan::startNs);
		for (const ActiveSpan& span : activeSpans)
		{
			spatial::profiler::ProfilerTagEnd(span.profilerTagToken);
			if (span.profilerTagToken != spatial::profiler::InvalidProfilerTagToken)
				s_timelineTagEndCount.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void CloseActiveGpuTags()
	{
		std::vector<ActiveGpuTag> activeTags;
		{
			std::scoped_lock lock{s_activeGpuTagsMutex};
			activeTags.swap(s_activeGpuTags);
			s_activeGpuTagSection.store(kInvalidGpuTagSection, std::memory_order_release);
		}
		for (auto it = activeTags.rbegin(); it != activeTags.rend(); ++it)
			spatial::profiler::ProfilerTagEnd(it->profilerTagToken);
	}

	void ResetState()
	{
		s_generation.fetch_add(1, std::memory_order_relaxed);
		CloseActiveSpans();
		CloseActiveGpuTags();
		for (auto& stats : s_stats)
		{
			stats.lastNs.store(0, std::memory_order_relaxed);
			stats.maxNs.store(0, std::memory_order_relaxed);
			stats.totalNs.store(0, std::memory_order_relaxed);
			stats.callCount.store(0, std::memory_order_relaxed);
			stats.gx2SubmissionCount.store(0, std::memory_order_relaxed);
			stats.gx2SubmittedWords.store(0, std::memory_order_relaxed);
			stats.lastGx2GuestLr.store(0, std::memory_order_relaxed);
			stats.maxGx2SubmissionWords.store(0, std::memory_order_relaxed);
			stats.maxGx2SubmissionGuestLr.store(0, std::memory_order_relaxed);
			stats.gpuFenceCount.store(0, std::memory_order_relaxed);
			stats.lastGpuFenceGuestLr.store(0, std::memory_order_relaxed);
			stats.gx2DrawDoneCount.store(0, std::memory_order_relaxed);
			stats.lastGx2DrawDoneGuestLr.store(0, std::memory_order_relaxed);
			stats.gx2SwapScanBuffersCount.store(0, std::memory_order_relaxed);
			stats.lastGx2SwapScanBuffersGuestLr.store(0, std::memory_order_relaxed);
			stats.gpuTagEmittedBeginCount.store(0, std::memory_order_relaxed);
			stats.gpuTagEmittedEndCount.store(0, std::memory_order_relaxed);
			stats.gpuTagConsumedBeginCount.store(0, std::memory_order_relaxed);
			stats.gpuTagConsumedEndCount.store(0, std::memory_order_relaxed);
			stats.gpuTagHostDurationNs.store(0, std::memory_order_relaxed);
			stats.gpuTagDrawCount.store(0, std::memory_order_relaxed);
			stats.gpuTagFastDrawCount.store(0, std::memory_order_relaxed);
			stats.gpuTagDrawFragmentCount.store(0, std::memory_order_relaxed);
		}
		s_invalidSectionCount.store(0, std::memory_order_relaxed);
		s_unmatchedEndCount.store(0, std::memory_order_relaxed);
		s_spanOverflowCount.store(0, std::memory_order_relaxed);
		s_timelineTagBeginCount.store(0, std::memory_order_relaxed);
		s_timelineTagEndCount.store(0, std::memory_order_relaxed);
		s_gx2SubmissionCount.store(0, std::memory_order_relaxed);
		s_gx2SubmittedWords.store(0, std::memory_order_relaxed);
		s_unattributedGx2SubmissionCount.store(0, std::memory_order_relaxed);
		s_unattributedGx2SubmittedWords.store(0, std::memory_order_relaxed);
		s_maxUnattributedGx2SubmissionWords.store(0, std::memory_order_relaxed);
		s_maxUnattributedGx2SubmissionGuestLr.store(0, std::memory_order_relaxed);
		s_gpuFenceCount.store(0, std::memory_order_relaxed);
		s_unattributedGpuFenceCount.store(0, std::memory_order_relaxed);
		s_lastUnattributedGpuFenceGuestLr.store(0, std::memory_order_relaxed);
		s_gx2DrawDoneCount.store(0, std::memory_order_relaxed);
		s_unattributedGx2DrawDoneCount.store(0, std::memory_order_relaxed);
		s_lastUnattributedGx2DrawDoneGuestLr.store(0, std::memory_order_relaxed);
		s_gx2SwapScanBuffersCount.store(0, std::memory_order_relaxed);
		s_unattributedGx2SwapScanBuffersCount.store(0, std::memory_order_relaxed);
		s_lastUnattributedGx2SwapScanBuffersGuestLr.store(0, std::memory_order_relaxed);
		s_gpuTagEmitNoCommandBufferCount.store(0, std::memory_order_relaxed);
		s_gpuTagEmitDisplayListSkipCount.store(0, std::memory_order_relaxed);
		s_gpuTagInvalidSectionCount.store(0, std::memory_order_relaxed);
		s_gpuTagStackOverflowCount.store(0, std::memory_order_relaxed);
		s_gpuTagUnmatchedEndCount.store(0, std::memory_order_relaxed);
		s_gpuTagStalePacketCount.store(0, std::memory_order_relaxed);
	}

	std::string HandleProfilerStatus(const std::vector<std::string>& args)
	{
		if (args.size() > 1 || (!args.empty() && args[0] != "all"))
			return "usage: guest_profiler_status [all]\n";
		const bool includeEmpty = !args.empty();
		std::ostringstream out;
		out << "guest_profiler_status:\n";
		out << "hle_registered=true\n";
		out << "enabled=" << (s_enabled.load(std::memory_order_relaxed) ? "true" : "false") << "\n";
		out << "title_id=" << fmt::format("{:016X}", static_cast<uint64>(CafeSystem::GetForegroundTitleId())) << "\n";
		out << "title_version=" << CafeSystem::GetForegroundTitleVersion() << "\n";
		out << "patch_crc=" << (applicationRPX ? fmt::format("0x{:08X}", applicationRPX->patchCRC) : "unavailable") << "\n";
		{
			std::scoped_lock lock{s_activeSpansMutex};
			size_t activeSpanCount = 0;
			for (const auto& [key, spans] : s_activeSpans)
				activeSpanCount += spans.size();
			out << "active_spans=" << activeSpanCount << "\n";
		}
		out << "invalid_section_count=" << s_invalidSectionCount.load(std::memory_order_relaxed) << "\n";
		out << "unmatched_end_count=" << s_unmatchedEndCount.load(std::memory_order_relaxed) << "\n";
		out << "span_overflow_count=" << s_spanOverflowCount.load(std::memory_order_relaxed) << "\n";
		out << "timeline_tag_begin_count=" << s_timelineTagBeginCount.load(std::memory_order_relaxed) << "\n";
		out << "timeline_tag_end_count=" << s_timelineTagEndCount.load(std::memory_order_relaxed) << "\n";
		out << "gx2_submissions=" << s_gx2SubmissionCount.load(std::memory_order_relaxed) << "\n";
		out << "gx2_words=" << s_gx2SubmittedWords.load(std::memory_order_relaxed) << "\n";
		out << "unattributed_gx2_submissions=" << s_unattributedGx2SubmissionCount.load(std::memory_order_relaxed) << "\n";
		out << "unattributed_gx2_words=" << s_unattributedGx2SubmittedWords.load(std::memory_order_relaxed) << "\n";
		out << "max_unattributed_gx2_submission_words=" << s_maxUnattributedGx2SubmissionWords.load(std::memory_order_relaxed) << "\n";
		out << "max_unattributed_gx2_submission_guest_lr=" << fmt::format("0x{:08X}", s_maxUnattributedGx2SubmissionGuestLr.load(std::memory_order_relaxed)) << "\n";
		out << "gpu_fences=" << s_gpuFenceCount.load(std::memory_order_relaxed) << "\n";
		out << "unattributed_gpu_fences=" << s_unattributedGpuFenceCount.load(std::memory_order_relaxed) << "\n";
		out << "last_unattributed_gpu_fence_guest_lr=" << fmt::format("0x{:08X}", s_lastUnattributedGpuFenceGuestLr.load(std::memory_order_relaxed)) << "\n";
		out << "gx2_draw_done=" << s_gx2DrawDoneCount.load(std::memory_order_relaxed) << "\n";
		out << "unattributed_gx2_draw_done=" << s_unattributedGx2DrawDoneCount.load(std::memory_order_relaxed) << "\n";
		out << "last_unattributed_gx2_draw_done_guest_lr=" << fmt::format("0x{:08X}", s_lastUnattributedGx2DrawDoneGuestLr.load(std::memory_order_relaxed)) << "\n";
		out << "gx2_swap_scan_buffers=" << s_gx2SwapScanBuffersCount.load(std::memory_order_relaxed) << "\n";
		out << "unattributed_gx2_swap_scan_buffers=" << s_unattributedGx2SwapScanBuffersCount.load(std::memory_order_relaxed) << "\n";
		out << "last_unattributed_gx2_swap_scan_buffers_guest_lr=" << fmt::format("0x{:08X}", s_lastUnattributedGx2SwapScanBuffersGuestLr.load(std::memory_order_relaxed)) << "\n";
		out << "timeline_lane_semantics=one-virtual-profiler-thread-per-guest-osthread\n";
		{
			std::scoped_lock lock{s_activeGpuTagsMutex};
			out << "gpu_tag_active_depth=" << s_activeGpuTags.size() << "\n";
		}
		const uint32 activeGpuTagSection = s_activeGpuTagSection.load(std::memory_order_acquire);
		out << "gpu_tag_active_section=" << (activeGpuTagSection < kSectionCount ? fmt::format("{}:{}", activeGpuTagSection, kSectionNames[activeGpuTagSection]) : "none") << "\n";
		out << "gpu_tag_emit_no_command_buffer=" << s_gpuTagEmitNoCommandBufferCount.load(std::memory_order_relaxed) << "\n";
		out << "gpu_tag_emit_display_list_skips=" << s_gpuTagEmitDisplayListSkipCount.load(std::memory_order_relaxed) << "\n";
		out << "gpu_tag_invalid_sections=" << s_gpuTagInvalidSectionCount.load(std::memory_order_relaxed) << "\n";
		out << "gpu_tag_stack_overflow=" << s_gpuTagStackOverflowCount.load(std::memory_order_relaxed) << "\n";
		out << "gpu_tag_unmatched_end=" << s_gpuTagUnmatchedEndCount.load(std::memory_order_relaxed) << "\n";
		out << "gpu_tag_stale_packets=" << s_gpuTagStalePacketCount.load(std::memory_order_relaxed) << "\n";
		for (size_t index = 0; index < s_stats.size(); ++index)
		{
			const uint64 callCount = s_stats[index].callCount.load(std::memory_order_relaxed);
			const uint64 gpuTagConsumedBeginCount = s_stats[index].gpuTagConsumedBeginCount.load(std::memory_order_relaxed);
			const uint64 gpuTagEmittedBeginCount = s_stats[index].gpuTagEmittedBeginCount.load(std::memory_order_relaxed);
			const bool hasGpuTagActivity = gpuTagEmittedBeginCount != 0 || gpuTagConsumedBeginCount != 0;
			if (!includeEmpty && callCount == 0 && !hasGpuTagActivity)
				continue;
			const uint64 totalNs = s_stats[index].totalNs.load(std::memory_order_relaxed);
			if (includeEmpty || callCount != 0)
			{
				out << fmt::format("section[{}]={} calls={} last_us={} average_us={} max_us={} gx2_submits={} gx2_words={} gx2_last_lr=0x{:08X} gx2_max_words={} gx2_max_lr=0x{:08X} gpu_fences={} gpu_fence_last_lr=0x{:08X} gx2_draw_done={} gx2_draw_done_last_lr=0x{:08X} gx2_swap_scan_buffers={} gx2_swap_scan_buffers_last_lr=0x{:08X}\n",
					index, kSectionNames[index], callCount,
					s_stats[index].lastNs.load(std::memory_order_relaxed) / 1000,
					callCount == 0 ? 0 : totalNs / callCount / 1000,
					s_stats[index].maxNs.load(std::memory_order_relaxed) / 1000,
					s_stats[index].gx2SubmissionCount.load(std::memory_order_relaxed),
					s_stats[index].gx2SubmittedWords.load(std::memory_order_relaxed),
					s_stats[index].lastGx2GuestLr.load(std::memory_order_relaxed),
					s_stats[index].maxGx2SubmissionWords.load(std::memory_order_relaxed),
					s_stats[index].maxGx2SubmissionGuestLr.load(std::memory_order_relaxed),
					s_stats[index].gpuFenceCount.load(std::memory_order_relaxed),
					s_stats[index].lastGpuFenceGuestLr.load(std::memory_order_relaxed),
					s_stats[index].gx2DrawDoneCount.load(std::memory_order_relaxed),
					s_stats[index].lastGx2DrawDoneGuestLr.load(std::memory_order_relaxed),
					s_stats[index].gx2SwapScanBuffersCount.load(std::memory_order_relaxed),
					s_stats[index].lastGx2SwapScanBuffersGuestLr.load(std::memory_order_relaxed));
			}
			if (includeEmpty || hasGpuTagActivity)
			{
				out << fmt::format("gpu_tag_section[{}]={} emitted_begin={} emitted_end={} consumed_begin={} consumed_end={} host_total_us={} draw_fragments={} draws={} fast_draws={}\n",
					index, kSectionNames[index],
					gpuTagEmittedBeginCount,
					s_stats[index].gpuTagEmittedEndCount.load(std::memory_order_relaxed),
					gpuTagConsumedBeginCount,
					s_stats[index].gpuTagConsumedEndCount.load(std::memory_order_relaxed),
					s_stats[index].gpuTagHostDurationNs.load(std::memory_order_relaxed) / 1000,
					s_stats[index].gpuTagDrawFragmentCount.load(std::memory_order_relaxed),
					s_stats[index].gpuTagDrawCount.load(std::memory_order_relaxed),
					s_stats[index].gpuTagFastDrawCount.load(std::memory_order_relaxed));
			}
		}
		out << "duration_semantics=guest-section-wall-time-including-host-scheduling-and-waits\n";
		out << "gpu_tag_duration_semantics=latte-command-stream-wall-time-between-ordered-guest-markers\n";
		return out.str();
	}

	std::string HandleProfilerReset(const std::vector<std::string>& args)
	{
		if (!args.empty())
			return "usage: guest_profiler_reset\n";
		ResetState();
		return "guest_profiler_reset succeeded\n";
	}

	std::string HandleProfilerEnable(const std::vector<std::string>& args)
	{
		if (args.size() != 1 || (args[0] != "on" && args[0] != "off"))
			return "usage: guest_profiler_enable <on|off>\n";
		const bool enabled = args[0] == "on";
		s_enabled.store(enabled, std::memory_order_relaxed);
		if (!enabled)
			CloseActiveSpans();
		return fmt::format("guest_profiler_enable {}\n", enabled ? "on" : "off");
	}
}

void GuestProfiler::RecordGx2Submission(PPCInterpreter_t* hCPU, uint32 submittedWords)
{
	if (!hCPU)
		return;
	const uint32 guestLr = hCPU->spr.LR;
	const auto activeSectionIds = GetActiveSectionIds(hCPU);
	s_gx2SubmissionCount.fetch_add(1, std::memory_order_relaxed);
	s_gx2SubmittedWords.fetch_add(submittedWords, std::memory_order_relaxed);
	if (activeSectionIds.empty())
	{
		s_unattributedGx2SubmissionCount.fetch_add(1, std::memory_order_relaxed);
		s_unattributedGx2SubmittedWords.fetch_add(submittedWords, std::memory_order_relaxed);
		UpdateMaxWithGuestLr(s_maxUnattributedGx2SubmissionWords, s_maxUnattributedGx2SubmissionGuestLr, submittedWords, guestLr);
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.mod.unattributed.gx2_submits", s_unattributedGx2SubmissionCount.load(std::memory_order_relaxed), "Cemu Guest GX2", "submissions");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.mod.unattributed.gx2_words", s_unattributedGx2SubmittedWords.load(std::memory_order_relaxed), "Cemu Guest GX2", "words");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.mod.unattributed.gx2_max_words", s_maxUnattributedGx2SubmissionWords.load(std::memory_order_relaxed), "Cemu Guest GX2", "words");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.mod.unattributed.gx2_max_lr", s_maxUnattributedGx2SubmissionGuestLr.load(std::memory_order_relaxed), "Cemu Guest GX2", "address");
	}
	for (const uint32 sectionId : activeSectionIds)
	{
		auto& stats = s_stats[sectionId];
		const uint64 submissionCount = stats.gx2SubmissionCount.fetch_add(1, std::memory_order_relaxed) + 1;
		const uint64 wordCount = stats.gx2SubmittedWords.fetch_add(submittedWords, std::memory_order_relaxed) + submittedWords;
		stats.lastGx2GuestLr.store(guestLr, std::memory_order_relaxed);
		UpdateMaxWithGuestLr(stats.maxGx2SubmissionWords, stats.maxGx2SubmissionGuestLr, submittedWords, guestLr);
		SPATIAL_PROFILER_COUNTER_SET(s_gx2SubmitCounterNames[sectionId].c_str(), submissionCount, "Cemu Guest GX2", "submissions");
		SPATIAL_PROFILER_COUNTER_SET(s_gx2WordCounterNames[sectionId].c_str(), wordCount, "Cemu Guest GX2", "words");
		SPATIAL_PROFILER_COUNTER_SET(s_gx2GuestLrCounterNames[sectionId].c_str(), guestLr, "Cemu Guest GX2", "address");
	}
}

void GuestProfiler::RecordGpuFence(PPCInterpreter_t* hCPU)
{
	if (!hCPU)
		return;
	const uint32 guestLr = hCPU->spr.LR;
	const auto activeSectionIds = GetActiveSectionIds(hCPU);
	s_gpuFenceCount.fetch_add(1, std::memory_order_relaxed);
	if (activeSectionIds.empty())
	{
		s_unattributedGpuFenceCount.fetch_add(1, std::memory_order_relaxed);
		s_lastUnattributedGpuFenceGuestLr.store(guestLr, std::memory_order_relaxed);
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.mod.unattributed.gpu_fences", s_unattributedGpuFenceCount.load(std::memory_order_relaxed), "Cemu Guest Fence", "fences");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.mod.unattributed.gpu_fence_last_lr", guestLr, "Cemu Guest Fence", "address");
	}
	for (const uint32 sectionId : activeSectionIds)
	{
		auto& stats = s_stats[sectionId];
		const uint64 fenceCount = stats.gpuFenceCount.fetch_add(1, std::memory_order_relaxed) + 1;
		stats.lastGpuFenceGuestLr.store(guestLr, std::memory_order_relaxed);
		SPATIAL_PROFILER_COUNTER_SET(s_gpuFenceCounterNames[sectionId].c_str(), fenceCount, "Cemu Guest Fence", "fences");
		SPATIAL_PROFILER_COUNTER_SET(s_gpuFenceGuestLrCounterNames[sectionId].c_str(), guestLr, "Cemu Guest Fence", "address");
	}
}

void GuestProfiler::RecordGx2DrawDone(PPCInterpreter_t* hCPU)
{
	if (!hCPU)
		return;
	const uint32 guestLr = hCPU->spr.LR;
	const auto activeSectionIds = GetActiveSectionIds(hCPU);
	const uint64 drawDoneCount = s_gx2DrawDoneCount.fetch_add(1, std::memory_order_relaxed) + 1;
	SPATIAL_PROFILER_COUNTER_SET("cemu.guest.gx2_draw_done.count", drawDoneCount, "Cemu Guest Frame End", "calls");
	SPATIAL_PROFILER_COUNTER_SET("cemu.guest.gx2_draw_done.last_lr", guestLr, "Cemu Guest Frame End", "address");
	if (activeSectionIds.empty())
	{
		const uint64 unattributedCount = s_unattributedGx2DrawDoneCount.fetch_add(1, std::memory_order_relaxed) + 1;
		s_lastUnattributedGx2DrawDoneGuestLr.store(guestLr, std::memory_order_relaxed);
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.mod.unattributed.gx2_draw_done", unattributedCount, "Cemu Guest Frame End", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.mod.unattributed.gx2_draw_done_last_lr", guestLr, "Cemu Guest Frame End", "address");
	}
	for (const uint32 sectionId : activeSectionIds)
	{
		auto& stats = s_stats[sectionId];
		const uint64 sectionCount = stats.gx2DrawDoneCount.fetch_add(1, std::memory_order_relaxed) + 1;
		stats.lastGx2DrawDoneGuestLr.store(guestLr, std::memory_order_relaxed);
		SPATIAL_PROFILER_COUNTER_SET(s_gx2DrawDoneCounterNames[sectionId].c_str(), sectionCount, "Cemu Guest Frame End", "calls");
		SPATIAL_PROFILER_COUNTER_SET(s_gx2DrawDoneGuestLrCounterNames[sectionId].c_str(), guestLr, "Cemu Guest Frame End", "address");
	}
}

void GuestProfiler::RecordGx2SwapScanBuffers(PPCInterpreter_t* hCPU)
{
	if (!hCPU)
		return;
	const uint32 guestLr = hCPU->spr.LR;
	const auto activeSectionIds = GetActiveSectionIds(hCPU);
	const uint64 swapCount = s_gx2SwapScanBuffersCount.fetch_add(1, std::memory_order_relaxed) + 1;
	SPATIAL_PROFILER_COUNTER_SET("cemu.guest.gx2_swap_scan_buffers.count", swapCount, "Cemu Guest Frame End", "calls");
	SPATIAL_PROFILER_COUNTER_SET("cemu.guest.gx2_swap_scan_buffers.last_lr", guestLr, "Cemu Guest Frame End", "address");
	if (activeSectionIds.empty())
	{
		const uint64 unattributedCount = s_unattributedGx2SwapScanBuffersCount.fetch_add(1, std::memory_order_relaxed) + 1;
		s_lastUnattributedGx2SwapScanBuffersGuestLr.store(guestLr, std::memory_order_relaxed);
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.mod.unattributed.gx2_swap_scan_buffers", unattributedCount, "Cemu Guest Frame End", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.mod.unattributed.gx2_swap_scan_buffers_last_lr", guestLr, "Cemu Guest Frame End", "address");
	}
	for (const uint32 sectionId : activeSectionIds)
	{
		auto& stats = s_stats[sectionId];
		const uint64 sectionCount = stats.gx2SwapScanBuffersCount.fetch_add(1, std::memory_order_relaxed) + 1;
		stats.lastGx2SwapScanBuffersGuestLr.store(guestLr, std::memory_order_relaxed);
		SPATIAL_PROFILER_COUNTER_SET(s_gx2SwapScanBuffersCounterNames[sectionId].c_str(), sectionCount, "Cemu Guest Frame End", "calls");
		SPATIAL_PROFILER_COUNTER_SET(s_gx2SwapScanBuffersGuestLrCounterNames[sectionId].c_str(), guestLr, "Cemu Guest Frame End", "address");
	}
}

void GuestProfiler::ConsumeGpuTag(bool begin, uint32 sectionId, uint32 guestThreadId,
	uint32 guestLr, uint32 generation)
{
	if (generation != s_generation.load(std::memory_order_relaxed))
	{
		s_gpuTagStalePacketCount.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (sectionId >= kSectionCount)
	{
		s_gpuTagInvalidSectionCount.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	std::scoped_lock lock{s_activeGpuTagsMutex};
	auto& stats = s_stats[sectionId];
	if (begin)
	{
		if (s_activeGpuTags.size() >= kMaxNestedGpuTags)
		{
			s_gpuTagStackOverflowCount.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const auto profilerTagToken = spatial::profiler::ProfilerTagBegin(
			s_gpuTagNames[sectionId].c_str(), __FILE__, "GuestGpuCommandStream", 0,
			kGpuTagProfilerThreadId);
		s_activeGpuTags.push_back(ActiveGpuTag{
			.sectionId = sectionId,
			.guestThreadId = guestThreadId,
			.guestLr = guestLr,
			.generation = generation,
			.startNs = GetTimestampNs(),
			.profilerTagToken = profilerTagToken,
		});
		s_activeGpuTagSection.store(sectionId, std::memory_order_release);
		stats.gpuTagConsumedBeginCount.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	if (s_activeGpuTags.empty() || s_activeGpuTags.back().sectionId != sectionId)
	{
		s_gpuTagUnmatchedEndCount.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const ActiveGpuTag activeTag = s_activeGpuTags.back();
	s_activeGpuTags.pop_back();
	spatial::profiler::ProfilerTagEnd(activeTag.profilerTagToken);
	const uint64 endNs = GetTimestampNs();
	stats.gpuTagConsumedEndCount.fetch_add(1, std::memory_order_relaxed);
	stats.gpuTagHostDurationNs.fetch_add(
		endNs > activeTag.startNs ? endNs - activeTag.startNs : 0,
		std::memory_order_relaxed);
	s_activeGpuTagSection.store(
		s_activeGpuTags.empty() ? kInvalidGpuTagSection : s_activeGpuTags.back().sectionId,
		std::memory_order_release);
}

uint32 GuestProfiler::GetActiveGpuTagSection()
{
	return s_activeGpuTagSection.load(std::memory_order_acquire);
}

void GuestProfiler::RecordGpuTagDrawBatch(uint32 sectionId, uint32 drawCount, uint32 fastDrawCount)
{
	if (sectionId >= kSectionCount || drawCount == 0)
		return;
	auto& stats = s_stats[sectionId];
	stats.gpuTagDrawFragmentCount.fetch_add(1, std::memory_order_relaxed);
	stats.gpuTagDrawCount.fetch_add(drawCount, std::memory_order_relaxed);
	stats.gpuTagFastDrawCount.fetch_add(fastDrawCount, std::memory_order_relaxed);
}

void GuestProfiler::Initialize()
{
	osLib_addFunction("coreinit", "hook_ProfileSectionBegin", HookProfileSectionBegin);
	osLib_addFunction("coreinit", "hook_ProfileSectionEnd", HookProfileSectionEnd);
	osLib_addFunction("gx2", "hook_GpuTagBegin", HookGpuTagBegin);
	osLib_addFunction("gx2", "hook_GpuTagEnd", HookGpuTagEnd);
	spatial::profiler::ProfilerSetThreadIdName(kGpuTagProfilerThreadId, "Cemu Guest GPU Command Stream");
}

void GuestProfiler::Shutdown()
{
	ResetState();
}

void GuestProfiler::Reset()
{
	ResetState();
}

void GuestProfiler::RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry)
{
	registry.Register("guest_profiler_status", "Show Guest mod profiler spans and evidence", HandleProfilerStatus);
	registry.Register("guest_profiler_reset", "Reset Guest mod profiler counters", HandleProfilerReset);
	registry.Register("guest_profiler_enable", "Enable or disable Guest mod profiler collection", HandleProfilerEnable);
}
