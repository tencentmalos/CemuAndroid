#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"

#include "Cafe/HW/Latte/Core/LatteQueryObject.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

#include "spatial/profiler/Profiler.h"

#include <atomic>
#include <sstream>

#define GPU7_QUERY_TYPE_OCCLUSION	(1)

uint64 queryEventCounter = 1;

struct LatteGX2QueryInformation
{
	MPTR queryMPTR;
	uint64 queryEventStart;
	uint64 queryEventEnd;
	uint64 sampleSum;
	bool queryEnded;
	bool isCpuQuery;
};

std::vector<LatteGX2QueryInformation> list_activeGX2Queries2;

std::vector<LatteQueryObject*> list_queriesInFlight;

uint64 latestQueryFinishedEventId = 0;

LatteQueryObject* _currentlyActiveRendererQuery = {0};

namespace
{
	std::atomic<LatteOcclusionQueryPolicy> s_requestedPolicy{LatteOcclusionQueryPolicy::AlwaysVisible};
	std::atomic<LatteOcclusionQueryPolicy> s_activePolicy{LatteOcclusionQueryPolicy::AlwaysVisible};
	std::atomic<uint64> s_cpuQueries{};
	std::atomic<uint64> s_gpuQueries{};
	std::atomic<uint64> s_completedQueries{};
	std::atomic<uint64> s_zeroResults{};
	std::atomic<uint64> s_nonzeroResults{};
	std::atomic<uint64> s_bypassedQueries{};
	std::atomic<uint64> s_resultSamples{};

	const char* GetPolicyName(LatteOcclusionQueryPolicy policy)
	{
		switch (policy)
		{
		case LatteOcclusionQueryPolicy::Accurate:
			return "accurate";
		case LatteOcclusionQueryPolicy::AlwaysVisible:
			return "always_visible";
		}
		return "unknown";
	}

	bool IsCpuQuery(MPTR queryMPTR)
	{
		const uint32* queryObjectData = reinterpret_cast<const uint32*>(memory_getPointerFromVirtualOffset(queryMPTR));
		return queryObjectData[8] == 0 && queryObjectData[9] == _swapEndianU32('OCPU');
	}

	void RecordCompletedQuery(const LatteGX2QueryInformation& query)
	{
		s_completedQueries.fetch_add(1, std::memory_order_relaxed);
		s_resultSamples.fetch_add(query.sampleSum, std::memory_order_relaxed);
		if (query.sampleSum == 0)
			s_zeroResults.fetch_add(1, std::memory_order_relaxed);
		else
			s_nonzeroResults.fetch_add(1, std::memory_order_relaxed);
	}

	void ApplyRequestedPolicyIfIdle()
	{
		if (_currentlyActiveRendererQuery != nullptr || !list_queriesInFlight.empty() || !list_activeGX2Queries2.empty())
			return;
		const LatteOcclusionQueryPolicy requested = s_requestedPolicy.load(std::memory_order_acquire);
		s_activePolicy.store(requested, std::memory_order_release);
	}
}

void LatteQuery_PublishProfilerCounters()
{
	SPATIAL_PROFILER_COUNTER_SET("cemu.query.policy", static_cast<uint32>(s_activePolicy.load(std::memory_order_relaxed)),
		"Cemu Occlusion Query", "enum");
	SPATIAL_PROFILER_COUNTER_SET("cemu.query.cpu", s_cpuQueries.load(std::memory_order_relaxed),
		"Cemu Occlusion Query", "queries");
	SPATIAL_PROFILER_COUNTER_SET("cemu.query.gpu", s_gpuQueries.load(std::memory_order_relaxed),
		"Cemu Occlusion Query", "queries");
	SPATIAL_PROFILER_COUNTER_SET("cemu.query.completed", s_completedQueries.load(std::memory_order_relaxed),
		"Cemu Occlusion Query", "queries");
	SPATIAL_PROFILER_COUNTER_SET("cemu.query.zero_results", s_zeroResults.load(std::memory_order_relaxed),
		"Cemu Occlusion Query", "queries");
	SPATIAL_PROFILER_COUNTER_SET("cemu.query.nonzero_results", s_nonzeroResults.load(std::memory_order_relaxed),
		"Cemu Occlusion Query", "queries");
	SPATIAL_PROFILER_COUNTER_SET("cemu.query.bypassed", s_bypassedQueries.load(std::memory_order_relaxed),
		"Cemu Occlusion Query", "queries");
}

