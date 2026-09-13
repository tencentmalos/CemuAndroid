// Standalone golden cross-check for the GuestPatch PPC relocator.
//
// Compiles the relocator in isolation (no Cemu runtime) and asserts it produces
// the same bytes as the Python engine's golden cases in
// tools/guest-functions/tests/test_reloc.py, at two allocation bases. This is
// the C++ half of the spec 5.2 / section 13 shared-golden contract.
//
// Build (from repo root):
//   clang++ -std=c++20 -I src -I dependencies/fmt/include \
//     src/Cafe/GuestPatch/tests/reloc_golden_check.cpp \
//     src/Cafe/GuestPatch/GuestPatchRelocator.cpp \
//     dependencies/fmt/src/format.cc -o /tmp/reloc_golden && /tmp/reloc_golden
//
// The file defines the fixed-width type aliases the relocator expects so it can
// build without pulling in Cemu's precompiled header.

#include <cstdint>

using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
using sint8 = std::int8_t;
using sint16 = std::int16_t;
using sint32 = std::int32_t;
using sint64 = std::int64_t;

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Cafe/GuestPatch/GuestPatchRelocator.h"

using namespace GuestPatch;

static int g_failures = 0;

static void expectBytes(const char* name, const std::vector<uint8>& got,
						 std::vector<uint8> want)
{
	if (got != want)
	{
		g_failures++;
		std::printf("FAIL %s\n  got :", name);
		for (auto b : got) std::printf(" %02x", b);
		std::printf("\n  want:");
		for (auto b : want) std::printf(" %02x", b);
		std::printf("\n");
	}
	else
	{
		std::printf("ok   %s\n", name);
	}
}

static void expectFail(const char* name, RelocResult r)
{
	if (r.ok)
	{
		g_failures++;
		std::printf("FAIL %s (expected rejection, got ok)\n", name);
	}
	else
	{
		std::printf("ok   %s (rejected: %s)\n", name, r.error.c_str());
	}
}

static std::vector<uint8> be32(uint32 v)
{
	return {(uint8)(v >> 24), (uint8)(v >> 16), (uint8)(v >> 8), (uint8)v};
}

static std::vector<uint8> be16(uint32 v)
{
	return {(uint8)(v >> 8), (uint8)v};
}

