#include "Cafe/GuestPatch/GuestPatchLoader.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <rapidjson/document.h>
#include <fmt/format.h>

#include "Cafe/GuestPatch/GuestPatchRelocator.h"
#include "Cafe/GuestPatch/GuestPatchSdk.h"
#include "../../guest/custom/v1/include/cemu/sdk_v1.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Common/precompiled.h"

// Codecave arena bounds (spec 4.3 / MMU.h).
static constexpr uint32 kCodecaveBase = MEMORY_CODECAVEAREA_ADDR;
static constexpr uint32 kCodecaveEnd = MEMORY_CODECAVEAREA_ADDR + MEMORY_CODECAVEAREA_SIZE;

// Loader-reserved data symbol receiving the module context handle (spec 8.1).
static const char* kReservedContextSymbol = "__cemu_custom_context";

namespace GuestPatch
{
	namespace
	{
		bool HexToBytes(const std::string& hex, std::vector<uint8>& out)
		{
			if ((hex.size() & 1) != 0)
				return false;
			out.clear();
			out.reserve(hex.size() / 2);
			auto nib = [](char c, int& v) -> bool {
				if (c >= '0' && c <= '9') { v = c - '0'; return true; }
				if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
				if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
				return false;
			};
			for (size_t i = 0; i < hex.size(); i += 2)
			{
				int hi, lo;
				if (!nib(hex[i], hi) || !nib(hex[i + 1], lo))
					return false;
				out.push_back((uint8)((hi << 4) | lo));
			}
			return true;
		}

		bool GetStr(const rapidjson::Value& v, const char* key, std::string& out)
		{
			if (!v.IsObject() || !v.HasMember(key) || !v[key].IsString())
				return false;
			out = v[key].GetString();
			return true;
		}

		bool GetUint(const rapidjson::Value& v, const char* key, uint32& out)
		{
			if (!v.IsObject() || !v.HasMember(key))
				return false;
			const auto& m = v[key];
			if (m.IsUint()) { out = m.GetUint(); return true; }
			if (m.IsUint64()) { out = (uint32)m.GetUint64(); return true; }
			if (m.IsInt() && m.GetInt() >= 0) { out = (uint32)m.GetInt(); return true; }
			return false;
		}

		bool ParseSectionKind(const std::string& s, SectionKind& out)
		{
			if (s == "text") { out = SectionKind::Text; return true; }
			if (s == "rodata") { out = SectionKind::Rodata; return true; }
			if (s == "data") { out = SectionKind::Data; return true; }
			if (s == "bss") { out = SectionKind::Bss; return true; }
			return false;
		}

		bool ParseRelocType(const std::string& s, RelocType& out)
		{
			if (s == "R_PPC_ADDR32") { out = RelocType::Addr32; return true; }
			if (s == "R_PPC_ADDR16_LO") { out = RelocType::Addr16Lo; return true; }
			if (s == "R_PPC_ADDR16_HI") { out = RelocType::Addr16Hi; return true; }
			if (s == "R_PPC_ADDR16_HA") { out = RelocType::Addr16Ha; return true; }
			if (s == "R_PPC_REL24") { out = RelocType::Rel24; return true; }
			if (s == "R_PPC_REL32") { out = RelocType::Rel32; return true; }
			return false;
		}

		bool ParseHookKind(const std::string& s, HookKind& out)
		{
			if (s == "callsite") { out = HookKind::Callsite; return true; }
			if (s == "entry") { out = HookKind::Entry; return true; }
			if (s == "manual") { out = HookKind::Manual; return true; }
			return false;
		}

		bool ParseOriginalPolicy(const std::string& s, OriginalPolicy& out)
		{
			if (s == "none") { out = OriginalPolicy::None; return true; }
			if (s == "import_original") { out = OriginalPolicy::ImportOriginal; return true; }
			if (s == "trampoline") { out = OriginalPolicy::Trampoline; return true; }
			return false;
		}

		bool ParseImportKind(const std::string& s, ImportKind& out)
		{
			if (s == "guest_function") { out = ImportKind::GuestFunction; return true; }
			if (s == "guest_data") { out = ImportKind::GuestData; return true; }
			if (s == "rpl_export") { out = ImportKind::RplExport; return true; }
			if (s == "sdk_export") { out = ImportKind::SdkExport; return true; }
			return false;
		}

