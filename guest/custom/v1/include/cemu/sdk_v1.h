#pragma once

// Cemu Custom SDK v1 -- shared wire ABI.
//
// This header is the single source of truth for the guest<->host service
// protocol (spec 8). Both the Host dispatcher (compiled into Cemu) and the
// guest SDK wrappers (compiled by tools/guest-functions with the PPC toolchain)
// include this file, so the register protocol, export ids, status codes and
// request/response layouts have exactly one definition. All wire fields are
// fixed-width big-endian; there are no host pointers, size_t, or implicit enum
// layouts on the wire.
//
// The guest side and host side are validated against the same golden byte
// fixtures (spec 8.1). Do not add host-only or guest-only fields here.

// The guest SDK is compiled freestanding for PPC32 (no hosted C++ library).
// Use the compiler-provided <stdint.h> rather than <cstdint>, which drags in
// libc++'s hosted configuration (thread API, availability markup) that does not
// exist in a freestanding build.
#include <stdint.h>

// The PPC register protocol for the dispatcher gateway (spec 8.1):
//   r3 = export_id
//   r4 = module_context_handle
//   r5 = request guest VA (0 if none)
//   r6 = request byte count
//   r7 = response guest VA (0 if none)
//   r8 = response capacity
//   return r3 = Status
//
// The dispatcher gateway VA is produced by RPLLoader_MakePPCCallable and bound
// to sdk_export imports by the module loader.

namespace cemu_sdk_v1
{
	// Host ABI version. Bumped when the wire protocol changes incompatibly.
	static const uint32_t kHostAbiVersion = 1;

	// Export ids. Stable numeric ids; names are the wire identity.
	enum ExportId : uint32_t
	{
		Export_RuntimeQueryCaps   = 1, // runtime.query_caps
		Export_XrReadInput        = 2, // xr.read_input
		Export_RenderBeginScope   = 3, // render.begin_scope
		Export_RenderEndScope     = 4, // render.end_scope
		Export_MathTransformBatch = 5, // math.transform_batch
	};

	// Standard status codes (spec 8.1). Exact values are part of the ABI.
	enum Status : uint32_t
	{
		Status_Ok              = 0,
		Status_Unsupported     = 1,
		Status_InvalidArgument = 2,
		Status_VersionMismatch = 3,
		Status_Stale           = 4,
		Status_Unavailable     = 5,
		Status_Busy            = 6,
	};

	// A big-endian uint32 on the wire. On the guest (big-endian PPC) this is a
	// plain uint32; the host byteswaps when reading/writing guest memory.
	// Layouts below use raw uint32_t and document that every multi-byte field is
	// big-endian in guest memory.

	// runtime.query_caps response.
	struct QueryCapsResponse
	{
		uint32_t structSize;      // sizeof(QueryCapsResponse)
		uint32_t hostAbiVersion;  // kHostAbiVersion
		uint32_t enabledCaps;     // bitmask of Cap_*
		uint32_t maxRequestBytes; // per-call request cap
		uint32_t titleEpoch;      // current title epoch
		uint32_t moduleGeneration;// this module's generation
	};

	enum Cap : uint32_t
	{
		Cap_XrInput        = 1u << 0,
		Cap_RenderScope    = 1u << 1,
		Cap_MathTransform  = 1u << 2,
	};

	// math.transform_batch request header, followed by the input elements.
	// Layout (spec 8.1): a bounded point/vector matrix transform. The matrix is
	// row-major 4x4 float; each element is 4 floats (x,y,z,w). Points use w=1,
	// directions use w=0 (the caller sets w).
	struct TransformBatchRequest
	{
		uint32_t structSize;   // sizeof(TransformBatchRequest)
		uint32_t elementCount; // number of 4-float elements following
		uint32_t stride;       // bytes between elements (>= 16)
		uint32_t flags;        // reserved, must be 0 in v1
		float    matrix[16];   // row-major 4x4, big-endian floats
		// followed by elementCount * (4 floats) at `stride` spacing
	};

	static const uint32_t kMaxTransformElements = 4096;

	// render.begin_scope request == ScopeDescV1 (spec 9).
	enum ViewIndex : uint32_t
	{
		View_Mono  = 0,
		View_Left  = 1,
		View_Right = 2,
	};

	enum ScopePhase : uint32_t
	{
		Phase_Shared  = 0,
		Phase_Shadow  = 1,
		Phase_Scene   = 2,
		Phase_Post    = 3,
		Phase_Ui      = 4,
		Phase_Present = 5,
	};

	struct ScopeDescV1
	{
		uint32_t structSize;      // sizeof(ScopeDescV1)
		uint32_t wireVersion;     // 1
		uint32_t titleEpoch;
		uint32_t moduleGeneration;
		uint32_t frameIdLo;       // 64-bit logical frame id
		uint32_t frameIdHi;
		uint32_t viewIndex;       // ViewIndex
		uint32_t phase;           // ScopePhase
		uint32_t poseSnapshotId;  // pose snapshot this scope is bound to
		uint32_t reserved0;
	};

	// render.begin_scope response.
	struct BeginScopeResponse
	{
		uint32_t structSize;
		uint32_t scopeId;   // opaque scope id used by end_scope
	};

	// render.end_scope request.
	struct EndScopeRequest
	{
		uint32_t structSize;
		uint32_t scopeId;
		uint32_t frameIdLo;
		uint32_t frameIdHi;
		uint32_t viewIndex;
	};
}
