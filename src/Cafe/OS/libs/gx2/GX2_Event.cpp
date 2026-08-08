#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/Diagnostics/GuestProfiler.h"
#include "GX2_Command.h"
#include "GX2_Event.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "GX2.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "util/helpers/ConcurrentQueue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>

#include "spatial/profiler/Profiler.h"

namespace
{
	constexpr uint64 kBotWEventTitleIdJp = 0x00050000101C9300ull;
	constexpr uint16 kBotWEventVersion = 208;
	constexpr auto kDisplayOrdinalWaitSlice = std::chrono::milliseconds(1);

	std::atomic_bool s_displayOrdinalDependencyEnabled{};
	std::atomic<uint32> s_displayOrdinalCounterPhysAddr{};
	std::atomic<uint32> s_displayOrdinalPublished{};
	std::atomic<uint64> s_displayOrdinalRegisterCount{};
	std::atomic<uint64> s_displayOrdinalSamples{};
	std::atomic<uint64> s_displayOrdinalNotifications{};
	std::atomic<uint64> s_displayOrdinalEmitted{};
	std::atomic<uint64> s_displayOrdinalConsumed{};
	std::atomic<uint64> s_displayOrdinalFallback{};
	std::atomic<uint64> s_displayOrdinalWaitCount{};
	std::atomic<uint64> s_displayOrdinalImmediateCount{};
	std::atomic<uint64> s_displayOrdinalWaitTotalUs{};
	std::atomic<uint64> s_displayOrdinalLastWaitUs{};
	std::atomic<uint64> s_displayOrdinalWakeups{};
	std::mutex s_displayOrdinalWaitMutex;
	std::condition_variable s_displayOrdinalWaitCondition;

	std::atomic_bool s_drawDoneVisibilityDeferralEnabled{};
	std::atomic<uint32> s_drawDoneVisibilityDeferralGuestLr{};
	std::atomic<uint64> s_drawDoneVisibilityDeferralRegisterCount{};
	std::atomic<uint64> s_drawDoneVisibilityDeferredCount{};
	std::atomic<uint64> s_drawDoneVisibilityNormalCount{};
	std::atomic<uint32> s_drawDoneVisibilityLastGuestLr{};

	constexpr uint32 kGuestFeedbackPolicyVersion = 1;
	std::atomic_bool s_guestFeedbackEnabled{};
	std::atomic<uint32> s_guestFeedbackGuestLr{};
	std::atomic<uint32> s_guestFeedbackPolicyVersion{};
	std::atomic<uint32> s_guestFeedbackMode{};
	std::atomic<uint64> s_guestFeedbackRegisterCount{};
	std::atomic<uint64> s_guestFeedbackBoundaryCount{};
	std::atomic<uint32> s_guestFeedbackLastGuestLr{};
	std::atomic<uint32> s_guestFeedbackFrameId{};
	std::atomic<uint32> s_drawDoneSequence{};

	void ResetDisplayOrdinalCounters()
	{
		s_displayOrdinalSamples.store(0, std::memory_order_relaxed);
		s_displayOrdinalNotifications.store(0, std::memory_order_relaxed);
		s_displayOrdinalEmitted.store(0, std::memory_order_relaxed);
		s_displayOrdinalConsumed.store(0, std::memory_order_relaxed);
		s_displayOrdinalFallback.store(0, std::memory_order_relaxed);
		s_displayOrdinalWaitCount.store(0, std::memory_order_relaxed);
		s_displayOrdinalImmediateCount.store(0, std::memory_order_relaxed);
		s_displayOrdinalWaitTotalUs.store(0, std::memory_order_relaxed);
		s_displayOrdinalLastWaitUs.store(0, std::memory_order_relaxed);
		s_displayOrdinalWakeups.store(0, std::memory_order_relaxed);
	}

	void ResetDisplayOrdinalDependency()
	{
		s_displayOrdinalDependencyEnabled.store(false, std::memory_order_release);
		s_displayOrdinalCounterPhysAddr.store(0, std::memory_order_relaxed);
		s_displayOrdinalPublished.store(0, std::memory_order_release);
		ResetDisplayOrdinalCounters();
		s_displayOrdinalWaitCondition.notify_all();
	}

	void ResetDrawDoneVisibilityDeferral()
	{
		s_drawDoneVisibilityDeferralEnabled.store(false, std::memory_order_release);
		s_drawDoneVisibilityDeferralGuestLr.store(0, std::memory_order_relaxed);
		s_drawDoneVisibilityDeferredCount.store(0, std::memory_order_relaxed);
		s_drawDoneVisibilityNormalCount.store(0, std::memory_order_relaxed);
		s_drawDoneVisibilityLastGuestLr.store(0, std::memory_order_relaxed);
	}