		// A single ba/bla into the low 32 MiB codecave; matches hooks.py.
		uint32 EncodeAbsBranch(uint32 targetVa, bool link)
		{
			const uint32 li = (targetVa >> 2) & 0x00FFFFFF;
			return (18u << 26) | (li << 2) | (1u << 1) | (link ? 1u : 0u);
		}

		// Case-insensitive equality for module names and hex digests.
		bool IEquals(const std::string& a, const std::string& b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
			{
				if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
					return false;
			}
			return true;
		}

		bool EncodeBranch(uint32 siteVa, uint32 targetVa, bool link, uint32& out,
						  std::string& err)
		{
			if ((siteVa & 3) || (targetVa & 3))
			{
				err = "branch site/target not 4-byte aligned";
				return false;
			}
			const sint64 disp = (sint64)targetVa - (sint64)siteVa;
			if (disp >= kRel24Min && disp <= kRel24Max && (disp & 3) == 0)
			{
				const uint32 liField = ((uint32)(sint32)disp >> 2) & 0x00FFFFFF;
				out = (18u << 26) | (liField << 2) | (0u << 1) | (link ? 1u : 0u);
				return true;
			}
			if (targetVa <= 0x01FFFFFC && targetVa >= 0x100)
			{
				out = EncodeAbsBranch(targetVa, link);
				return true;
			}
			err = fmt::format("target 0x{:08x} unreachable from 0x{:08x}", targetVa, siteVa);
			return false;
		}

		uint32 ReadBE32(const std::vector<uint8>& b, size_t off)
		{
			return ((uint32)b[off] << 24) | ((uint32)b[off + 1] << 16) |
				   ((uint32)b[off + 2] << 8) | (uint32)b[off + 3];
		}

		// Entry-hook prologue whitelist for auto-replay (spec 5.3). Only these
		// position-independent first instructions may be relocated into the
		// original trampoline; anything else requires an explicit .S bridge.
		bool IsReplayablePrologueInsn(uint32 insn)
		{
			// mflr r0 : 0x7C0802A6
			if (insn == 0x7C0802A6)
				return true;
			const uint32 primary = (insn >> 26) & 0x3F;
			// stwu r1,-N(r1): primary 37, rS=1, rA=1
			if (primary == 37 && ((insn >> 21) & 0x1F) == 1 && ((insn >> 16) & 0x1F) == 1)
				return true;
			// stw rS,D(r1): primary 36, rA=1 (audited stack save)
			if (primary == 36 && ((insn >> 16) & 0x1F) == 1)
				return true;
			return false;
		}

		// Validate that an entry hook's overwritten prologue can be auto-replayed.
		bool ValidateEntryPrologue(const std::vector<uint8>& original, std::string& err)
		{
			if (original.size() < 4 || (original.size() % 4) != 0)
			{
				err = "entry hook original must be a non-empty multiple of 4 bytes";
				return false;
			}
			for (size_t i = 0; i < original.size(); i += 4)
			{
				const uint32 insn = ReadBE32(original, i);
				const uint32 primary = (insn >> 26) & 0x3F;
				if (primary == 18 || primary == 16)
				{
					err = "entry prologue contains a branch; requires a manual bridge";
					return false;
				}
				if (!IsReplayablePrologueInsn(insn))
				{
					err = fmt::format("entry prologue insn 0x{:08x} not in auto-replay "
									  "whitelist; requires an explicit .S bridge", insn);
					return false;
				}
			}
			return true;
		}
	}

	Loader& Loader::Instance()
	{
		static Loader s_instance;
		return s_instance;
	}