LatteQueryVisibilitySnapshot LatteQuery_GetVisibilitySnapshot()
{
	return {
		.inFlightQueries = static_cast<uint32>(list_queriesInFlight.size()),
		.guestQueries = static_cast<uint32>(list_activeGX2Queries2.size()),
		.latestFinishedEventId = latestQueryFinishedEventId,
		.nextEventId = queryEventCounter,
		.rendererQueryActive = _currentlyActiveRendererQuery != nullptr,
	};
}

uint64 LatteQuery_getNextEventId()
{
	uint64 ev = queryEventCounter;
	queryEventCounter++;
	return ev;
}

void LatteQuery_begin(LatteQueryObject* queryObject, uint64 eventId)
{
	queryObject->queryEventStart = eventId;
	queryObject->begin();
}

void LatteQuery_end(LatteQueryObject* queryObject, uint64 eventId)
{
	cemu_assert_debug(!queryObject->queryEnded);
	queryObject->queryEnded = true;
	queryObject->queryEventEnd = eventId;
	queryObject->end();
}

LatteQueryObject* LatteQuery_createSamplePassedQuery()
{
	return g_renderer->occlusionQuery_create();
}

void LatteQuery_finishGX2Query(LatteGX2QueryInformation& gx2Query)
{
	uint32* queryObjectData = (uint32*)memory_getPointerFromVirtualOffset(gx2Query.queryMPTR);
	*(uint64*)(queryObjectData + 0) = 0;
	*(uint64*)(queryObjectData + 2) = gx2Query.sampleSum;
	*(uint64*)(queryObjectData + 4) = 0;
	*(uint64*)(queryObjectData + 6) = 0;

	*(uint64*)(queryObjectData + 8) = 0; // overwrites the 'OCPU' magic constant letting GX2QueryGetOcclusionResult know that the query is finished (for CPU queries)
	RecordCompletedQuery(gx2Query);
}

void LatteQuery_UpdateFinishedQueries()
{
	if (!list_queriesInFlight.empty())
		g_renderer->occlusionQuery_updateState();
	for(uint32 i=0; i<list_queriesInFlight.size(); i++)
	{
		LatteQueryObject* queryObject = list_queriesInFlight[i];
		cemu_assert_debug(queryObject->queryEnded);
		if( queryObject->queryEnded == false )
			continue;
		// check if result is available
		uint64 numSamplesPassed;
		if (!queryObject->getResult(numSamplesPassed))
			break;

		cemu_assert_debug(latestQueryFinishedEventId < queryObject->queryEventEnd);
		latestQueryFinishedEventId = queryObject->queryEventEnd;

		// add number of passed samples to all gx2 queries that were active at the time
		for (auto& it : list_activeGX2Queries2)
		{
			if (queryObject->queryEventStart >= it.queryEventStart && queryObject->queryEventEnd <= it.queryEventEnd)
				it.sampleSum += numSamplesPassed;
		}

		list_queriesInFlight.erase(list_queriesInFlight.begin() + i);
		i--;
		g_renderer->occlusionQuery_destroy(queryObject);
	}
	// check for finished GX2 queries
	for (sint32 i = 0; i < list_activeGX2Queries2.size(); i++)
	{
		auto& gx2Query = list_activeGX2Queries2[i];
		if (gx2Query.queryEnded && latestQueryFinishedEventId >= gx2Query.queryEventEnd)
		{
			LatteQuery_finishGX2Query(gx2Query);
			list_activeGX2Queries2.erase(list_activeGX2Queries2.begin() + i);
			i--;
		}
	}
	ApplyRequestedPolicyIfIdle();
}

