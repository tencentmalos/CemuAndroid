#include "Cafe/GuestPatch/GuestPatchSdk.h"

#include <array>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "../../guest/custom/v1/include/cemu/sdk_v1.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/GuestPatch/GuestRenderScope.h"

namespace GuestPatch
{
	namespace
	{
		struct ContextRecord
		{
			uint32 titleEpoch = 0;
			uint32 moduleGeneration = 0;
			uint32 capabilities = 0;
			bool valid = false;
		};

		std::unordered_map<uint32, ContextRecord> s_contexts;
		uint32 s_nextHandle = 1;
		uint32 s_gatewayVa = 0;

		// Service call counters (for guest_patch_status).
		uint64 s_calls = 0;
		uint64 s_rejects = 0;

		float ReadBEFloat(uint32 va)
		{
			uint32 raw = memory_readU32(va);
			float f;
			std::memcpy(&f, &raw, sizeof(f));
			return f;
		}

		void WriteBEFloat(uint32 va, float f)
		{
			uint32 raw;
			std::memcpy(&raw, &f, sizeof(raw));
			memory_writeU32(va, raw);
		}

		bool GuestRangeValid(uint32 va, uint32 size)
		{
			if (size == 0)
				return true;
			if (va == 0)
				return false;
			if ((uint64)va + size > 0x100000000ull)
				return false;
			return memory_getPointerFromVirtualOffsetAllowNull(va) != nullptr &&
				   memory_getPointerFromVirtualOffsetAllowNull(va + size - 1) != nullptr;
		}

		using namespace cemu_sdk_v1;

		uint32 SvcQueryCaps(const ContextRecord& ctx, uint32 respVa, uint32 respCap)
		{
			if (respCap < sizeof(QueryCapsResponse) || !GuestRangeValid(respVa, sizeof(QueryCapsResponse)))
				return Status_InvalidArgument;
			memory_writeU32(respVa + offsetof(QueryCapsResponse, structSize), sizeof(QueryCapsResponse));
			memory_writeU32(respVa + offsetof(QueryCapsResponse, hostAbiVersion), kHostAbiVersion);
			memory_writeU32(respVa + offsetof(QueryCapsResponse, enabledCaps), ctx.capabilities);
			memory_writeU32(respVa + offsetof(QueryCapsResponse, maxRequestBytes), 64 * 1024);
			memory_writeU32(respVa + offsetof(QueryCapsResponse, titleEpoch), ctx.titleEpoch);
			memory_writeU32(respVa + offsetof(QueryCapsResponse, moduleGeneration), ctx.moduleGeneration);
			return Status_Ok;
		}

		uint32 SvcTransformBatch(const ContextRecord& ctx, uint32 reqVa, uint32 reqSize,
								 uint32 respVa, uint32 respCap)
		{
			if ((ctx.capabilities & Cap_MathTransform) == 0)
				return Status_Unsupported;
			if (reqSize < sizeof(TransformBatchRequest) || !GuestRangeValid(reqVa, reqSize))
				return Status_InvalidArgument;

			const uint32 structSize = memory_readU32(reqVa + offsetof(TransformBatchRequest, structSize));
			const uint32 count = memory_readU32(reqVa + offsetof(TransformBatchRequest, elementCount));
			const uint32 stride = memory_readU32(reqVa + offsetof(TransformBatchRequest, stride));
			const uint32 flags = memory_readU32(reqVa + offsetof(TransformBatchRequest, flags));
			if (structSize < sizeof(TransformBatchRequest) || flags != 0)
				return Status_InvalidArgument;
			if (count == 0 || count > kMaxTransformElements)
				return Status_InvalidArgument;
			if (stride < 16)
				return Status_InvalidArgument;

			// Bounds: request holds the header, then count elements at `stride`.
			const uint64 elemsBase = (uint64)reqVa + offsetof(TransformBatchRequest, matrix) + 16 * sizeof(float);
			const uint64 needed = (uint64)(count - 1) * stride + 16; // last element extent
			if (elemsBase + needed > (uint64)reqVa + reqSize)
				return Status_InvalidArgument;

			// Output: count elements of 4 floats, tightly packed.
			const uint64 outNeeded = (uint64)count * 16;
			if (respCap < outNeeded || !GuestRangeValid(respVa, (uint32)outNeeded))
				return Status_InvalidArgument;

			// Load the row-major 4x4 matrix into a Host snapshot.
			float m[16];
			const uint32 matVa = reqVa + offsetof(TransformBatchRequest, matrix);
			for (int i = 0; i < 16; i++)
				m[i] = ReadBEFloat(matVa + i * sizeof(float));

			const uint32 inBase = (uint32)elemsBase;
			for (uint32 e = 0; e < count; e++)
			{
				const uint32 inVa = inBase + e * stride;
				float v[4];
				for (int c = 0; c < 4; c++)
					v[c] = ReadBEFloat(inVa + c * sizeof(float));
				// row-major: out[r] = sum_c m[r*4+c] * v[c]
				float o[4];
				for (int r = 0; r < 4; r++)
					o[r] = m[r * 4 + 0] * v[0] + m[r * 4 + 1] * v[1] +
						   m[r * 4 + 2] * v[2] + m[r * 4 + 3] * v[3];
				const uint32 outVa = respVa + e * 16;
				for (int c = 0; c < 4; c++)
					WriteBEFloat(outVa + c * sizeof(float), o[c]);
			}
			return Status_Ok;
		}