	bool Loader::ParseManifest(const std::string& json, ModuleManifest& out,
							   std::string& err)
	{
		rapidjson::Document d;
		d.Parse(json.data(), json.size());
		if (d.HasParseError() || !d.IsObject())
		{
			err = "manifest is not valid JSON";
			return false;
		}
		std::string schema;
		if (!GetStr(d, "schema", schema) || schema != "cemu.guest-functions.v1")
		{
			err = "manifest schema mismatch";
			return false;
		}
		GetStr(d, "content_digest", out.contentDigest);

		// identity
		if (!d.HasMember("identity") || !d["identity"].IsObject())
		{
			err = "manifest missing identity";
			return false;
		}
		const auto& id = d["identity"];
		if (!GetStr(id, "title_id", out.identity.titleId) ||
			!GetUint(id, "title_version", out.identity.titleVersion) ||
			!GetStr(id, "module", out.identity.module) ||
			!GetUint(id, "patch_crc", out.identity.patchCrc) ||
			!GetStr(id, "source_rpx_sha256", out.identity.sourceRpxSha256))
		{
			err = "manifest identity is incomplete";
			return false;
		}
		GetStr(id, "region", out.identity.region);
		GetUint(id, "update_version", out.identity.updateVersion);

		// image
		if (!d.HasMember("image") || !d["image"].IsObject())
		{
			err = "manifest missing image";
			return false;
		}
		const auto& img = d["image"];
		if (!GetStr(img, "path", out.imagePath) ||
			!GetStr(img, "file_sha256", out.imageFileSha256) ||
			!GetStr(img, "module_digest", out.moduleDigest) ||
			!GetUint(img, "size", out.imageSize))
		{
			err = "manifest image block is incomplete";
			return false;
		}
		if (out.imagePath.find("..") != std::string::npos || out.imagePath.empty() ||
			out.imagePath.front() == '/')
		{
			err = "manifest image path escapes package root";
			return false;
		}

		// sections
		if (!d.HasMember("sections") || !d["sections"].IsArray() || d["sections"].Empty())
		{
			err = "manifest has no sections";
			return false;
		}
		for (const auto& sv : d["sections"].GetArray())
		{
			ModuleSection sec;
			std::string kindStr;
			if (!GetStr(sv, "id", sec.id) || !GetStr(sv, "kind", kindStr) ||
				!ParseSectionKind(kindStr, sec.kind) ||
				!GetUint(sv, "image_offset", sec.imageOffset) ||
				!GetUint(sv, "file_size", sec.fileSize) ||
				!GetUint(sv, "memory_size", sec.memorySize) ||
				!GetUint(sv, "alignment", sec.alignment))
			{
				err = "manifest section entry is invalid";
				return false;
			}
			if (sec.alignment != 4 && sec.alignment != 8 && sec.alignment != 16 &&
				sec.alignment != 256)
			{
				err = fmt::format("section '{}' has unsupported alignment {}", sec.id, sec.alignment);
				return false;
			}
			out.sections.push_back(std::move(sec));
		}

		// symbols
		if (d.HasMember("symbols") && d["symbols"].IsArray())
		{
			for (const auto& sv : d["symbols"].GetArray())
			{
				ModuleSymbol sym;
				std::string kindStr;
				if (!GetStr(sv, "name", sym.name) || !GetStr(sv, "section", sym.section) ||
					!GetUint(sv, "offset", sym.offset) || !GetStr(sv, "kind", kindStr))
				{
					err = "manifest symbol entry is invalid";
					return false;
				}
				sym.kind = kindStr == "func" ? SymbolKind::Func :
					kindStr == "object" ? SymbolKind::Object : SymbolKind::Notype;
				out.symbols.push_back(std::move(sym));
			}
		}

		// imports
		if (d.HasMember("imports") && d["imports"].IsArray())
		{
			for (const auto& iv : d["imports"].GetArray())
			{
				ModuleImport imp;
				std::string kindStr;
				if (!GetStr(iv, "symbol", imp.symbol) || !GetStr(iv, "kind", kindStr) ||
					!ParseImportKind(kindStr, imp.kind))
				{
					err = "manifest import entry is invalid";
					return false;
				}
				imp.hasGuestVa = GetUint(iv, "guest_va", imp.guestVa);
				GetStr(iv, "rpl_module", imp.rplModule);
				GetStr(iv, "rpl_symbol", imp.rplSymbol);
				GetUint(iv, "sdk_export_id", imp.sdkExportId);
				std::string ob;
				if (GetStr(iv, "original_bytes", ob) && !ob.empty())
				{
					if (!HexToBytes(ob, imp.originalBytes))
					{
						err = "import original_bytes is not valid hex";
						return false;
					}
				}
				out.imports.push_back(std::move(imp));
			}
		}

		// relocations
		if (d.HasMember("relocations") && d["relocations"].IsArray())
		{
			for (const auto& rv : d["relocations"].GetArray())
			{
				ModuleRelocation rel;
				std::string typeStr;
				uint32 offset = 0;
				if (!GetStr(rv, "section", rel.section) ||
					!GetUint(rv, "offset", offset) ||
					!GetStr(rv, "type", typeStr) || !ParseRelocType(typeStr, rel.type) ||
					!GetStr(rv, "symbol", rel.symbol))
				{
					err = "manifest relocation entry is invalid";
					return false;
				}
				rel.offset = offset;
				if (!rv.HasMember("addend") || !rv["addend"].IsInt())
				{
					err = "manifest relocation addend is not a signed int";
					return false;
				}
				rel.addend = rv["addend"].GetInt();
				out.relocations.push_back(std::move(rel));
			}
		}

		// hooks
		if (d.HasMember("hooks") && d["hooks"].IsArray())
		{
			for (const auto& hv : d["hooks"].GetArray())
			{
				ModuleHook hook;
				std::string kindStr, policyStr, ob, cb, abiProfile;
				if (!GetStr(hv, "kind", kindStr) || !ParseHookKind(kindStr, hook.kind) ||
					!GetUint(hv, "guest_va", hook.guestVa) ||
					!GetStr(hv, "original_bytes", ob) ||
					!GetStr(hv, "target_symbol", hook.targetSymbol) ||
					!GetStr(hv, "original_policy", policyStr) ||
					!ParseOriginalPolicy(policyStr, hook.originalPolicy) ||
					!GetStr(hv, "abi_profile", abiProfile))
				{
					err = "manifest hook entry is invalid";
					return false;
				}
				if (!HexToBytes(ob, hook.originalBytes) || hook.originalBytes.empty())
				{
					err = "hook original_bytes is not valid hex";
					return false;
				}
				GetStr(hv, "original_symbol", hook.originalSymbol);
				if (GetStr(hv, "continuation_bytes", cb) && !cb.empty())
				{
					if (!HexToBytes(cb, hook.continuationBytes))
					{
						err = "hook continuation_bytes is not valid hex";
						return false;
					}
				}
				out.hooks.push_back(std::move(hook));
			}
		}

		// host (optional)
		if (d.HasMember("host") && d["host"].IsObject())
		{
			out.host.present = true;
			GetUint(d["host"], "min_host_abi", out.host.minHostAbi);
			if (d["host"].HasMember("required_exports") && d["host"]["required_exports"].IsArray())
				for (const auto& e : d["host"]["required_exports"].GetArray())
					if (e.IsString()) out.host.requiredExports.push_back(e.GetString());
			if (d["host"].HasMember("optional_exports") && d["host"]["optional_exports"].IsArray())
				for (const auto& e : d["host"]["optional_exports"].GetArray())
					if (e.IsString()) out.host.optionalExports.push_back(e.GetString());
		}

		return true;
	}

