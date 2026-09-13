#pragma once

// GuestRenderScope -- ordered per-eye/frame render scope carried in the GX2 /
// Latte command stream (spec 9).
//
// Producer side (HLE, GX2 thread): render.begin_scope validates and copies an
// immutable ScopeDescV1, allocates a scope id, and emits an ordered Begin packet
// into the current main GX2 command stream. render.end_scope emits an ordered
// End packet. Nothing is driven by a shared current_eye variable; the eye/frame
// identity travels in the command stream so the Latte thread sees scope changes
// in submission order.
//
// Consumer side (LatteThread): the private PM4 opcode is decoded in submission
// order; the decoder switches the active scope context so subsequent draws carry
// the correct eye/frame/pose identity. This is deliberately the minimal protocol
// that a draw can actually carry -- not a general render-plan DAG.

#include <cstdint>
#include <string>

namespace GuestPatch
{
	// Mirrors cemu_sdk_v1::ScopeDescV1, decoded host-side.
	struct RenderScopeDesc
	{
		uint32 titleEpoch = 0;
		uint32 moduleGeneration = 0;
		uint64 frameId = 0;
		uint32 viewIndex = 0;   // 0=mono,1=left,2=right
		uint32 phase = 0;
		uint32 poseSnapshotId = 0;
		uint32 scopeId = 0;
	};

	enum class ScopeEmitResult : uint8
	{
		Emitted,
		NoCommandBuffer,
		DisplayList,
		QueueFull,
		InvalidArgument,
	};

	// Producer (GX2/HLE thread). Returns a scope id on begin.
	class RenderScopeProducer
	{
	public:
		static RenderScopeProducer& Instance();

		// Validate + freeze the descriptor, allocate a scope id, emit Begin.
		ScopeEmitResult Begin(const RenderScopeDesc& desc, uint32& scopeIdOut);
		// Emit End for a previously-begun scope id.
		ScopeEmitResult End(uint32 scopeId, uint64 frameId, uint32 viewIndex);

		// Counters for guest_patch_status.
		uint32 EmittedCount() const { return m_emitted; }
		uint32 RetiredCount() const { return m_retired; }

		void Reset();

	private:
		RenderScopeProducer() = default;
		uint32 m_nextScopeId = 1;
		uint32 m_inFlight = 0;
		uint32 m_emitted = 0;
		uint32 m_retired = 0;
	};

	// Consumer state (LatteThread). Holds the currently-active scope context so
	// draws can be tagged. Updated as Begin/End packets are decoded in order.
	class RenderScopeConsumer
	{
	public:
		static RenderScopeConsumer& Instance();

		void OnBegin(const RenderScopeDesc& desc);
		void OnEnd(uint32 scopeId, uint64 frameId, uint32 viewIndex);

		bool HasActiveScope() const { return m_hasActive; }
		const RenderScopeDesc& ActiveScope() const { return m_active; }

		uint32 ConsumedBeginCount() const { return m_consumedBegin; }
		uint32 ConsumedEndCount() const { return m_consumedEnd; }
		uint32 ErrorCount() const { return m_errors; }

		void Reset();

	private:
		RenderScopeConsumer() = default;
		RenderScopeDesc m_active{};
		bool m_hasActive = false;
		uint32 m_consumedBegin = 0;
		uint32 m_consumedEnd = 0;
		uint32 m_errors = 0;
	};

	std::string RenderScopeStatus();
}
