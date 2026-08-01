#include "Cafe/Diagnostics/GuestProfiler.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/RPL/rpl.h"

#include "spatial/debugbus/DebugCommandRegistry.h"
#include "spatial/profiler/Profiler.h"

namespace
{
	constexpr size_t kSectionCount = 40;
	constexpr size_t kMaxNestedSpansPerSection = 16;

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
	};

	struct ActiveSpan
	{
		uint64 startNs{};
		uint32 generation{};
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
	std::unordered_set<uint32> s_namedGuestThreads;

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

	void NameGuestProfilerThread(uint32 guestThreadId)
	{
		if (!s_namedGuestThreads.emplace(guestThreadId).second)
			return;
		const std::string name = fmt::format("Cemu Guest 0x{:08X}", guestThreadId);
		spatial::profiler::ProfilerSetThreadIdName(GetProfilerThreadId(guestThreadId), name.c_str());
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
		osLib_returnFromFunction(hCPU, 0);
	}

	void HookProfileSectionEnd(PPCInterpreter_t* hCPU)
	{
		const uint32 sectionId = hCPU->gpr[3];
		EndSection(hCPU, sectionId);
		osLib_returnFromFunction(hCPU, 0);
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

	void Reset()
	{
		s_generation.fetch_add(1, std::memory_order_relaxed);
		CloseActiveSpans();
		for (auto& stats : s_stats)
		{
			stats.lastNs.store(0, std::memory_order_relaxed);
			stats.maxNs.store(0, std::memory_order_relaxed);
			stats.totalNs.store(0, std::memory_order_relaxed);
			stats.callCount.store(0, std::memory_order_relaxed);
		}
		s_invalidSectionCount.store(0, std::memory_order_relaxed);
		s_unmatchedEndCount.store(0, std::memory_order_relaxed);
		s_spanOverflowCount.store(0, std::memory_order_relaxed);
		s_timelineTagBeginCount.store(0, std::memory_order_relaxed);
		s_timelineTagEndCount.store(0, std::memory_order_relaxed);
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
		out << "timeline_lane_semantics=one-virtual-profiler-thread-per-guest-osthread\n";
		for (size_t index = 0; index < s_stats.size(); ++index)
		{
			const uint64 callCount = s_stats[index].callCount.load(std::memory_order_relaxed);
			if (!includeEmpty && callCount == 0)
				continue;
			const uint64 totalNs = s_stats[index].totalNs.load(std::memory_order_relaxed);
			out << fmt::format("section[{}]={} calls={} last_us={} average_us={} max_us={}\n",
				index, kSectionNames[index], callCount,
				s_stats[index].lastNs.load(std::memory_order_relaxed) / 1000,
				callCount == 0 ? 0 : totalNs / callCount / 1000,
				s_stats[index].maxNs.load(std::memory_order_relaxed) / 1000);
		}
		out << "duration_semantics=guest-section-wall-time-including-host-scheduling-and-waits\n";
		return out.str();
	}

	std::string HandleProfilerReset(const std::vector<std::string>& args)
	{
		if (!args.empty())
			return "usage: guest_profiler_reset\n";
		Reset();
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

void GuestProfiler::Initialize()
{
	osLib_addFunction("coreinit", "hook_ProfileSectionBegin", HookProfileSectionBegin);
	osLib_addFunction("coreinit", "hook_ProfileSectionEnd", HookProfileSectionEnd);
}

void GuestProfiler::Shutdown()
{
	Reset();
}

void GuestProfiler::RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry)
{
	registry.Register("guest_profiler_status", "Show Guest mod profiler spans and evidence", HandleProfilerStatus);
	registry.Register("guest_profiler_reset", "Reset Guest mod profiler counters", HandleProfilerReset);
	registry.Register("guest_profiler_enable", "Enable or disable Guest mod profiler collection", HandleProfilerEnable);
}