	bool Loader::VerifyIdentity(const ModuleManifest& m, const RPLModule* rpl,
								const std::string& sourceRpxSha, std::string& err)
	{
		if (rpl == nullptr)
		{
			err = "no target module";
			return false;
		}
		if (!IEquals(rpl->moduleName, m.identity.module))
		{
			err = fmt::format("module name mismatch: manifest '{}' vs loaded '{}'",
							  m.identity.module, rpl->moduleName);
			return false;
		}
		if (rpl->patchCRC != m.identity.patchCrc)
		{
			err = fmt::format("patch_crc mismatch: manifest 0x{:08x} vs module 0x{:08x}",
							  m.identity.patchCrc, rpl->patchCRC);
			return false;
		}
		if (!sourceRpxSha.empty() && !m.identity.sourceRpxSha256.empty() &&
			!IEquals(sourceRpxSha, m.identity.sourceRpxSha256))
		{
			err = "source RPX SHA-256 mismatch";
			return false;
		}
		return true;
	}

	bool Loader::LayoutAndRelocate(LoadedModule& lm, const std::vector<uint8>& image,
								   std::string& err)
	{
		const ModuleManifest& m = lm.manifest;
		if (image.size() != m.imageSize)
		{
			err = fmt::format("image size {} does not match manifest {}", image.size(), m.imageSize);
			return false;
		}

		// Validate section layout and compute total module size.
		uint32 requiredSize = 0;
		for (const auto& s : m.sections)
		{
			const uint64 end = (uint64)s.imageOffset + s.memorySize;
			if (end > 0xFFFFFFFFull)
			{
				err = "section layout overflow";
				return false;
			}
			requiredSize = std::max<uint32>(requiredSize, (uint32)end);
			if (s.kind != SectionKind::Bss)
			{
				const uint64 fileEnd = (uint64)s.imageOffset + s.fileSize;
				if (fileEnd > image.size())
				{
					err = fmt::format("section '{}' file data exceeds image", s.id);
					return false;
				}
			}
		}

		// Plan original trampolines for entry hooks that need to call the
		// original function (spec 5.3). Reserve their bodies at the end of the
		// module allocation so ownership stays with the module (spec 4.3).
		requiredSize = (requiredSize + 3u) & ~3u;
		for (const auto& hook : m.hooks)
		{
			if (hook.kind != HookKind::Entry ||
				hook.originalPolicy != OriginalPolicy::Trampoline)
				continue;
			if (hook.originalSymbol.empty())
			{
				err = "entry trampoline hook has no original_symbol";
				return false;
			}
			if (!ValidateEntryPrologue(hook.originalBytes, err))
				return false;
			TrampolinePlan plan;
			plan.originalSymbol = hook.originalSymbol;
			plan.hookGuestVa = hook.guestVa;
			plan.replayBytes = hook.originalBytes;
			plan.resumeVa = hook.guestVa + (uint32)hook.originalBytes.size();
			plan.bodySize = (uint32)hook.originalBytes.size() + 4; // + resume branch
			plan.imageOffset = requiredSize;
			requiredSize += plan.bodySize;
			requiredSize = (requiredSize + 3u) & ~3u;
			lm.trampolines.push_back(std::move(plan));
		}

		// Reserve a 4-byte SDK context handle slot at the end of the module for
		// modules that declare a host requirement (spec 8.1). __cemu_custom_context
		// resolves to this slot; Commit writes the registered handle here.
		lm.usesSdk = m.host.present;
		if (lm.usesSdk)
		{
			requiredSize = (requiredSize + 3u) & ~3u;
			lm.contextHandleOffset = requiredSize;
			requiredSize += 4;
			requiredSize = (requiredSize + 3u) & ~3u;
		}

		if (requiredSize == 0 || requiredSize > 256 * 1024)
		{
			err = fmt::format("module size {} out of range", requiredSize);
			return false;
		}

		// Allocate an owned codecave arena (256-byte aligned per allocator).
		MEMPTR<void> alloc = RPLLoader_AllocateCodeCaveMem(256, requiredSize);
		if (alloc.IsNull())
		{
			err = "codecave allocation failed";
			return false;
		}
		lm.moduleBase = alloc.GetMPTR();
		lm.moduleSize = requiredSize;
		if (lm.moduleBase < kCodecaveBase ||
			(uint64)lm.moduleBase + requiredSize > kCodecaveEnd)
		{
			err = "allocation outside codecave arena";
			RPLLoader_ReleaseCodeCaveMem(alloc);
			lm.moduleBase = 0;
			return false;
		}

		// Resolve per-section VAs.
		lm.sectionVa.resize(m.sections.size());
		for (size_t i = 0; i < m.sections.size(); i++)
			lm.sectionVa[i] = lm.moduleBase + m.sections[i].imageOffset;

		// Build the Host-side relocated image (BSS zero-initialized).
		lm.relocatedImage.assign(requiredSize, 0);
		for (const auto& s : m.sections)
		{
			if (s.kind == SectionKind::Bss)
				continue;
			std::memcpy(lm.relocatedImage.data() + s.imageOffset,
						image.data() + s.imageOffset, s.fileSize);
		}

		// Emit original-trampoline bodies: replayed prologue + a resume branch
		// back to (hookGuestVa + prologue size). The resume branch is a
		// non-linking b/ba so it does not clobber LR (spec 5.3).
		for (auto& tp : lm.trampolines)
		{
			const uint32 trampVa = lm.moduleBase + tp.imageOffset;
			std::memcpy(lm.relocatedImage.data() + tp.imageOffset,
						tp.replayBytes.data(), tp.replayBytes.size());
			const uint32 branchSite = trampVa + (uint32)tp.replayBytes.size();
			uint32 resumeInsn = 0;
			std::string berr;
			if (!EncodeBranch(branchSite, tp.resumeVa, /*link*/false, resumeInsn, berr))
			{
				err = fmt::format("trampoline resume branch: {}", berr);
				return false;
			}
			uint8* dst = lm.relocatedImage.data() + tp.imageOffset + tp.replayBytes.size();
			dst[0] = (uint8)((resumeInsn >> 24) & 0xFF);
			dst[1] = (uint8)((resumeInsn >> 16) & 0xFF);
			dst[2] = (uint8)((resumeInsn >> 8) & 0xFF);
			dst[3] = (uint8)(resumeInsn & 0xFF);
		}

		return true;
	}