int main()
{
	// ADDR32 with addend
	{
		auto buf = std::vector<uint8>(4, 0);
		auto r = ApplyRelocation(buf.data(), 4, RelocType::Addr32, 0, 0x10203040, 8, 0);
		(void)r;
		expectBytes("ADDR32+addend", buf, be32(0x10203048));
	}
	// ADDR32 negative addend wraps u32
	{
		auto buf = std::vector<uint8>(4, 0);
		ApplyRelocation(buf.data(), 4, RelocType::Addr32, 0, 0x00000004, -8, 0);
		expectBytes("ADDR32-neg", buf, be32(0xFFFFFFFC));
	}
	// ADDR16_LO / HI / HA
	{
		auto buf = std::vector<uint8>(2, 0);
		ApplyRelocation(buf.data(), 2, RelocType::Addr16Lo, 0, 0x1234ABCD, 0, 0);
		expectBytes("ADDR16_LO", buf, be16(0xABCD));
	}
	{
		auto buf = std::vector<uint8>(2, 0);
		ApplyRelocation(buf.data(), 2, RelocType::Addr16Hi, 0, 0x1234ABCD, 0, 0);
		expectBytes("ADDR16_HI", buf, be16(0x1234));
	}
	{
		auto buf = std::vector<uint8>(2, 0);
		ApplyRelocation(buf.data(), 2, RelocType::Addr16Ha, 0, 0x12347FFF, 0, 0);
		expectBytes("ADDR16_HA no carry", buf, be16(0x1234));
	}
	{
		auto buf = std::vector<uint8>(2, 0);
		ApplyRelocation(buf.data(), 2, RelocType::Addr16Ha, 0, 0x12348000, 0, 0);
		expectBytes("ADDR16_HA carry", buf, be16(0x1235));
	}
	{
		auto buf = std::vector<uint8>(2, 0);
		ApplyRelocation(buf.data(), 2, RelocType::Addr16Ha, 0, 0xFFFF8000, 0, 0);
		expectBytes("ADDR16_HA rollover", buf, be16(0x0000));
	}
	// ADDR16_LO immediate at insn+2
	{
		auto buf = be32(0x38640000); // addi r3,r4,0
		ApplyRelocation(buf.data(), 4, RelocType::Addr16Lo, 2, 0x0000BEEF, 0, 0);
		expectBytes("ADDR16_LO@+2", buf, be32(0x3864BEEF));
	}
	// REL24 forward
	{
		uint32 bl = (18u << 26) | 1;
		auto buf = be32(bl);
		ApplyRelocation(buf.data(), 4, RelocType::Rel24, 0, 0x1100, 0, 0x1000);
		uint32 expected = (18u << 26) | ((0x100u >> 2) << 2) | 1;
		expectBytes("REL24 forward", buf, be32(expected));
	}
	// REL24 out of range rejected
	{
		uint32 bl = (18u << 26) | 1;
		auto buf = be32(bl);
		expectFail("REL24 out-of-range",
				   ApplyRelocation(buf.data(), 4, RelocType::Rel24, 0, 0x08000000, 0, 0));
	}
	// REL24 non-I-form host rejected
	{
		uint32 bc = (16u << 26);
		auto buf = be32(bc);
		expectFail("REL24 non-I-form",
				   ApplyRelocation(buf.data(), 4, RelocType::Rel24, 0, 0x1100, 0, 0x1000));
	}
	// REL32
	{
		auto buf = std::vector<uint8>(4, 0);
		ApplyRelocation(buf.data(), 4, RelocType::Rel32, 0, 0x1234, 0, 0x1000);
		expectBytes("REL32 forward", buf, be32(0x234));
	}
	// Two-base golden (matches Python TwoBaseGoldenTest)
	{
		auto applyModule = [](uint32 base, uint32 symOff,
							  std::vector<uint8>& bl, std::vector<uint8>& a32,
							  std::vector<uint8>& lis, std::vector<uint8>& addi) {
			uint32 symVa = base + symOff;
			bl = be32((18u << 26) | 1);
			ApplyRelocation(bl.data(), 4, RelocType::Rel24, 0, symVa, 0, base + 0x00);
			a32 = std::vector<uint8>(4, 0);
			ApplyRelocation(a32.data(), 4, RelocType::Addr32, 0, symVa, 0, base + 0x04);
			lis = be32((15u << 26) | (3u << 21));
			ApplyRelocation(lis.data(), 4, RelocType::Addr16Ha, 2, symVa, 0, base + 0x08);
			addi = be32((14u << 26) | (3u << 21) | (3u << 16));
			ApplyRelocation(addi.data(), 4, RelocType::Addr16Lo, 2, symVa, 0, base + 0x0C);
		};
		std::vector<uint8> blA, a32A, lisA, addiA, blB, a32B, lisB, addiB;
		uint32 baseA = 0x01800000, baseB = 0x01810000, symOff = 0x40;
		applyModule(baseA, symOff, blA, a32A, lisA, addiA);
		applyModule(baseB, symOff, blB, a32B, lisB, addiB);

		expectBytes("two-base REL24 identical", blA, blB);
		auto vaA = ((uint32)a32A[0] << 24) | (a32A[1] << 16) | (a32A[2] << 8) | a32A[3];
		auto vaB = ((uint32)a32B[0] << 24) | (a32B[1] << 16) | (a32B[2] << 8) | a32B[3];
		if (vaA != baseA + symOff || vaB != baseB + symOff || (vaB - vaA) != (baseB - baseA))
		{
			g_failures++;
			std::printf("FAIL two-base ADDR32 (vaA=%08x vaB=%08x)\n", vaA, vaB);
		}
		else
		{
			std::printf("ok   two-base ADDR32 tracks base\n");
		}
		auto reconstruct = [](const std::vector<uint8>& lisW, const std::vector<uint8>& addiW) {
			uint32 ha = ((uint32)lisW[2] << 8) | lisW[3];
			uint32 lo = ((uint32)addiW[2] << 8) | addiW[3];
			sint32 loSigned = (lo & 0x8000) ? (sint32)lo - 0x10000 : (sint32)lo;
			return (uint32)(((ha << 16) + loSigned) & 0xFFFFFFFF);
		};
		if (reconstruct(lisA, addiA) != baseA + symOff || reconstruct(lisB, addiB) != baseB + symOff)
		{
			g_failures++;
			std::printf("FAIL two-base HA/LO reconstruct\n");
		}
		else
		{
			std::printf("ok   two-base HA/LO reconstruct\n");
		}
	}

	if (g_failures == 0)
		std::printf("\nALL GOLDEN CASES PASSED\n");
	else
		std::printf("\n%d GOLDEN CASES FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
