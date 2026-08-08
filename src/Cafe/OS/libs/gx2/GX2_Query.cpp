#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "GX2.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/CafeSystem.h"
#include "GX2_Query.h"

#include "spatial/profiler/Profiler.h"

#include <atomic>
#include <mutex>
#include <sstream>
#include <unordered_map>

#define LATTE_GC_NUM_RB							2
#define _QUERY_REG_COUNT						8 // each reg/result is 64bits, little endian

namespace GX2
{
	struct GX2Query
	{
		// 4*2 sets of uint64 results
		uint32 reg[_QUERY_REG_COUNT * 2];
	};

	static_assert(sizeof(GX2Query) == 0x40);

	namespace
	{
		std::mutex s_queryTypeMutex;
		std::unordered_map<const GX2Query*, uint32> s_queryTypes;
		std::atomic<uint64> s_getCalls{};
		std::atomic<uint64> s_getCpuCalls{};
		std::atomic<uint64> s_getGpuCalls{};
		std::atomic<uint64> s_getUnknownCalls{};
		std::atomic<uint64> s_getNotReady{};
		std::atomic<uint64> s_getReadyZero{};
		std::atomic<uint64> s_getReadyNonzero{};
		std::atomic<uint64> s_conditionalBeginCalls{};
		std::atomic<uint64> s_conditionalEndCalls{};
		std::atomic<uint64> s_conditionalCpuCalls{};
		std::atomic<uint64> s_conditionalGpuCalls{};
		std::atomic<uint64> s_conditionalUnknownCalls{};
		std::atomic<uint64> s_conditionalDontWaitCalls{};
		std::atomic<uint64> s_conditionalPixelsMustPassCalls{};

		uint32 GetRecordedQueryType(const GX2Query* query)
		{
			std::scoped_lock lock{s_queryTypeMutex};
			const auto it = s_queryTypes.find(query);
			return it != s_queryTypes.end() ? it->second : UINT32_MAX;
		}

	}