	bool Loader::ResolveSymbolVa(const LoadedModule& m, const std::string& name,
								 uint32& vaOut)
	{
		for (const auto& rs : m.resolvedSymbols)
		{
			if (rs.name == name)
			{
				vaOut = rs.va;
				return true;
			}
		}
		return false;
	}

	bool Loader::ResolveImports(LoadedModule& lm, std::string& err)
	{
		const ModuleManifest& m = lm.manifest;

		// Internal symbols resolve to moduleBase + section VA + offset.
		for (const auto& sym : m.symbols)
		{
			const uint32 secVa = lm.SectionVaById(sym.section);
			if (secVa == 0)
			{
				err = fmt::format("symbol '{}' references unknown section '{}'", sym.name, sym.section);
				return false;
			}
			lm.resolvedSymbols.push_back({sym.name, secVa + sym.offset});
		}

		// Original trampolines: the entry hook's original_symbol import resolves
		// to the trampoline VA inside this module, not to the (now overwritten)
		// original entry. This prevents recursion (spec 5.3).
		for (const auto& tp : lm.trampolines)
			lm.resolvedSymbols.push_back({tp.originalSymbol, lm.moduleBase + tp.imageOffset});

		// Loader-reserved context symbol. Resolves to the reserved handle slot
		// when the module uses the SDK; otherwise to moduleBase as a stable
		// in-module address (spec 8.1).
		{
			const uint32 ctxVa = lm.usesSdk ? (lm.moduleBase + lm.contextHandleOffset)
											: lm.moduleBase;
			lm.resolvedSymbols.push_back({kReservedContextSymbol, ctxVa});
		}

		// Local section-relative reloc symbols (@section:<name>).
		for (const auto& sec : m.sections)
			lm.resolvedSymbols.push_back({fmt::format("@section:{}", sec.id),
										  lm.SectionVaById(sec.id)});

		// Imports.
		for (const auto& imp : m.imports)
		{
			// If this import is bound to an original trampoline, the trampoline
			// VA already resolved it; do not overwrite with the original entry.
			bool boundToTrampoline = false;
			for (const auto& tp : lm.trampolines)
			{
				if (tp.originalSymbol == imp.symbol)
				{
					boundToTrampoline = true;
					break;
				}
			}
			if (boundToTrampoline)
				continue;

			uint32 va = 0;
			switch (imp.kind)
			{
			case ImportKind::GuestFunction:
			case ImportKind::GuestData:
				if (!imp.hasGuestVa)
				{
					err = fmt::format("import '{}' has no guest_va", imp.symbol);
					return false;
				}
				va = imp.guestVa;
				break;
			case ImportKind::RplExport:
			{
				RPLModule* dep = RPLLoader_FindModuleByName(imp.rplModule);
				if (!dep)
				{
					err = fmt::format("import '{}' RPL module '{}' not loaded", imp.symbol, imp.rplModule);
					return false;
				}
				const bool isData = false;
				MPTR exp = RPLLoader_FindRPLExport(dep, imp.rplSymbol.c_str(), isData);
				if (exp == 0)
				{
					err = fmt::format("import '{}' RPL export '{}' not found", imp.symbol, imp.rplSymbol);
					return false;
				}
				va = exp;
				break;
			}
			case ImportKind::SdkExport:
				// All sdk_export imports resolve to the single real dispatcher
				// gateway (spec 8.1). The export id travels in r3 at call time;
				// the guest wrapper places it there, so every sdk import binds to
				// the same gateway VA rather than a per-export stub.
				va = Sdk::Instance().GatewayVa();
				if (va == 0)
				{
					err = fmt::format("SDK gateway unavailable for import '{}'", imp.symbol);
					return false;
				}
				break;
			}
			lm.resolvedSymbols.push_back({imp.symbol, va});
		}

		// Apply relocations against the relocated image.
		for (const auto& rel : m.relocations)
		{
			const uint32 secVa = lm.SectionVaById(rel.section);
			if (secVa == 0)
			{
				err = fmt::format("relocation targets unknown section '{}'", rel.section);
				return false;
			}
			// Find the section in the image to compute the byte pointer.
			const ModuleSection* sec = nullptr;
			uint32 secImageOffset = 0;
			for (const auto& s : m.sections)
			{
				if (s.id == rel.section) { sec = &s; secImageOffset = s.imageOffset; break; }
			}
			if (!sec)
			{
				err = "relocation section not found";
				return false;
			}
			uint32 symVa = 0;
			if (!ResolveSymbolVa(lm, rel.symbol, symVa))
			{
				err = fmt::format("relocation references unresolved symbol '{}'", rel.symbol);
				return false;
			}
			const uint32 siteVa = secVa + rel.offset;
			uint8* secBuf = lm.relocatedImage.data() + secImageOffset;
			RelocResult rr = ApplyRelocation(secBuf, sec->memorySize, rel.type,
											 rel.offset, symVa, rel.addend, siteVa);
			if (!rr.ok)
			{
				err = fmt::format("relocation {} at {}+{}: {}",
								  RelocTypeName(rel.type), rel.section, rel.offset, rr.error);
				return false;
			}
		}
		return true;
	}

