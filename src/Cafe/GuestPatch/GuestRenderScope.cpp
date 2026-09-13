#include "Cafe/GuestPatch/GuestRenderScope.h"

#include <fmt/format.h>

#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"

// Producer runs on the GX2 submission thread; consumer runs on the LatteThread.
// The two never share mutable state directly -- identity travels through the
// ordered command stream (spec 9). The small in-flight cap here bounds producer
// resources; the actual ordering guarantee comes from the command stream.

namespace GuestPatch
{
	// v1 limits (spec 9): 8 in-flight frames * 16 scope depth per stream.
	static constexpr uint32 kMaxInFlightScopes = 8 * 16;

	RenderScopeProducer& RenderScopeProducer::Instance()
	{
		static RenderScopeProducer s;
		return s;
	}

	ScopeEmitResult RenderScopeProducer::Begin(const RenderScopeDesc& desc,
											   uint32& scopeIdOut)
	{
		if (desc.viewIndex > 2)
			return ScopeEmitResult::InvalidArgument;
		if (m_inFlight >= kMaxInFlightScopes)
			return ScopeEmitResult::QueueFull;

		const uint32 scopeId = m_nextScopeId++;
		if (m_nextScopeId == 0)
			m_nextScopeId = 1;

		const uint32 control = desc.viewIndex & IT_HLE_GUEST_RENDER_SCOPE_VIEW_MASK;
		GX2::GuestGpuTagEmitResult r = GX2::GX2EmitGuestRenderScope(
			/*begin*/true, control, scopeId, desc.moduleGeneration, desc.titleEpoch,
			(uint32)(desc.frameId & 0xFFFFFFFF), (uint32)(desc.frameId >> 32),
			desc.phase, desc.poseSnapshotId);
		switch (r)
		{
		case GX2::GuestGpuTagEmitResult::Emitted:
			m_inFlight++;
			m_emitted++;
			scopeIdOut = scopeId;
			return ScopeEmitResult::Emitted;
		case GX2::GuestGpuTagEmitResult::NoCommandBuffer:
			return ScopeEmitResult::NoCommandBuffer;
		case GX2::GuestGpuTagEmitResult::DisplayList:
			return ScopeEmitResult::DisplayList;
		}
		return ScopeEmitResult::NoCommandBuffer;
	}

	ScopeEmitResult RenderScopeProducer::End(uint32 scopeId, uint64 frameId,
											 uint32 viewIndex)
	{
		if (viewIndex > 2)
			return ScopeEmitResult::InvalidArgument;
		const uint32 control = viewIndex & IT_HLE_GUEST_RENDER_SCOPE_VIEW_MASK;
		GX2::GuestGpuTagEmitResult r = GX2::GX2EmitGuestRenderScope(
			/*begin*/false, control, scopeId, /*generation*/0, /*titleEpoch*/0,
			(uint32)(frameId & 0xFFFFFFFF), (uint32)(frameId >> 32),
			/*phase*/0, /*poseSnapshotId*/0);
		switch (r)
		{
		case GX2::GuestGpuTagEmitResult::Emitted:
			if (m_inFlight > 0)
				m_inFlight--;
			m_retired++;
			return ScopeEmitResult::Emitted;
		case GX2::GuestGpuTagEmitResult::NoCommandBuffer:
			return ScopeEmitResult::NoCommandBuffer;
		case GX2::GuestGpuTagEmitResult::DisplayList:
			return ScopeEmitResult::DisplayList;
		}
		return ScopeEmitResult::NoCommandBuffer;
	}

	void RenderScopeProducer::Reset()
	{
		m_nextScopeId = 1;
		m_inFlight = 0;
		m_emitted = 0;
		m_retired = 0;
	}

	RenderScopeConsumer& RenderScopeConsumer::Instance()
	{
		static RenderScopeConsumer s;
		return s;
	}

	void RenderScopeConsumer::OnBegin(const RenderScopeDesc& desc)
	{
		// A new Begin replaces the active scope context. Nested begins without an
		// intervening end are permitted only as a stream-ordered replacement in
		// v1; draws consume whatever the most recent Begin established.
		m_active = desc;
		m_hasActive = true;
		m_consumedBegin++;
	}

	void RenderScopeConsumer::OnEnd(uint32 scopeId, uint64 frameId, uint32 viewIndex)
	{
		m_consumedEnd++;
		if (!m_hasActive)
		{
			// End without an active scope: unknown context. Recover to mono
			// rather than inheriting a stale right-eye state (spec 9).
			m_errors++;
			return;
		}
		if (m_active.scopeId != scopeId)
		{
			// Mismatched end (lost/duplicate). Count it and drop back to no
			// active scope so continued draws do not use the wrong eye.
			m_errors++;
		}
		m_hasActive = false;
	}

	void RenderScopeConsumer::Reset()
	{
		m_active = {};
		m_hasActive = false;
		m_consumedBegin = 0;
		m_consumedEnd = 0;
		m_errors = 0;
	}

	std::string RenderScopeStatus()
	{
		auto& prod = RenderScopeProducer::Instance();
		auto& cons = RenderScopeConsumer::Instance();
		std::string out = "render_scope\n";
		out += fmt::format("  producer: emitted={} retired={}\n",
						   prod.EmittedCount(), prod.RetiredCount());
		out += fmt::format("  consumer: begin={} end={} errors={} active={}\n",
						   cons.ConsumedBeginCount(), cons.ConsumedEndCount(),
						   cons.ErrorCount(), cons.HasActiveScope() ? "yes" : "no");
		if (cons.HasActiveScope())
		{
			const auto& a = cons.ActiveScope();
			out += fmt::format("  active: frame={} view={} phase={} scope={}\n",
							   a.frameId, a.viewIndex, a.phase, a.scopeId);
		}
		return out;
	}
}