	void ResetGuestFeedbackPolicy()
	{
		s_guestFeedbackEnabled.store(false, std::memory_order_release);
		s_guestFeedbackGuestLr.store(0, std::memory_order_relaxed);
		s_guestFeedbackPolicyVersion.store(0, std::memory_order_relaxed);
		s_guestFeedbackMode.store(0, std::memory_order_relaxed);
		s_guestFeedbackBoundaryCount.store(0, std::memory_order_relaxed);
		s_guestFeedbackLastGuestLr.store(0, std::memory_order_relaxed);
		s_guestFeedbackFrameId.store(0, std::memory_order_relaxed);
		s_drawDoneSequence.store(0, std::memory_order_relaxed);
		LatteTextureReadback_ResetFeedbackObservation();
	}

	bool IsDisplayOrdinalReached(uint32 current, uint32 target)
	{
		return current >= target;
	}

	void PublishDisplayOrdinal(uint32 ordinal, bool fromVsyncCallback)
	{
		if (!s_displayOrdinalDependencyEnabled.load(std::memory_order_acquire))
			return;

		uint32 current = s_displayOrdinalPublished.load(std::memory_order_relaxed);
		while (current < ordinal &&
			!s_displayOrdinalPublished.compare_exchange_weak(current, ordinal,
				std::memory_order_release, std::memory_order_relaxed))
		{
		}
		if (fromVsyncCallback)
			s_displayOrdinalNotifications.fetch_add(1, std::memory_order_relaxed);
		else
			s_displayOrdinalSamples.fetch_add(1, std::memory_order_relaxed);

		SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.published",
			s_displayOrdinalPublished.load(std::memory_order_relaxed), "Cemu Display Dependency", "ordinal");
		s_displayOrdinalWaitCondition.notify_all();
	}

	void PublishRegisteredDisplayOrdinal(bool fromVsyncCallback)
	{
		if (!s_displayOrdinalDependencyEnabled.load(std::memory_order_acquire))
			return;
		const uint32 physAddr = s_displayOrdinalCounterPhysAddr.load(std::memory_order_relaxed);
		if (physAddr == 0)
			return;
		const auto* counter = reinterpret_cast<const uint32be*>(memory_getPointerFromPhysicalOffset(physAddr));
		PublishDisplayOrdinal(counter->value(), fromVsyncCallback);
	}
}

namespace GX2
{

	SysAllocator<coreinit::OSThreadQueue> g_vsyncThreadQueue;
	SysAllocator<coreinit::OSThreadQueue> g_flipThreadQueue;