	bool Loader::BuildHooks(LoadedModule& lm, std::string& err)
	{
		const ModuleManifest& m = lm.manifest;
		for (const auto& hook : m.hooks)
		{
			uint32 targetVa = 0;
			if (!ResolveSymbolVa(lm, hook.targetSymbol, targetVa))
			{
				err = fmt::format("hook target symbol '{}' not resolved", hook.targetSymbol);
				return false;
			}
			InstalledHook ih;
			ih.guestVa = hook.guestVa;
			ih.originalBytes = hook.originalBytes;

			if (hook.kind == HookKind::Manual)
			{
				err = "manual hooks require an explicit bridge (not yet in G1)";
				return false;
			}

			uint32 insn = 0;
			const bool link = (hook.kind == HookKind::Callsite);
			if (!EncodeBranch(hook.guestVa, targetVa, link, insn, err))
				return false;

			// For callsite, the original must be a linking branch (bl/bla).
			if (hook.kind == HookKind::Callsite)
			{
				if (hook.originalBytes.size() != 4)
				{
					err = "callsite hook must cover exactly 4 bytes";
					return false;
				}
				const uint32 orig = ((uint32)hook.originalBytes[0] << 24) |
									((uint32)hook.originalBytes[1] << 16) |
									((uint32)hook.originalBytes[2] << 8) |
									(uint32)hook.originalBytes[3];
				if (((orig >> 26) & 0x3F) != 18 || (orig & 1) != 1)
				{
					err = "callsite original is not a linking I-form branch";
					return false;
				}
			}

			ih.installedBytes = {
				(uint8)((insn >> 24) & 0xFF), (uint8)((insn >> 16) & 0xFF),
				(uint8)((insn >> 8) & 0xFF), (uint8)(insn & 0xFF)};
			lm.installedHooks.push_back(std::move(ih));
		}
		return true;
	}