void LatteQuery_UpdateFinishedQueriesForceFinishAll()
{
	cemu_assert_debug(_currentlyActiveRendererQuery == nullptr);
	if (list_queriesInFlight.empty())
	{
		ApplyRequestedPolicyIfIdle();
		return;
	}
	g_renderer->occlusionQuery_flush(); // guarantees that all query commands have been submitted and finished processing
	while (true)
	{
		LatteQuery_UpdateFinishedQueries();
		if (list_queriesInFlight.empty())
			break;
	}
}

sint32 checkQueriesCounter = 0;

void LatteQuery_endActiveRendererQuery(uint64 currentEventId)
{
	if (_currentlyActiveRendererQuery != nullptr)
	{
		LatteQuery_end(_currentlyActiveRendererQuery, currentEventId);
		list_queriesInFlight.emplace_back(_currentlyActiveRendererQuery);
		_currentlyActiveRendererQuery = nullptr;
	}
}

void LatteQuery_BeginOcclusionQuery(MPTR queryMPTR)
{
	ApplyRequestedPolicyIfIdle();
	if (s_activePolicy.load(std::memory_order_acquire) != LatteOcclusionQueryPolicy::Accurate)
	{
		checkQueriesCounter = 0;
	}
	else if (checkQueriesCounter < 7)
	{
		checkQueriesCounter++;
	}
	else
	{
		LatteQuery_UpdateFinishedQueries();
		checkQueriesCounter = 0;
	}

	for(auto& it : list_activeGX2Queries2)
	{
		if (it.queryMPTR == queryMPTR)
		{
			debug_printf("itHLEBeginOcclusionQuery: Query 0x%08x is already active\n", queryMPTR);
			return;
		}
	}
	uint64 currentEventId = LatteQuery_getNextEventId();
	// end any currently active query
	LatteQuery_endActiveRendererQuery(currentEventId);
	// create GX2 query binding
	LatteGX2QueryInformation queryBinding{};
	queryBinding.queryEventStart = currentEventId;
	queryBinding.queryMPTR = queryMPTR;
	queryBinding.isCpuQuery = IsCpuQuery(queryMPTR);
	if (queryBinding.isCpuQuery)
		s_cpuQueries.fetch_add(1, std::memory_order_relaxed);
	else
		s_gpuQueries.fetch_add(1, std::memory_order_relaxed);
	list_activeGX2Queries2.emplace_back(queryBinding);
	if (s_activePolicy.load(std::memory_order_acquire) == LatteOcclusionQueryPolicy::AlwaysVisible)
	{
		s_bypassedQueries.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	// start renderer query
	LatteQueryObject* queryObject = LatteQuery_createSamplePassedQuery();
	LatteQuery_begin(queryObject, currentEventId);
	_currentlyActiveRendererQuery = queryObject;
}

void LatteQuery_EndOcclusionQuery(MPTR queryMPTR)
{
	if (queryMPTR == MPTR_NULL)
		return;
	uint64 currentEventId = LatteQuery_getNextEventId();
	if (s_activePolicy.load(std::memory_order_acquire) == LatteOcclusionQueryPolicy::AlwaysVisible)
	{
		for (sint32 i = 0; i < list_activeGX2Queries2.size(); i++)
		{
			auto& query = list_activeGX2Queries2[i];
			if (query.queryMPTR != queryMPTR)
				continue;
			query.queryEventEnd = currentEventId;
			query.queryEnded = true;
			query.sampleSum = 1;
			latestQueryFinishedEventId = currentEventId;
			LatteQuery_finishGX2Query(query);
			list_activeGX2Queries2.erase(list_activeGX2Queries2.begin() + i);
			break;
		}
		ApplyRequestedPolicyIfIdle();
		return;
	}
	// mark query binding as ended
	for(auto& it : list_activeGX2Queries2)
	{
		if (it.queryMPTR == queryMPTR)
		{
			it.queryEventEnd = currentEventId;
			it.queryEnded = true;
			break;
		}
	}
	// end currently active renderer query
	LatteQuery_endActiveRendererQuery(currentEventId);
	// check if there are still active GX2 queries
	bool hasActiveGX2Query = false;
	for (auto& it : list_activeGX2Queries2)
	{
		if (!it.queryEnded)
		{
			hasActiveGX2Query = true;
			break;
		}
	}
	// start a new renderer query if there are still active GX2 queries
	if (hasActiveGX2Query)
	{
		LatteQueryObject* queryObject = LatteQuery_createSamplePassedQuery();
		LatteQuery_begin(queryObject, currentEventId);
		list_queriesInFlight.emplace_back(queryObject);
		_currentlyActiveRendererQuery = queryObject;
	}
}

void LatteQuery_CancelActiveGPU7Queries()
{
	cemu_assert_debug(_currentlyActiveRendererQuery == nullptr);
}

void LatteQuery_Init()
{
	s_requestedPolicy.store(LatteOcclusionQueryPolicy::AlwaysVisible, std::memory_order_relaxed);
	s_activePolicy.store(LatteOcclusionQueryPolicy::AlwaysVisible, std::memory_order_relaxed);
	s_cpuQueries.store(0, std::memory_order_relaxed);
	s_gpuQueries.store(0, std::memory_order_relaxed);
	s_completedQueries.store(0, std::memory_order_relaxed);
	s_zeroResults.store(0, std::memory_order_relaxed);
	s_nonzeroResults.store(0, std::memory_order_relaxed);
	s_bypassedQueries.store(0, std::memory_order_relaxed);
	s_resultSamples.store(0, std::memory_order_relaxed);
	LatteQuery_PublishProfilerCounters();
}

void LatteQuery_RequestPolicy(LatteOcclusionQueryPolicy policy)
{
	s_requestedPolicy.store(policy, std::memory_order_release);
}

LatteOcclusionQueryPolicySnapshot LatteQuery_GetPolicySnapshot()
{
	return {
		.requestedPolicy = s_requestedPolicy.load(std::memory_order_acquire),
		.activePolicy = s_activePolicy.load(std::memory_order_acquire),
		.cpuQueries = s_cpuQueries.load(std::memory_order_relaxed),
		.gpuQueries = s_gpuQueries.load(std::memory_order_relaxed),
		.completedQueries = s_completedQueries.load(std::memory_order_relaxed),
		.zeroResults = s_zeroResults.load(std::memory_order_relaxed),
		.nonzeroResults = s_nonzeroResults.load(std::memory_order_relaxed),
		.bypassedQueries = s_bypassedQueries.load(std::memory_order_relaxed),
		.resultSamples = s_resultSamples.load(std::memory_order_relaxed),
	};
}

std::string LatteQuery_GetPolicyStatus()
{
	const auto snapshot = LatteQuery_GetPolicySnapshot();
	std::ostringstream out;
	out << "occlusion_query_policy:\n";
	out << "requested=" << GetPolicyName(snapshot.requestedPolicy) << "\n";
	out << "active=" << GetPolicyName(snapshot.activePolicy) << "\n";
	out << "default=always_visible\n";
	out << "pending=" << (snapshot.requestedPolicy != snapshot.activePolicy ? "true" : "false") << "\n";
	out << "host_query_bypassed=" << (snapshot.activePolicy == LatteOcclusionQueryPolicy::AlwaysVisible ? "true" : "false") << "\n";
	out << "conservative_visible_result=1\n";
	out << "cpu_queries=" << snapshot.cpuQueries << "\n";
	out << "gpu_queries=" << snapshot.gpuQueries << "\n";
	out << "completed_queries=" << snapshot.completedQueries << "\n";
	out << "zero_results=" << snapshot.zeroResults << "\n";
	out << "nonzero_results=" << snapshot.nonzeroResults << "\n";
	out << "bypassed_queries=" << snapshot.bypassedQueries << "\n";
	out << "result_samples=" << snapshot.resultSamples << "\n";
	out << "conditional_render_host_culling=false\n";
	return out.str();
}
