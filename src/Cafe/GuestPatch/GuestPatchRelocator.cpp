#include "Cafe/GuestPatch/GuestPatchRelocator.h"

#include <fmt/format.h>

// Byte-for-byte mirror of tools/guest-functions/gfbuild/reloc.py. Keep the two
// in lockstep; a divergence should surface as a golden-fixture failure.

namespace GuestPatch
{
	namespace
	{
		void PutBE16(uint8* buf, uint32 off, uint32 value)
		{
			buf[off + 0] = (uint8)((value >> 8) & 0xFF);
			buf[off + 1] = (uint8)(value & 0xFF);
		}

		void PutBE32(uint8* buf, uint32 off, uint32 value)
		{
			buf[off + 0] = (uint8)((value >> 24) & 0xFF);
			buf[off + 1] = (uint8)((value >> 16) & 0xFF);
			buf[off + 2] = (uint8)((value >> 8) & 0xFF);
			buf[off + 3] = (uint8)(value & 0xFF);
		}

		uint32 GetBE32(const uint8* buf, uint32 off)
		{
			return ((uint32)buf[off + 0] << 24) | ((uint32)buf[off + 1] << 16) |
				   ((uint32)buf[off + 2] << 8) | (uint32)buf[off + 3];
		}

		RelocResult Fail(const std::string& msg)
		{
			return RelocResult{false, msg};
		}

		RelocResult Ok()
		{
			return RelocResult{true, {}};
		}
	}

	bool IsWhitelisted(uint32 rPpcType)
	{
		switch (rPpcType)
		{
		case (uint32)RelocType::Addr32:
		case (uint32)RelocType::Addr16Lo:
		case (uint32)RelocType::Addr16Hi:
		case (uint32)RelocType::Addr16Ha:
		case (uint32)RelocType::Rel24:
		case (uint32)RelocType::Rel32:
			return true;
		default:
			return false;
		}
	}

	const char* RelocTypeName(RelocType type)
	{
		switch (type)
		{
		case RelocType::Addr32: return "R_PPC_ADDR32";
		case RelocType::Addr16Lo: return "R_PPC_ADDR16_LO";
		case RelocType::Addr16Hi: return "R_PPC_ADDR16_HI";
		case RelocType::Addr16Ha: return "R_PPC_ADDR16_HA";
		case RelocType::Rel24: return "R_PPC_REL24";
		case RelocType::Rel32: return "R_PPC_REL32";
		default: return "R_PPC_UNKNOWN";
		}
	}

	RelocResult ApplyRelocation(uint8* sectionBuf, uint32 sectionSize,
								RelocType type, uint32 offset,
								uint32 symbolVa, sint32 addend, uint32 siteVa)
	{
		const uint64 s = symbolVa;
		const sint64 a = addend;

		auto boundsHalf = [&]() -> bool { return offset + 2 <= sectionSize; };
		auto boundsWord = [&]() -> bool { return offset + 4 <= sectionSize; };

		switch (type)
		{
		case RelocType::Addr32:
		{
			if (!boundsWord())
				return Fail(fmt::format("ADDR32 word at {} out of section bounds", offset));
			const uint32 value = (uint32)((s + (uint64)(sint64)a) & 0xFFFFFFFF);
			PutBE32(sectionBuf, offset, value);
			return Ok();
		}
		case RelocType::Addr16Lo:
		{
			if (!boundsHalf())
				return Fail(fmt::format("ADDR16_LO halfword at {} out of section bounds", offset));
			const uint32 value = (uint32)((s + (uint64)(sint64)a) & 0xFFFF);
			PutBE16(sectionBuf, offset, value);
			return Ok();
		}
		case RelocType::Addr16Hi:
		{
			if (!boundsHalf())
				return Fail(fmt::format("ADDR16_HI halfword at {} out of section bounds", offset));
			const uint32 value = (uint32)(((s + (uint64)(sint64)a) >> 16) & 0xFFFF);
			PutBE16(sectionBuf, offset, value);
			return Ok();
		}
		case RelocType::Addr16Ha:
		{
			if (!boundsHalf())
				return Fail(fmt::format("ADDR16_HA halfword at {} out of section bounds", offset));
			const uint32 value = (uint32)(((s + (uint64)(sint64)a + 0x8000) >> 16) & 0xFFFF);
			PutBE16(sectionBuf, offset, value);
			return Ok();
		}
		case RelocType::Rel24:
		{
			if (!boundsWord())
				return Fail(fmt::format("REL24 word at {} out of section bounds", offset));
			const sint64 d64 = (sint64)s + a - (sint64)siteVa;
			const sint32 d = (sint32)d64;
			if ((d & 3) != 0)
				return Fail(fmt::format("REL24 displacement {} not 4-byte aligned", d));
			if (d < kRel24Min || d > kRel24Max)
				return Fail(fmt::format("REL24 displacement {} out of range", d));
			const uint32 insn = GetBE32(sectionBuf, offset);
			const uint32 primary = (insn >> 26) & 0x3F;
			if (primary != 18)
				return Fail(fmt::format("REL24 host insn 0x{:08x} is not I-form (primary {})", insn, primary));
			const uint32 li = ((uint32)d >> 2) & 0x00FFFFFF;
			const uint32 newInsn = (insn & 0xFC000003u) | (li << 2);
			PutBE32(sectionBuf, offset, newInsn);
			return Ok();
		}
		case RelocType::Rel32:
		{
			if (!boundsWord())
				return Fail(fmt::format("REL32 word at {} out of section bounds", offset));
			const sint64 d = (sint64)s + a - (sint64)siteVa;
			if (d < -0x80000000LL || d > 0x7FFFFFFFLL)
				return Fail(fmt::format("REL32 displacement {} not representable", d));
			PutBE32(sectionBuf, offset, (uint32)(d & 0xFFFFFFFF));
			return Ok();
		}
		default:
			return Fail(fmt::format("relocation type {} not in v1 whitelist", (int)type));
		}
	}
}