	bool Loader::CheckConflicts(const LoadedModule& lm, std::string& err)
	{
		// Compare this module's hook write ranges against other prepared/active
		// modules' hook ranges. Overlap is rejected (spec 7.2).
		auto overlaps = [](uint32 a0, uint32 a1, uint32 b0, uint32 b1) {
			return a0 < b1 && b0 < a1;
		};
		for (const auto& other : m_modules)
		{
			if (other.get() == &lm)
				continue;
			if (other->state == ModuleState::Retired || other->state == ModuleState::Rejected)
				continue;
			for (const auto& oh : other->installedHooks)
			{
				const uint32 o0 = oh.guestVa;
				const uint32 o1 = oh.guestVa + (uint32)oh.installedBytes.size();
				for (const auto& h : lm.installedHooks)
				{
					const uint32 h0 = h.guestVa;
					const uint32 h1 = h.guestVa + (uint32)h.installedBytes.size();
					if (overlaps(h0, h1, o0, o1))
					{
						err = fmt::format("hook at 0x{:08x} conflicts with module '{}'",
										  h0, other->manifest.identity.module);
						return false;
					}
				}
			}
		}
		return true;
	}

	PrepareResult Loader::Prepare(const PrepareInput& input)
	{
		auto lm = std::make_unique<LoadedModule>();
		lm->generation = m_nextGeneration++;
		std::string err;

		if (!ParseManifest(input.manifestJson, lm->manifest, err))
			return {false, err, nullptr};

		// image.bin SHA verification against manifest.
		// (Caller passes raw image bytes; SHA computed here for the record.)
		if (input.image.size() != lm->manifest.imageSize)
			return {false, "image.bin size mismatch", nullptr};

		if (!VerifyIdentity(lm->manifest, input.targetModule, input.sourceRpxSha256, err))
			return {false, err, nullptr};

		if (!LayoutAndRelocate(*lm, input.image, err))
			return {false, err, nullptr};

		if (!ResolveImports(*lm, err))
		{
			ReleaseAllocation(*lm);
			return {false, err, nullptr};
		}

		if (!BuildHooks(*lm, err))
		{
			ReleaseAllocation(*lm);
			return {false, err, nullptr};
		}

		if (!CheckConflicts(*lm, err))
		{
			ReleaseAllocation(*lm);
			return {false, err, nullptr};
		}

		lm->state = ModuleState::Prepared;
		LoadedModule* raw = lm.get();
		m_modules.push_back(std::move(lm));
		return {true, {}, raw};
	}