	void GX2SetGPUFence(uint32be* fencePtr, uint32 mask, uint32 compareOp, uint32 compareValue)
	{
		SPATIAL_PROFILER_AUTO_SCOPE_NAME("gx2.guest.set_gpu_fence");
		GuestProfiler::RecordGpuFence(PPCInterpreter_getCurrentInstance());
		const uint32 physAddr = memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(fencePtr)) & ~3u;
		static std::atomic<uint64> fenceCount{};
		const uint64 currentFenceCount = fenceCount.fetch_add(1, std::memory_order_relaxed) + 1;
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.gpu_fence.count", currentFenceCount, "Cemu Guest Fence", "fences");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.gpu_fence.phys_addr", physAddr, "Cemu Guest Fence", "address");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.gpu_fence.compare_op", compareOp, "Cemu Guest Fence", "enum");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.gpu_fence.reference", compareValue, "Cemu Guest Fence", "value");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.gpu_fence.mask", mask, "Cemu Guest Fence", "value");
		const bool displayOrdinalMatch = s_displayOrdinalDependencyEnabled.load(std::memory_order_acquire) &&
			physAddr == s_displayOrdinalCounterPhysAddr.load(std::memory_order_relaxed) &&
			mask == 0xFFFFFFFFu && compareOp == 6;
		if (displayOrdinalMatch)
		{
			// Sample once on the submitting Guest thread. Later updates are published once,
			// after the registered VSYNC callback has completed, instead of polling Guest RAM.
			PublishDisplayOrdinal(fencePtr->value(), false);
			const uint32 feedbackFrameId = s_guestFeedbackFrameId.load(std::memory_order_relaxed);
			GX2ReserveCmdSpace(3);
			gx2WriteGather_submit(
				pm4HeaderType3(IT_HLE_WAIT_DISPLAY_ORDINAL, 2),
				compareValue,
				feedbackFrameId);
			const uint64 emitted = s_displayOrdinalEmitted.fetch_add(1, std::memory_order_relaxed) + 1;
			SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.emitted", emitted, "Cemu Display Dependency", "waits");
			return;
		}
		if (s_displayOrdinalDependencyEnabled.load(std::memory_order_relaxed))
			s_displayOrdinalFallback.fetch_add(1, std::memory_order_relaxed);
		GX2ReserveCmdSpace(7);
		uint8 compareOpTable[] = { 0x7,0x1,0x3,0x2,0x6,0x4,0x5,0x0 };
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_WAIT_REG_MEM, 6));
		gx2WriteGather_submitU32AsBE((uint32)(compareOpTable[compareOp & 7]) | 0x10); // compare operand + memory select (0x10)
		gx2WriteGather_submitU32AsBE(physAddr | 2); // physical address + type size flag(?)
		gx2WriteGather_submitU32AsBE(0); // ukn, always set to 0
		gx2WriteGather_submitU32AsBE(compareValue); // fence value
		gx2WriteGather_submitU32AsBE(mask); // fence mask
		gx2WriteGather_submitU32AsBE(0xA); // unknown purpose
	}

	enum class GX2PipeEventType : uint32
	{
		TOP = 0,
		BOTTOM = 1,
		BOTTOM_AFTER_FLUSH = 2
	};

	void GX2SubmitUserTimeStamp(uint64* timestampOut, uint64 value, GX2PipeEventType eventType, uint32 triggerInterrupt)
	{
		GX2ReserveCmdSpace(11);

		MPTR physTimestampAddr = memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(timestampOut));
		uint32 valHigh = (uint32)(value >> 32);
		uint32 valLow = (uint32)(value & 0xffffffff);

		if (eventType == GX2PipeEventType::TOP)
		{
			// write when on top of pipe
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_MEM_WRITE, 4));
			gx2WriteGather_submitU32AsBE(physTimestampAddr | 0x2);
			gx2WriteGather_submitU32AsBE(0); // 0x40000 -> 32bit write, 0x0 -> 64bit write?
			gx2WriteGather_submitU32AsBE(valLow); // low
			gx2WriteGather_submitU32AsBE(valHigh); // high
			if (triggerInterrupt != 0)
			{
				// top callback
				gx2WriteGather_submitU32AsBE(0x0000304A);
				gx2WriteGather_submitU32AsBE(0x40000000);
			}
		}
		else if (eventType == GX2PipeEventType::BOTTOM_AFTER_FLUSH)
		{
			// write when on bottom of pipe
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_MEM_WRITE, 4));
			gx2WriteGather_submitU32AsBE(physTimestampAddr | 0x2);
			gx2WriteGather_submitU32AsBE(0); // 0x40000 -> 32bit write, 0x0 -> 64bit write?
			gx2WriteGather_submitU32AsBE(valLow); // low
			gx2WriteGather_submitU32AsBE(valHigh); // high
			// trigger CB
			if (triggerInterrupt != 0)
			{
				// bottom callback
				// todo -> Fix this
				gx2WriteGather_submitU32AsBE(0x0000304B); // hax -> This event is handled differently and uses a different packet?
				gx2WriteGather_submitU32AsBE(0x40000000);
				// trigger bottom of pipe callback
				// used by Mario & Sonic Rio
				gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_BOTTOM_OF_PIPE_CB, 3));
				gx2WriteGather_submitU32AsBE(physTimestampAddr);
				gx2WriteGather_submitU32AsBE(valLow); // low
				gx2WriteGather_submitU32AsBE(valHigh); // high
			}
		}
		else if (eventType == GX2PipeEventType::BOTTOM)
		{
			// fix this
			// write timestamp when on bottom of pipe
			if (triggerInterrupt != 0)
			{
				// write value and trigger CB
				// todo: Use correct packet
				gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_BOTTOM_OF_PIPE_CB, 3));
				gx2WriteGather_submitU32AsBE(physTimestampAddr);
				gx2WriteGather_submitU32AsBE(valLow); // low
				gx2WriteGather_submitU32AsBE(valHigh); // high
			}
			else
			{
				// write value but don't trigger CB
				gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_MEM_WRITE, 4));
				gx2WriteGather_submitU32AsBE(physTimestampAddr | 0x2);
				gx2WriteGather_submitU32AsBE(0); // 0x40000 -> 32bit write, 0x0 -> 64bit write?
				gx2WriteGather_submitU32AsBE(valLow); // low
				gx2WriteGather_submitU32AsBE(valHigh); // high
			}
		}
		else
		{
			cemu_assert_debug(false);
		}
	}

	struct GX2EventFunc
	{
		MEMPTR<void> callbackFuncPtr;
		MEMPTR<void> userData;
	}s_eventCallback[GX2CallbackEventTypeCount]{};

	void GX2SetEventCallback(GX2CallbackEventType eventType, void* callbackFuncPtr, void* userData)
	{
		if ((size_t)eventType >= GX2CallbackEventTypeCount)
		{
			cemuLog_log(LogType::Force, "GX2SetEventCallback(): Unknown eventType");
			return;
		}
		s_eventCallback[(size_t)eventType].callbackFuncPtr = callbackFuncPtr;
		s_eventCallback[(size_t)eventType].userData = userData;
	}

	void GX2GetEventCallback(GX2CallbackEventType eventType, MEMPTR<void>* callbackFuncPtrOut, MEMPTR<void>* userDataOut)
	{
		if ((size_t)eventType >= GX2CallbackEventTypeCount)
		{
			cemuLog_log(LogType::Force, "GX2GetEventCallback(): Unknown eventType");
			return;
		}
		if (callbackFuncPtrOut)
			*callbackFuncPtrOut = s_eventCallback[(size_t)eventType].callbackFuncPtr;
		if (userDataOut)
			*userDataOut = s_eventCallback[(size_t)eventType].userData;
	}

	// event callback thread
	bool s_callbackThreadLaunched{};
	SysAllocator<OSThread_t> s_eventCallbackThread;
	SysAllocator<uint8, 0x2000> s_eventCallbackThreadStack;
	SysAllocator<char, 64> s_eventCallbackThreadName;
	// event callback queue
	struct GX2EventQueueEntry
	{
		GX2EventQueueEntry() {};
		GX2EventQueueEntry(GX2CallbackEventType eventType) : eventType(eventType) {};
		GX2CallbackEventType eventType{(GX2CallbackEventType)-1};
	};

	SysAllocator<coreinit::OSSemaphore> s_eventCbQueueSemaphore;
	ConcurrentQueue<GX2EventQueueEntry> s_eventCbQueue;

	void __GX2NotifyEvent(GX2CallbackEventType eventType)
	{
		if ((size_t)eventType >= GX2CallbackEventTypeCount)
		{
			cemu_assert_debug(false);
			return;
		}
		if (s_eventCallback[(size_t)eventType].callbackFuncPtr)
		{
			s_eventCbQueue.push(eventType);
			coreinit::OSSignalSemaphore(s_eventCbQueueSemaphore);
		}
		// wake up threads that are waiting for VSYNC or FLIP event
		if (eventType == GX2CallbackEventType::VSYNC)
		{
			__OSLockScheduler();
			g_vsyncThreadQueue->wakeupEntireWaitQueue(false);
			__OSUnlockScheduler();
		}
		else if (eventType == GX2CallbackEventType::FLIP)
		{
			__OSLockScheduler();
			g_flipThreadQueue->wakeupEntireWaitQueue(false);
			__OSUnlockScheduler();
		}
	}

	void __GX2CallbackThread(PPCInterpreter_t* hCPU)
	{
		while (coreinit::OSWaitSemaphore(s_eventCbQueueSemaphore))
		{
			GX2EventQueueEntry entry;
			if (!s_eventCbQueue.peek2(entry))
				continue;
			if(!s_eventCallback[(size_t)entry.eventType].callbackFuncPtr)
				continue;
			PPCCoreCallback(s_eventCallback[(size_t)entry.eventType].callbackFuncPtr, (sint32)entry.eventType, s_eventCallback[(size_t)entry.eventType].userData);
			if (entry.eventType == GX2CallbackEventType::VSYNC)
				PublishRegisteredDisplayOrdinal(true);
		}
		osLib_returnFromFunction(hCPU, 0);
	}

	bool GX2WaitDisplayOrdinal(uint32 targetOrdinal, uint32 feedbackFrameId)
	{
		SPATIAL_PROFILER_AUTO_SCOPE_NAME("latte.sync.wait_display_ordinal");
		SPATIAL_PROFILER_AUTO_SCOPE_NAME("latte.dependency.host_event.wait");
		const auto waitStart = std::chrono::steady_clock::now();
		const uint32 initialOrdinal = s_displayOrdinalPublished.load(std::memory_order_acquire);
		uint64 wakeups = 0;
		bool notifiedIdle = false;

		while (s_displayOrdinalDependencyEnabled.load(std::memory_order_acquire) &&
			!IsDisplayOrdinalReached(s_displayOrdinalPublished.load(std::memory_order_acquire), targetOrdinal))
		{
			if (!notifiedIdle && g_renderer)
			{
				g_renderer->NotifyLatteCommandProcessorIdle();
				notifiedIdle = true;
			}
			LatteTiming_HandleTimedVsync();
			LatteAsyncCommands_checkAndExecute();

			std::unique_lock lock{s_displayOrdinalWaitMutex};
			s_displayOrdinalWaitCondition.wait_for(lock, kDisplayOrdinalWaitSlice, [targetOrdinal] {
				return !s_displayOrdinalDependencyEnabled.load(std::memory_order_acquire) ||
					IsDisplayOrdinalReached(s_displayOrdinalPublished.load(std::memory_order_acquire), targetOrdinal);
			});
			wakeups++;
		}

		const uint32 finalOrdinal = s_displayOrdinalPublished.load(std::memory_order_acquire);
		const uint64 waitUs = static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - waitStart).count());
		const uint64 waitCount = s_displayOrdinalWaitCount.fetch_add(1, std::memory_order_relaxed) + 1;
		const uint64 consumed = s_displayOrdinalConsumed.fetch_add(1, std::memory_order_relaxed) + 1;
		if (IsDisplayOrdinalReached(initialOrdinal, targetOrdinal))
			s_displayOrdinalImmediateCount.fetch_add(1, std::memory_order_relaxed);
		s_displayOrdinalWaitTotalUs.fetch_add(waitUs, std::memory_order_relaxed);
		s_displayOrdinalLastWaitUs.store(waitUs, std::memory_order_relaxed);
		s_displayOrdinalWakeups.fetch_add(wakeups, std::memory_order_relaxed);

		SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.consumed", consumed, "Cemu Display Dependency", "waits");
		SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.wait_count", waitCount, "Cemu Display Dependency", "waits");
		SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.last_wait_us", waitUs, "Cemu Display Dependency", "us");
		SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.target", targetOrdinal, "Cemu Display Dependency", "ordinal");
		SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.initial", initialOrdinal, "Cemu Display Dependency", "ordinal");
		SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.final", finalOrdinal, "Cemu Display Dependency", "ordinal");
		SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.wakeups_last", wakeups, "Cemu Display Dependency", "wakeups");
		SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.feedback_frame_id", feedbackFrameId,
			"Cemu Display Dependency", "frame");
		return IsDisplayOrdinalReached(finalOrdinal, targetOrdinal);
	}

	std::string GX2GetDisplayOrdinalDependencyStatus()
	{
		std::ostringstream out;
		out << "display_ordinal_dependency:\n";
		out << "enabled=" << (s_displayOrdinalDependencyEnabled.load(std::memory_order_acquire) ? "true" : "false") << "\n";
		out << "counter_phys_addr=0x" << std::hex << s_displayOrdinalCounterPhysAddr.load(std::memory_order_relaxed) << std::dec << "\n";
		out << "published=" << s_displayOrdinalPublished.load(std::memory_order_relaxed) << "\n";
		out << "register_count=" << s_displayOrdinalRegisterCount.load(std::memory_order_relaxed) << "\n";
		out << "samples=" << s_displayOrdinalSamples.load(std::memory_order_relaxed) << "\n";
		out << "notifications=" << s_displayOrdinalNotifications.load(std::memory_order_relaxed) << "\n";
		out << "emitted=" << s_displayOrdinalEmitted.load(std::memory_order_relaxed) << "\n";
		out << "consumed=" << s_displayOrdinalConsumed.load(std::memory_order_relaxed) << "\n";
		out << "fallback=" << s_displayOrdinalFallback.load(std::memory_order_relaxed) << "\n";
		out << "wait_count=" << s_displayOrdinalWaitCount.load(std::memory_order_relaxed) << "\n";
		out << "immediate=" << s_displayOrdinalImmediateCount.load(std::memory_order_relaxed) << "\n";
		out << "wait_total_us=" << s_displayOrdinalWaitTotalUs.load(std::memory_order_relaxed) << "\n";
		out << "last_wait_us=" << s_displayOrdinalLastWaitUs.load(std::memory_order_relaxed) << "\n";
		out << "wakeups=" << s_displayOrdinalWakeups.load(std::memory_order_relaxed) << "\n";
		return out.str();
	}

	std::string GX2GetDrawDoneVisibilityDeferralStatus()
	{
		std::ostringstream out;
		out << "draw_done_visibility_deferral:\n";
		out << "enabled=" << (s_drawDoneVisibilityDeferralEnabled.load(std::memory_order_acquire) ? "true" : "false") << "\n";
		out << "guest_lr=" << fmt::format("0x{:08X}", s_drawDoneVisibilityDeferralGuestLr.load(std::memory_order_relaxed)) << "\n";
		out << "register_count=" << s_drawDoneVisibilityDeferralRegisterCount.load(std::memory_order_relaxed) << "\n";
		out << "deferred=" << s_drawDoneVisibilityDeferredCount.load(std::memory_order_relaxed) << "\n";
		out << "normal=" << s_drawDoneVisibilityNormalCount.load(std::memory_order_relaxed) << "\n";
		out << "last_guest_lr=" << fmt::format("0x{:08X}", s_drawDoneVisibilityLastGuestLr.load(std::memory_order_relaxed)) << "\n";
		return out.str();
	}

	std::string GX2GetGuestFeedbackStatus()
	{
		std::ostringstream out;
		out << "guest_feedback:\n";
		out << "enabled=" << (s_guestFeedbackEnabled.load(std::memory_order_acquire) ? "true" : "false") << "\n";
		out << "policy_version=" << s_guestFeedbackPolicyVersion.load(std::memory_order_relaxed) << "\n";
		const uint32 mode = s_guestFeedbackMode.load(std::memory_order_relaxed);
		out << "mode=" << (mode == 1 ? "guarded_previous_generation" : "observe_full_visibility") << "\n";
		out << "guest_lr=" << fmt::format("0x{:08X}", s_guestFeedbackGuestLr.load(std::memory_order_relaxed)) << "\n";
		out << "register_count=" << s_guestFeedbackRegisterCount.load(std::memory_order_relaxed) << "\n";
		out << "enqueued_boundaries=" << s_guestFeedbackBoundaryCount.load(std::memory_order_relaxed) << "\n";
		out << "last_guest_lr=" << fmt::format("0x{:08X}", s_guestFeedbackLastGuestLr.load(std::memory_order_relaxed)) << "\n";
		out << LatteTextureReadback_GetFeedbackObservationStatus();
		return out.str();
	}

	GX2GuestFeedbackPolicySnapshot GX2GetGuestFeedbackPolicySnapshot()
	{
		return {
			.enabled = s_guestFeedbackEnabled.load(std::memory_order_acquire),
			.legacyDeferralEnabled = s_drawDoneVisibilityDeferralEnabled.load(std::memory_order_acquire),
			.mode = s_guestFeedbackMode.load(std::memory_order_relaxed),
		};
	}

	void GX2WaitForVsync()
	{
		__OSLockScheduler();
		g_vsyncThreadQueue.GetPtr()->queueAndWait(coreinit::OSGetCurrentThread());
		__OSUnlockScheduler();
	}

	void GX2WaitForFlip()
	{
		if ((sint32)(_swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE) == _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE)))
			return; // dont wait if no flip is requested
		__OSLockScheduler();
		g_flipThreadQueue.GetPtr()->queueAndWait(coreinit::OSGetCurrentThread());
		__OSUnlockScheduler();
	}

	bool GX2DrawDone()
	{
		SPATIAL_PROFILER_AUTO_SCOPE_NAME("gx2.guest.draw_done");
		const uint32 drawDoneSequence = s_drawDoneSequence.fetch_add(1, std::memory_order_relaxed) + 1;
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		const uint32 guestLr = hCPU ? hCPU->spr.LR : 0;
		GuestProfiler::RecordGx2DrawDone(hCPU);
		s_drawDoneVisibilityLastGuestLr.store(guestLr, std::memory_order_relaxed);
		const bool deferVisibility =
			s_drawDoneVisibilityDeferralEnabled.load(std::memory_order_acquire) &&
			guestLr == s_drawDoneVisibilityDeferralGuestLr.load(std::memory_order_relaxed);
		const bool feedbackBoundary =
			s_guestFeedbackEnabled.load(std::memory_order_acquire) &&
			guestLr == s_guestFeedbackGuestLr.load(std::memory_order_relaxed);
		const bool feedbackEnabled = s_guestFeedbackEnabled.load(std::memory_order_relaxed);
		const uint32 feedbackFrameId = feedbackBoundary
			? s_guestFeedbackFrameId.fetch_add(1, std::memory_order_relaxed) + 1
			: s_guestFeedbackFrameId.load(std::memory_order_relaxed);
		const uint32 drawDonePhase = feedbackBoundary ? 1 : (feedbackEnabled ? 2 : 0);
		s_guestFeedbackLastGuestLr.store(guestLr, std::memory_order_relaxed);
		const uint32 feedbackPacketMode = feedbackBoundary
			? s_guestFeedbackMode.load(std::memory_order_relaxed) + 1
			: 0;
		{
			SPATIAL_PROFILER_AUTO_SCOPE_NAME("gx2.guest.draw_done.enqueue_visibility_barrier");
			// optional force full sync (texture readback and occlusion queries)
			bool forceFullSync = false;
			if (g_renderer && g_renderer->GetType() == RendererAPI::Vulkan)
				forceFullSync = true;
			if (!deferVisibility && (forceFullSync || ActiveSettings::WaitForGX2DrawDoneEnabled()))
			{
				GX2ReserveCmdSpace(4);
				// write PM4 command
				gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_SYNC_ASYNC_OPERATIONS, 3));
				gx2WriteGather_submitU32AsBE(feedbackPacketMode);
				gx2WriteGather_submitU32AsBE(feedbackFrameId);
				gx2WriteGather_submitU32AsBE(drawDoneSequence);
				if (feedbackBoundary)
				{
					const uint64 boundaryCount = s_guestFeedbackBoundaryCount.fetch_add(1, std::memory_order_relaxed) + 1;
					SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.enqueued_boundaries", boundaryCount,
						"Cemu Guest Feedback", "boundaries");
				}
			}
		}
		if (deferVisibility)
		{
			SPATIAL_PROFILER_AUTO_SCOPE_NAME("gx2.guest.draw_done.defer_guest_memory_visibility");
			const uint64 deferred = s_drawDoneVisibilityDeferredCount.fetch_add(1, std::memory_order_relaxed) + 1;
			SPATIAL_PROFILER_COUNTER_SET("cemu.host.draw_done_visibility.deferred", deferred,
				"Cemu Guest Visibility", "barriers");
		}
		else if (s_drawDoneVisibilityDeferralEnabled.load(std::memory_order_relaxed))
		{
			const uint64 normal = s_drawDoneVisibilityNormalCount.fetch_add(1, std::memory_order_relaxed) + 1;
			SPATIAL_PROFILER_COUNTER_SET("cemu.host.draw_done_visibility.normal", normal,
				"Cemu Guest Visibility", "barriers");
		}
		SPATIAL_PROFILER_COUNTER_SET("cemu.host.draw_done_visibility.last_guest_lr", guestLr,
			"Cemu Guest Visibility", "address");
		{
			SPATIAL_PROFILER_AUTO_SCOPE_NAME("gx2.guest.draw_done.submit");
			// flush pipeline
			GX2Command_Flush(0x100, true);
		}

		uint64 ts = GX2GetLastSubmittedTimeStamp();
		const uint64 initialRetired = GX2GetRetiredTimeStamp();
		const auto waitStart = std::chrono::steady_clock::now();
		bool waitResult = false;
		{
			if (feedbackBoundary)
			{
				SPATIAL_PROFILER_AUTO_SCOPE_NAME("gx2.guest.draw_done.wait_retirement.feedback_boundary");
				waitResult = GX2WaitTimeStamp(ts);
			}
			else if (feedbackEnabled)
			{
				SPATIAL_PROFILER_AUTO_SCOPE_NAME("gx2.guest.draw_done.wait_retirement.post_feedback");
				waitResult = GX2WaitTimeStamp(ts);
			}
			else
			{
				SPATIAL_PROFILER_AUTO_SCOPE_NAME("gx2.guest.draw_done.wait_retirement.generic");
				waitResult = GX2WaitTimeStamp(ts);
			}
		}
		const uint64 finalRetired = GX2GetRetiredTimeStamp();
		const uint64 waitUs = static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - waitStart).count());
		SPATIAL_PROFILER_COUNTER_SET("cemu.sync.draw_done.frame_id", feedbackFrameId, "Cemu Sync DrawDone", "frame");
		SPATIAL_PROFILER_COUNTER_SET("cemu.sync.draw_done.sequence", drawDoneSequence, "Cemu Sync DrawDone", "sequence");
		SPATIAL_PROFILER_COUNTER_SET("cemu.sync.draw_done.phase", drawDonePhase, "Cemu Sync DrawDone", "enum");
		SPATIAL_PROFILER_COUNTER_SET("cemu.sync.draw_done.guest_lr", guestLr, "Cemu Sync DrawDone", "address");
		SPATIAL_PROFILER_COUNTER_SET("cemu.sync.draw_done.target_retirement", ts, "Cemu Sync DrawDone", "timestamp");
		SPATIAL_PROFILER_COUNTER_SET("cemu.sync.draw_done.initial_retired", initialRetired, "Cemu Sync DrawDone", "timestamp");
		SPATIAL_PROFILER_COUNTER_SET("cemu.sync.draw_done.final_retired", finalRetired, "Cemu Sync DrawDone", "timestamp");
		SPATIAL_PROFILER_COUNTER_SET("cemu.sync.draw_done.gap_before", ts > initialRetired ? ts - initialRetired : 0,
			"Cemu Sync DrawDone", "submissions");
		SPATIAL_PROFILER_COUNTER_SET("cemu.sync.draw_done.gap_after", ts > finalRetired ? ts - finalRetired : 0,
			"Cemu Sync DrawDone", "submissions");
		SPATIAL_PROFILER_COUNTER_SET("cemu.sync.draw_done.wait_us", waitUs, "Cemu Sync DrawDone", "us");
		return waitResult;
	}

	void GX2Init_event()
	{
		// clear queue

		// launch event callback thread
		if (s_callbackThreadLaunched)
			return;
		s_callbackThreadLaunched = true;
		strcpy(s_eventCallbackThreadName.GetPtr(), "GX2 event callback");
		coreinit::OSCreateThreadType(s_eventCallbackThread, PPCInterpreter_makeCallableExportDepr(__GX2CallbackThread), 0, nullptr, (uint8*)s_eventCallbackThreadStack.GetPtr() + s_eventCallbackThreadStack.GetByteSize(), (sint32)s_eventCallbackThreadStack.GetByteSize(), 16, OSThread_t::ATTR_DETACHED, OSThread_t::THREAD_TYPE::TYPE_IO);
		coreinit::OSSetThreadName(s_eventCallbackThread, s_eventCallbackThreadName);
		coreinit::OSResumeThread(s_eventCallbackThread);
	}

	void GX2EventInit()
	{
		cafeExportRegister("gx2", GX2SetGPUFence, LogType::GX2);
		cafeExportRegister("gx2", GX2SubmitUserTimeStamp, LogType::GX2);

		cafeExportRegister("gx2", GX2SetEventCallback, LogType::GX2);
		cafeExportRegister("gx2", GX2GetEventCallback, LogType::GX2);

		cafeExportRegister("gx2", GX2WaitForVsync, LogType::GX2);
		cafeExportRegister("gx2", GX2WaitForFlip, LogType::GX2);
		cafeExportRegister("gx2", GX2DrawDone, LogType::GX2);

		coreinit::OSInitThreadQueue(g_vsyncThreadQueue.GetPtr());
		coreinit::OSInitThreadQueue(g_flipThreadQueue.GetPtr());

		coreinit::OSInitSemaphore(s_eventCbQueueSemaphore, 0);
	}

    void GX2EventResetToDefaultState()
    {
		ResetDisplayOrdinalDependency();
		ResetDrawDoneVisibilityDeferral();
		ResetGuestFeedbackPolicy();
        s_callbackThreadLaunched = false;
        for(auto& it : s_eventCallback)
        {
            it.callbackFuncPtr = nullptr;
            it.userData = nullptr;
        }
    }
}

