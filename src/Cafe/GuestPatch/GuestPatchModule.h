#pragma once

// GuestPatchModule -- runtime representation of a guest-function module.
//
// Mirrors the runtime manifest schema (cemu.guest-functions.v1) produced by
// tools/guest-functions/build.py. The loader parses guest_functions.json plus
// image.bin into these structures, verifies identity/imports/hooks, lays the
// module out in an owned codecave allocation, applies relocations against the
// allocated base and installs hooks.
//
// See docs/plans/2026-09-13-guest-function-patch-custom-sdk-spec.md sections 4,
// 5 and 7. The relocation math here must match
// tools/guest-functions/gfbuild/reloc.py byte-for-byte; both are validated
// against the same golden fixtures.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct RPLModule;

namespace GuestPatch
{
	enum class SectionKind : uint8
	{
		Text,
		Rodata,
		Data,
		Bss,
	};

	enum class SymbolKind : uint8
	{
		Func,
		Object,
		Notype,
	};

	enum class ImportKind : uint8
	{
		GuestFunction,
		GuestData,
		RplExport,
		SdkExport,
	};

	enum class HookKind : uint8
	{
		Callsite,
		Entry,
		Manual,
	};

	enum class OriginalPolicy : uint8
	{
		None,
		ImportOriginal,
		Trampoline,
	};

	// PPC relocation types on the v1 whitelist (spec 5.2). Values match the
	// R_PPC_* numbers so the loader and build tool agree.
	enum class RelocType : uint8
	{
		Addr32 = 1,
		Addr16Lo = 4,
		Addr16Hi = 5,
		Addr16Ha = 6,
		Rel24 = 10,
		Rel32 = 26,
	};

	struct ModuleSection
	{
		std::string id;
		SectionKind kind = SectionKind::Text;
		uint32 imageOffset = 0;
		uint32 fileSize = 0;
		uint32 memorySize = 0;
		uint32 alignment = 4;
	};

	struct ModuleSymbol
	{
		std::string name;
		std::string section;
		uint32 offset = 0;
		SymbolKind kind = SymbolKind::Notype;
	};

	struct ModuleImport
	{
		std::string symbol;
		ImportKind kind = ImportKind::GuestFunction;
		uint32 guestVa = 0;           // GuestFunction/GuestData
		std::string rplModule;        // RplExport
		std::string rplSymbol;        // RplExport
		uint32 sdkExportId = 0;       // SdkExport
		std::vector<uint8> originalBytes; // optional fingerprint
		bool hasGuestVa = false;
	};

	struct ModuleRelocation
	{
		std::string section;
		uint32 offset = 0;
		RelocType type = RelocType::Addr32;
		std::string symbol;
		sint32 addend = 0;
	};

	struct ModuleHook
	{
		HookKind kind = HookKind::Callsite;
		uint32 guestVa = 0;
		std::vector<uint8> originalBytes;
		std::string targetSymbol;
		OriginalPolicy originalPolicy = OriginalPolicy::None;
		std::string originalSymbol;
		std::vector<uint8> continuationBytes; // manual hooks
	};

	struct ModuleIdentity
	{
		std::string titleId;          // 16 hex chars
		std::string region;
		uint32 titleVersion = 0;
		uint32 updateVersion = 0;
		std::string module;           // e.g. "u-king"
		uint32 patchCrc = 0;
		std::string sourceRpxSha256;  // 64 hex chars
	};

	struct ModuleHostRequirement
	{
		bool present = false;
		uint32 minHostAbi = 0;
		std::vector<std::string> requiredExports;
		std::vector<std::string> optionalExports;
	};

	// The parsed, not-yet-installed manifest. Base-independent.
	struct ModuleManifest
	{
		std::string contentDigest;
		ModuleIdentity identity;
		std::string imagePath;
		std::string imageFileSha256;
		std::string moduleDigest;
		uint32 imageSize = 0;

		std::vector<ModuleSection> sections;
		std::vector<ModuleSymbol> symbols;
		std::vector<ModuleImport> imports;
		std::vector<ModuleRelocation> relocations;
		std::vector<ModuleHook> hooks;
		ModuleHostRequirement host;
	};

	enum class ModuleState : uint8
	{
		Prepared,
		Committed,
		Active,
		Retiring,
		Retired,
		Rejected,
	};

	// A hook's recorded install: what was written and what to restore.
	struct InstalledHook
	{
		uint32 guestVa = 0;
		std::vector<uint8> originalBytes;   // restored on retire
		std::vector<uint8> installedBytes;  // for verification/dump
	};

	// A symbol resolved to a concrete guest VA after allocation.
	struct ResolvedSymbol
	{
		std::string name;
		uint32 va = 0;
	};

	// An original trampoline reserved inside the module allocation for an entry
	// hook whose wrapper needs to call the original function (spec 5.3).
	struct TrampolinePlan
	{
		std::string originalSymbol;       // import bound to this trampoline
		uint32 hookGuestVa = 0;           // the hooked entry
		std::vector<uint8> replayBytes;   // relocated original prologue
		uint32 imageOffset = 0;           // offset within the module image
		uint32 bodySize = 0;              // replay bytes + resume branch
		uint32 resumeVa = 0;              // hookGuestVa + replayBytes.size()
	};

	// The fully-prepared, base-bound module. Owns its codecave allocation.
	struct LoadedModule
	{
		ModuleManifest manifest;
		ModuleState state = ModuleState::Prepared;
		std::string rejectReason;

		uint32 moduleBase = 0;      // codecave allocation base
		uint32 moduleSize = 0;      // total allocation size
		uint32 generation = 0;      // module generation for this title epoch

		// SDK context (spec 8.1). Reserved 4-byte data slot inside the module
		// that holds the dispatcher context handle; __cemu_custom_context
		// resolves here. Zero contextHandle means the module uses no SDK.
		uint32 contextHandleOffset = 0; // image offset of the handle slot
		uint32 contextHandle = 0;       // registered dispatcher handle
		bool usesSdk = false;

		// Per-section resolved base VA (moduleBase + imageOffset).
		std::vector<uint32> sectionVa;

		// Relocated image, ready to copy into guest memory at moduleBase.
		std::vector<uint8> relocatedImage;

		// Symbols resolved to VAs (internal + reserved).
		std::vector<ResolvedSymbol> resolvedSymbols;

		// Hooks that were written (for rollback / retire / dump).
		std::vector<InstalledHook> installedHooks;

		// Original-entry trampolines for entry hooks with a Trampoline policy.
		std::vector<TrampolinePlan> trampolines;

		uint32 SectionVaById(const std::string& id) const;
	};
}
