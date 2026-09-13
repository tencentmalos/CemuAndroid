#pragma once

// GuestPatchRelocator -- PPC32 relocation math for guest-function modules.
//
// This is the C++ mirror of tools/guest-functions/gfbuild/reloc.py. The two
// implementations must agree byte-for-byte; both are exercised against the same
// golden cases (spec 5.2). Only the v1 whitelist is implemented; any other type
// is a hard error, never a silent zero fixup.

#include <cstdint>
#include <string>

#include "Cafe/GuestPatch/GuestPatchModule.h"

namespace GuestPatch
{
	// REL24 signed displacement range: 26-bit signed, 4-byte aligned.
	constexpr sint32 kRel24Min = -0x02000000;
	constexpr sint32 kRel24Max = 0x01FFFFFC;

	struct RelocResult
	{
		bool ok = false;
		std::string error;
	};

	// Apply one relocation into a mutable section image buffer.
	//   sectionBuf / sectionSize : the section's bytes (module image slice)
	//   offset                   : byte offset within the section
	//   symbolVa (S), addend (A), siteVa (P) per spec 5.2
	// Returns ok=false with a message on any range/alignment/opcode violation.
	RelocResult ApplyRelocation(uint8* sectionBuf, uint32 sectionSize,
								 RelocType type, uint32 offset,
								 uint32 symbolVa, sint32 addend, uint32 siteVa);

	bool IsWhitelisted(uint32 rPpcType);
	const char* RelocTypeName(RelocType type);
}