	void GX2PublishOcclusionQueryConsumerCounters()
	{
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_get.calls", s_getCalls.load(std::memory_order_relaxed),
			"Cemu Guest Query Consumer", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_get.cpu_calls", s_getCpuCalls.load(std::memory_order_relaxed),
			"Cemu Guest Query Consumer", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_get.gpu_calls", s_getGpuCalls.load(std::memory_order_relaxed),
			"Cemu Guest Query Consumer", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_get.unknown_calls", s_getUnknownCalls.load(std::memory_order_relaxed),
			"Cemu Guest Query Consumer", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_get.not_ready", s_getNotReady.load(std::memory_order_relaxed),
			"Cemu Guest Query Consumer", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_get.ready_zero", s_getReadyZero.load(std::memory_order_relaxed),
			"Cemu Guest Query Consumer", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_get.ready_nonzero", s_getReadyNonzero.load(std::memory_order_relaxed),
			"Cemu Guest Query Consumer", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_predication.begin_calls", s_conditionalBeginCalls.load(std::memory_order_relaxed),
			"Cemu Guest Query Predication", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_predication.end_calls", s_conditionalEndCalls.load(std::memory_order_relaxed),
			"Cemu Guest Query Predication", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_predication.cpu_calls", s_conditionalCpuCalls.load(std::memory_order_relaxed),
			"Cemu Guest Query Predication", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_predication.gpu_calls", s_conditionalGpuCalls.load(std::memory_order_relaxed),
			"Cemu Guest Query Predication", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_predication.unknown_calls", s_conditionalUnknownCalls.load(std::memory_order_relaxed),
			"Cemu Guest Query Predication", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_predication.dont_wait_calls", s_conditionalDontWaitCalls.load(std::memory_order_relaxed),
			"Cemu Guest Query Predication", "calls");
		SPATIAL_PROFILER_COUNTER_SET("cemu.guest.query_predication.pixels_must_pass_calls", s_conditionalPixelsMustPassCalls.load(std::memory_order_relaxed),
			"Cemu Guest Query Predication", "calls");
	}

	void _BeginOcclusionQuery(GX2Query* queryInfo, bool isGPUQuery)
	{
		if (isGPUQuery)
		{
			uint64 titleId = CafeSystem::GetForegroundTitleId();
			if (titleId == 0x00050000101c4c00ULL || titleId == 0x00050000101c4d00 || titleId == 0x0005000010116100) // XCX EU, US, JPN
			{
				// in XCX queries are used to determine if certain objects are visible
				// if we are not setting the result fast enough and the query still holds a value of 0 (which is the default for GPU queries)
				// then XCX will not render affected objects, causing flicker
				// note: This is a very old workaround. It may no longer be necessary since the introduction of full sync. Investigate
				*(uint64*)(queryInfo->reg + 2) = 0x100000;
			}
			else
			{
				GX2ReserveCmdSpace(5 * _QUERY_REG_COUNT);
				MPTR queryInfoPhys = memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(queryInfo));
				for (sint32 i = 0; i < _QUERY_REG_COUNT; i++)
				{
					gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_MEM_WRITE, 4));
					gx2WriteGather_submitU32AsBE((queryInfoPhys + i * 8) | 0x2);
					gx2WriteGather_submitU32AsBE(0x20000); // 0x20000 -> ?
					uint32 v = 0;
					if (i >= LATTE_GC_NUM_RB * 2)
						v |= 0x80000000;
					gx2WriteGather_submitU32AsBE(0);
					gx2WriteGather_submitU32AsBE(v);
				}
			}
		}
		else
		{
			memset(queryInfo, 0, 0x10); // size maybe GPU7_GC_NUM_RB*2*4 ?
			queryInfo->reg[LATTE_GC_NUM_RB * 4 + 0] = 0;
			queryInfo->reg[LATTE_GC_NUM_RB * 4 + 1] = _swapEndianU32('OCPU');
		}
		// todo: Set mmDB_RENDER_CONTROL
	}

	void GX2QueryBegin(uint32 queryType, GX2Query* query)
	{
		{
			std::scoped_lock lock{s_queryTypeMutex};
			s_queryTypes[query] = queryType;
		}
		if (queryType == GX2_QUERY_TYPE_OCCLUSION_CPU)
		{
			_BeginOcclusionQuery(query, false);
		}
		else if (queryType == GX2_QUERY_TYPE_OCCLUSION_GPU)
		{
			_BeginOcclusionQuery(query, true);
		}
		else
		{
			debug_printf("GX2QueryBegin(): Unsupported type %d\n", queryType);
			debugBreakpoint();
			return;
		}
		// HLE packet
		GX2ReserveCmdSpace(2);
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_BEGIN_OCCLUSION_QUERY, 1));
		gx2WriteGather_submitU32AsBE(MEMPTR<GX2Query>(query).GetMPTR());
	}

	void GX2QueryEnd(uint32 queryType, GX2Query* query)
	{
		GX2ReserveCmdSpace(2);
		if (queryType == GX2_QUERY_TYPE_OCCLUSION_CPU || queryType == GX2_QUERY_TYPE_OCCLUSION_GPU)
		{
			// HLE packet
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_END_OCCLUSION_QUERY, 1));
			gx2WriteGather_submitU32AsBE(MEMPTR<GX2Query>(query).GetMPTR());
		}
		else
		{
			debug_printf("GX2QueryBegin(): Unsupported %d\n", queryType);
			debugBreakpoint();
			return;
		}
	}

	uint32 GX2QueryGetOcclusionResult(GX2Query* query, uint64be* resultOut)
	{
		s_getCalls.fetch_add(1, std::memory_order_relaxed);
		const uint32 queryType = GetRecordedQueryType(query);
		if (queryType == GX2_QUERY_TYPE_OCCLUSION_CPU)
			s_getCpuCalls.fetch_add(1, std::memory_order_relaxed);
		else if (queryType == GX2_QUERY_TYPE_OCCLUSION_GPU)
			s_getGpuCalls.fetch_add(1, std::memory_order_relaxed);
		else
			s_getUnknownCalls.fetch_add(1, std::memory_order_relaxed);
		if (query->reg[LATTE_GC_NUM_RB * 4 + 1] == _swapEndianU32('OCPU') && query->reg[LATTE_GC_NUM_RB * 4 + 0] == 0)
		{
			// CPU query result not ready
			s_getNotReady.fetch_add(1, std::memory_order_relaxed);
			return GX2_FALSE;
		}

		uint64 startValue = *(uint64*)(query->reg + 0);
		uint64 endValue = *(uint64*)(query->reg + 2);
		if ((startValue & 0x8000000000000000ULL) || (endValue & 0x8000000000000000ULL))
		{
			s_getNotReady.fetch_add(1, std::memory_order_relaxed);
			return GX2_FALSE;
		}
		const uint64 result = endValue - startValue;
		*resultOut = result;
		if (result == 0)
			s_getReadyZero.fetch_add(1, std::memory_order_relaxed);
		else
			s_getReadyNonzero.fetch_add(1, std::memory_order_relaxed);
		return GX2_TRUE;
	}

	void GX2QueryBeginConditionalRender(uint32 queryType, GX2Query* query, uint32 dontWaitBool, uint32 pixelsMustPassBool)
	{
		s_conditionalBeginCalls.fetch_add(1, std::memory_order_relaxed);
		if (queryType == GX2_QUERY_TYPE_OCCLUSION_CPU)
			s_conditionalCpuCalls.fetch_add(1, std::memory_order_relaxed);
		else if (queryType == GX2_QUERY_TYPE_OCCLUSION_GPU)
			s_conditionalGpuCalls.fetch_add(1, std::memory_order_relaxed);
		else
			s_conditionalUnknownCalls.fetch_add(1, std::memory_order_relaxed);
		if (dontWaitBool != 0)
			s_conditionalDontWaitCalls.fetch_add(1, std::memory_order_relaxed);
		if (pixelsMustPassBool != 0)
			s_conditionalPixelsMustPassCalls.fetch_add(1, std::memory_order_relaxed);
		GX2ReserveCmdSpace(3);

		uint32 flags = 0;
		if (pixelsMustPassBool)
			flags |= (1<<31);
		if (queryType == GX2_QUERY_TYPE_OCCLUSION_GPU)
			flags |= (1 << 13);
		else
			flags |= (2 << 13);

		flags |= ((dontWaitBool != 0) << 19);

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_PREDICATION, 2));
		gx2WriteGather_submitU32AsBE(memory_virtualToPhysical(MEMPTR<GX2Query>(query).GetMPTR()));
		gx2WriteGather_submitU32AsBE(flags);
	}

	void GX2QueryEndConditionalRender()
	{
		s_conditionalEndCalls.fetch_add(1, std::memory_order_relaxed);
		GX2ReserveCmdSpace(3);

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_PREDICATION, 2));
		gx2WriteGather_submitU32AsBE(MPTR_NULL);
		gx2WriteGather_submitU32AsBE(0); // unknown / todo
	}

	void GX2QueryInit()
	{
		{
			std::scoped_lock lock{s_queryTypeMutex};
			s_queryTypes.clear();
		}
		s_getCalls.store(0, std::memory_order_relaxed);
		s_getCpuCalls.store(0, std::memory_order_relaxed);
		s_getGpuCalls.store(0, std::memory_order_relaxed);
		s_getUnknownCalls.store(0, std::memory_order_relaxed);
		s_getNotReady.store(0, std::memory_order_relaxed);
		s_getReadyZero.store(0, std::memory_order_relaxed);
		s_getReadyNonzero.store(0, std::memory_order_relaxed);
		s_conditionalBeginCalls.store(0, std::memory_order_relaxed);
		s_conditionalEndCalls.store(0, std::memory_order_relaxed);
		s_conditionalCpuCalls.store(0, std::memory_order_relaxed);
		s_conditionalGpuCalls.store(0, std::memory_order_relaxed);
		s_conditionalUnknownCalls.store(0, std::memory_order_relaxed);
		s_conditionalDontWaitCalls.store(0, std::memory_order_relaxed);
		s_conditionalPixelsMustPassCalls.store(0, std::memory_order_relaxed);
		GX2PublishOcclusionQueryConsumerCounters();
		cafeExportRegister("gx2", GX2QueryBegin, LogType::GX2);
		cafeExportRegister("gx2", GX2QueryEnd, LogType::GX2);
		cafeExportRegister("gx2", GX2QueryGetOcclusionResult, LogType::GX2);
		cafeExportRegister("gx2", GX2QueryBeginConditionalRender, LogType::GX2);
		cafeExportRegister("gx2", GX2QueryEndConditionalRender, LogType::GX2);
	}

	std::string GX2GetOcclusionQueryConsumerStatus()
	{
		std::ostringstream out;
		out << "occlusion_query_consumer:\n";
		out << "get_calls=" << s_getCalls.load(std::memory_order_relaxed) << "\n";
		out << "get_cpu_calls=" << s_getCpuCalls.load(std::memory_order_relaxed) << "\n";
		out << "get_gpu_calls=" << s_getGpuCalls.load(std::memory_order_relaxed) << "\n";
		out << "get_unknown_calls=" << s_getUnknownCalls.load(std::memory_order_relaxed) << "\n";
		out << "get_not_ready=" << s_getNotReady.load(std::memory_order_relaxed) << "\n";
		out << "get_ready_zero=" << s_getReadyZero.load(std::memory_order_relaxed) << "\n";
		out << "get_ready_nonzero=" << s_getReadyNonzero.load(std::memory_order_relaxed) << "\n";
		out << "conditional_begin_calls=" << s_conditionalBeginCalls.load(std::memory_order_relaxed) << "\n";
		out << "conditional_end_calls=" << s_conditionalEndCalls.load(std::memory_order_relaxed) << "\n";
		out << "conditional_cpu_calls=" << s_conditionalCpuCalls.load(std::memory_order_relaxed) << "\n";
		out << "conditional_gpu_calls=" << s_conditionalGpuCalls.load(std::memory_order_relaxed) << "\n";
		out << "conditional_unknown_calls=" << s_conditionalUnknownCalls.load(std::memory_order_relaxed) << "\n";
		out << "conditional_dont_wait_calls=" << s_conditionalDontWaitCalls.load(std::memory_order_relaxed) << "\n";
		out << "conditional_pixels_must_pass_calls=" << s_conditionalPixelsMustPassCalls.load(std::memory_order_relaxed) << "\n";
		out << "host_conditional_render_culling=false\n";
		return out.str();
	}
};