		uint32 SvcBeginScope(const ContextRecord& ctx, uint32 reqVa, uint32 reqSize,
							 uint32 respVa, uint32 respCap)
		{
			if ((ctx.capabilities & Cap_RenderScope) == 0)
				return Status_Unsupported;
			if (reqSize < sizeof(ScopeDescV1) || !GuestRangeValid(reqVa, sizeof(ScopeDescV1)))
				return Status_InvalidArgument;
			if (respCap < sizeof(BeginScopeResponse) || !GuestRangeValid(respVa, sizeof(BeginScopeResponse)))
				return Status_InvalidArgument;

			RenderScopeDesc desc;
			desc.titleEpoch = memory_readU32(reqVa + offsetof(ScopeDescV1, titleEpoch));
			desc.moduleGeneration = memory_readU32(reqVa + offsetof(ScopeDescV1, moduleGeneration));
			const uint32 frameLo = memory_readU32(reqVa + offsetof(ScopeDescV1, frameIdLo));
			const uint32 frameHi = memory_readU32(reqVa + offsetof(ScopeDescV1, frameIdHi));
			desc.frameId = ((uint64)frameHi << 32) | frameLo;
			desc.viewIndex = memory_readU32(reqVa + offsetof(ScopeDescV1, viewIndex));
			desc.phase = memory_readU32(reqVa + offsetof(ScopeDescV1, phase));
			desc.poseSnapshotId = memory_readU32(reqVa + offsetof(ScopeDescV1, poseSnapshotId));

			uint32 scopeId = 0;
			ScopeEmitResult r = RenderScopeProducer::Instance().Begin(desc, scopeId);
			switch (r)
			{
			case ScopeEmitResult::Emitted:
				memory_writeU32(respVa + offsetof(BeginScopeResponse, structSize), sizeof(BeginScopeResponse));
				memory_writeU32(respVa + offsetof(BeginScopeResponse, scopeId), scopeId);
				return Status_Ok;
			case ScopeEmitResult::NoCommandBuffer:
				return Status_Unavailable;
			case ScopeEmitResult::DisplayList:
				return Status_Unavailable;
			case ScopeEmitResult::QueueFull:
				return Status_Busy;
			case ScopeEmitResult::InvalidArgument:
				return Status_InvalidArgument;
			}
			return Status_Unavailable;
		}