	bool Loader::Commit(LoadedModule* module, std::string& errorOut)
	{
		if (!module || module->state != ModuleState::Prepared)
		{
			errorOut = "module not in Prepared state";
			return false;
		}

		// Re-check hook original bytes against live guest memory (safe point).
		for (const auto& h : module->installedHooks)
		{
			const uint8* live = memory_getPointerFromVirtualOffsetAllowNull(h.guestVa);
			if (!live)
			{
				errorOut = fmt::format("hook site 0x{:08x} not mapped", h.guestVa);
				return false;
			}
			if (std::memcmp(live, h.originalBytes.data(), h.originalBytes.size()) != 0)
			{
				errorOut = fmt::format("hook site 0x{:08x} original bytes changed", h.guestVa);
				return false;
			}
		}

		// Write the module image.
		uint8* dst = memory_getPointerFromVirtualOffsetAllowNull(module->moduleBase);
		if (!dst)
		{
			errorOut = "module base not mapped";
			return false;
		}
		std::memcpy(dst, module->relocatedImage.data(), module->relocatedImage.size());
		PPCRecompiler_invalidateRange(module->moduleBase,
									  module->moduleBase + module->moduleSize);

		// Register the SDK context and publish its handle into the reserved slot
		// so guest wrappers can pass it in r4 (spec 8.1). Capabilities the host
		// actually provides in v1: render scope + math; XR is reported by
		// query_caps only when a provider exists.
		if (module->usesSdk)
		{
			const uint32 caps = cemu_sdk_v1::Cap_RenderScope | cemu_sdk_v1::Cap_MathTransform;
			module->contextHandle = Sdk::Instance().RegisterContext(
				/*titleEpoch*/1, module->generation, caps);
			memory_writeU32(module->moduleBase + module->contextHandleOffset,
							module->contextHandle);
			PPCRecompiler_invalidateRange(module->moduleBase + module->contextHandleOffset,
										  module->moduleBase + module->contextHandleOffset + 4);
		}

		// Write hooks.
		for (const auto& h : module->installedHooks)
		{
			uint8* live = memory_getPointerFromVirtualOffset(h.guestVa);
			std::memcpy(live, h.installedBytes.data(), h.installedBytes.size());
			PPCRecompiler_invalidateRange(h.guestVa,
										  h.guestVa + (uint32)h.installedBytes.size());
		}

		module->state = ModuleState::Active;
		return true;
	}

	void Loader::Retire(LoadedModule* module)
	{
		if (!module)
			return;
		// Stop new SDK dispatch to this module's context first (spec 7.5:
		// stop producers before draining consumers).
		if (module->usesSdk && module->contextHandle != 0)
		{
			Sdk::Instance().UnregisterContext(module->contextHandle);
			module->contextHandle = 0;
		}
		if (module->state == ModuleState::Active || module->state == ModuleState::Committed)
		{
			module->state = ModuleState::Retiring;
			// Restore hooks in reverse order.
			for (auto it = module->installedHooks.rbegin();
				 it != module->installedHooks.rend(); ++it)
			{
				uint8* live = memory_getPointerFromVirtualOffsetAllowNull(it->guestVa);
				if (live)
				{
					std::memcpy(live, it->originalBytes.data(), it->originalBytes.size());
					PPCRecompiler_invalidateRange(it->guestVa,
												  it->guestVa + (uint32)it->originalBytes.size());
				}
			}
		}
		ReleaseAllocation(*module);
		module->state = ModuleState::Retired;
	}

	void Loader::RetireAll()
	{
		for (auto& m : m_modules)
			Retire(m.get());
		m_modules.clear();
	}

	void Loader::ReleaseAllocation(LoadedModule& lm)
	{
		if (lm.moduleBase != 0)
		{
			PPCRecompiler_invalidateRange(lm.moduleBase, lm.moduleBase + lm.moduleSize);
			RPLLoader_ReleaseCodeCaveMem(MEMPTR<void>{lm.moduleBase});
			lm.moduleBase = 0;
		}
	}
}