void gx2Export_hook_RegisterDisplayOrdinalCounter(PPCInterpreter_t* hCPU)
{
	const uint32 virtualAddr = hCPU->gpr[3] & ~3u;
	if (virtualAddr == 0)
	{
		ResetDisplayOrdinalDependency();
		cemuLog_log(LogType::Force, "Display ordinal dependency disabled by Guest patch");
		osLib_returnFromFunction(hCPU, 1);
		return;
	}

	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	const uint16 titleVersion = CafeSystem::GetForegroundTitleVersion();
	if (titleId != kBotWEventTitleIdJp || titleVersion != kBotWEventVersion)
	{
		cemuLog_log(LogType::Force,
			"Display ordinal dependency rejected for title {:016x} v{}; expected BotW JP v{}",
			titleId, titleVersion, kBotWEventVersion);
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	ResetDisplayOrdinalDependency();
	const uint32 physAddr = memory_virtualToPhysical(virtualAddr) & ~3u;
	s_displayOrdinalCounterPhysAddr.store(physAddr, std::memory_order_relaxed);
	s_displayOrdinalRegisterCount.fetch_add(1, std::memory_order_relaxed);
	s_displayOrdinalDependencyEnabled.store(true, std::memory_order_release);
	SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.enabled", 1, "Cemu Display Dependency", "bool");
	SPATIAL_PROFILER_COUNTER_SET("cemu.host.display_ordinal.phys_addr", physAddr, "Cemu Display Dependency", "address");
	cemuLog_log(LogType::Force,
		"Display ordinal dependency enabled for BotW JP v{} counter {:08x} (physical {:08x})",
		titleVersion, virtualAddr, physAddr);
	osLib_returnFromFunction(hCPU, 1);
}

void gx2Export_hook_RegisterDrawDoneVisibilityDeferral(PPCInterpreter_t* hCPU)
{
	const uint32 guestLr = hCPU->gpr[3] & ~3u;
	if (guestLr == 0)
	{
		ResetDrawDoneVisibilityDeferral();
		cemuLog_log(LogType::Force, "DrawDone visibility deferral disabled by Guest patch");
		osLib_returnFromFunction(hCPU, 1);
		return;
	}

	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	const uint16 titleVersion = CafeSystem::GetForegroundTitleVersion();
	if (titleId != kBotWEventTitleIdJp || titleVersion != kBotWEventVersion)
	{
		cemuLog_log(LogType::Force,
			"DrawDone visibility deferral rejected for title {:016x} v{}; expected BotW JP v{}",
			titleId, titleVersion, kBotWEventVersion);
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	ResetDrawDoneVisibilityDeferral();
	s_drawDoneVisibilityDeferralGuestLr.store(guestLr, std::memory_order_relaxed);
	s_drawDoneVisibilityDeferralRegisterCount.fetch_add(1, std::memory_order_relaxed);
	s_drawDoneVisibilityDeferralEnabled.store(true, std::memory_order_release);
	SPATIAL_PROFILER_COUNTER_SET("cemu.host.draw_done_visibility.enabled", 1,
		"Cemu Guest Visibility", "bool");
	SPATIAL_PROFILER_COUNTER_SET("cemu.host.draw_done_visibility.guest_lr", guestLr,
		"Cemu Guest Visibility", "address");
	cemuLog_log(LogType::Force,
		"DrawDone visibility deferral enabled for BotW JP v{} Guest LR {:08x}",
		titleVersion, guestLr);
	osLib_returnFromFunction(hCPU, 1);
}

void gx2Export_hook_RegisterGuestFeedbackPolicy(PPCInterpreter_t* hCPU)
{
	const uint32 guestLr = hCPU->gpr[3] & ~3u;
	const uint32 policyVersion = hCPU->gpr[4];
	const uint32 mode = hCPU->gpr[5];
	if (guestLr == 0)
	{
		ResetGuestFeedbackPolicy();
		cemuLog_log(LogType::Force, "Guest feedback policy disabled by Guest patch");
		osLib_returnFromFunction(hCPU, 1);
		return;
	}
	if (policyVersion != kGuestFeedbackPolicyVersion)
	{
		cemuLog_log(LogType::Force, "Guest feedback policy version {} rejected; supported version is {}",
			policyVersion, kGuestFeedbackPolicyVersion);
		osLib_returnFromFunction(hCPU, 0);
		return;
	}
	if (mode > 1)
	{
		cemuLog_log(LogType::Force, "Guest feedback mode {} rejected; supported modes are 0 and 1", mode);
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	ResetGuestFeedbackPolicy();
	s_guestFeedbackGuestLr.store(guestLr, std::memory_order_relaxed);
	s_guestFeedbackPolicyVersion.store(policyVersion, std::memory_order_relaxed);
	s_guestFeedbackMode.store(mode, std::memory_order_relaxed);
	s_guestFeedbackRegisterCount.fetch_add(1, std::memory_order_relaxed);
	s_guestFeedbackEnabled.store(true, std::memory_order_release);
	SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.enabled", 1, "Cemu Guest Feedback", "bool");
	SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.guest_lr", guestLr, "Cemu Guest Feedback", "address");
	SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.policy_version", policyVersion, "Cemu Guest Feedback", "version");
	SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.mode", mode, "Cemu Guest Feedback", "enum");
	cemuLog_log(LogType::Force, "Guest feedback policy v{} mode {} registered for DrawDone Guest LR {:08x}",
		policyVersion, mode == 1 ? "guarded_previous_generation" : "observe_full_visibility", guestLr);
	osLib_returnFromFunction(hCPU, 1);
}