		uint32 SvcEndScope(const ContextRecord& ctx, uint32 reqVa, uint32 reqSize)
		{
			if ((ctx.capabilities & Cap_RenderScope) == 0)
				return Status_Unsupported;
			if (reqSize < sizeof(EndScopeRequest) || !GuestRangeValid(reqVa, sizeof(EndScopeRequest)))
				return Status_InvalidArgument;
			const uint32 scopeId = memory_readU32(reqVa + offsetof(EndScopeRequest, scopeId));
			const uint32 frameLo = memory_readU32(reqVa + offsetof(EndScopeRequest, frameIdLo));
			const uint32 frameHi = memory_readU32(reqVa + offsetof(EndScopeRequest, frameIdHi));
			const uint32 viewIndex = memory_readU32(reqVa + offsetof(EndScopeRequest, viewIndex));
			const uint64 frameId = ((uint64)frameHi << 32) | frameLo;
			ScopeEmitResult r = RenderScopeProducer::Instance().End(scopeId, frameId, viewIndex);
			switch (r)
			{
			case ScopeEmitResult::Emitted: return Status_Ok;
			case ScopeEmitResult::NoCommandBuffer:
			case ScopeEmitResult::DisplayList: return Status_Unavailable;
			case ScopeEmitResult::QueueFull: return Status_Busy;
			case ScopeEmitResult::InvalidArgument: return Status_InvalidArgument;
			}
			return Status_Unavailable;
		}

		// The real dispatcher gateway. Registered via RPLLoader_MakePPCCallable.
		void DispatcherExport(PPCInterpreter_t* hCPU)
		{
			const uint32 exportId = hCPU->gpr[3];
			const uint32 handle = hCPU->gpr[4];
			const uint32 reqVa = hCPU->gpr[5];
			const uint32 reqSize = hCPU->gpr[6];
			const uint32 respVa = hCPU->gpr[7];
			const uint32 respCap = hCPU->gpr[8];

			s_calls++;

			auto it = s_contexts.find(handle);
			if (it == s_contexts.end() || !it->second.valid)
			{
				s_rejects++;
				osLib_returnFromFunction(hCPU, Status_Stale);
				return;
			}
			const ContextRecord& ctx = it->second;

			uint32 status;
			switch (exportId)
			{
			case Export_RuntimeQueryCaps:
				status = SvcQueryCaps(ctx, respVa, respCap);
				break;
			case Export_MathTransformBatch:
				status = SvcTransformBatch(ctx, reqVa, reqSize, respVa, respCap);
				break;
			case Export_RenderBeginScope:
				status = SvcBeginScope(ctx, reqVa, reqSize, respVa, respCap);
				break;
			case Export_RenderEndScope:
				status = SvcEndScope(ctx, reqVa, reqSize);
				break;
			case Export_XrReadInput:
				// No XR provider wired in v1; report Unavailable (spec 8.2).
				status = Status_Unavailable;
				break;
			default:
				status = Status_Unsupported;
				break;
			}
			if (status != Status_Ok)
				s_rejects++;
			osLib_returnFromFunction(hCPU, status);
		}
	}

	Sdk& Sdk::Instance()
	{
		static Sdk s;
		return s;
	}

	uint32 Sdk::GatewayVa()
	{
		if (s_gatewayVa == 0)
			s_gatewayVa = RPLLoader_MakePPCCallable(DispatcherExport);
		return s_gatewayVa;
	}

	uint32 Sdk::RegisterContext(uint32 titleEpoch, uint32 moduleGeneration,
								uint32 capabilities)
	{
		const uint32 handle = s_nextHandle++;
		if (s_nextHandle == 0)
			s_nextHandle = 1;
		ContextRecord rec;
		rec.titleEpoch = titleEpoch;
		rec.moduleGeneration = moduleGeneration;
		rec.capabilities = capabilities;
		rec.valid = true;
		s_contexts[handle] = rec;
		return handle;
	}

	void Sdk::UnregisterContext(uint32 handle)
	{
		auto it = s_contexts.find(handle);
		if (it != s_contexts.end())
			it->second.valid = false;
	}

	std::string Sdk::Status() const
	{
		return fmt::format("sdk: gateway=0x{:08x} contexts={} calls={} rejects={}\n",
						   s_gatewayVa, s_contexts.size(), s_calls, s_rejects);
	}

	void Sdk::Reset()
	{
		s_contexts.clear();
		s_nextHandle = 1;
		s_calls = 0;
		s_rejects = 0;
		// s_gatewayVa is stable across titles (HLE registration is persistent).
	}
}
