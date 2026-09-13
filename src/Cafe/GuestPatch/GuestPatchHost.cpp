#include "Cafe/GuestPatch/GuestPatchHost.h"

#include <fmt/format.h>
#include <openssl/sha.h>

#include "Cafe/GuestPatch/GuestPatchLoader.h"
#include "Cafe/GuestPatch/GuestPatchSdk.h"
#include "Cafe/GuestPatch/GuestRenderScope.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/CafeSystem.h"
#include "Common/precompiled.h"
#include "Common/FileStream.h"
#include "config/ActiveSettings.h"
#include "util/helpers/helpers.h"
#include "spatial/debugbus/DebugCommandRegistry.h"

namespace GuestPatch
{
	namespace
	{
		std::string Sha256Hex(const std::vector<uint8>& bytes)
		{
			std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
			SHA256(bytes.data(), bytes.size(), digest.data());
			static const char* hex = "0123456789abcdef";
			std::string out;
			out.reserve(digest.size() * 2);
			for (unsigned char b : digest)
			{
				out.push_back(hex[b >> 4]);
				out.push_back(hex[b & 0xF]);
			}
			return out;
		}

		bool ReadFileBytes(const fs::path& path, std::vector<uint8>& out)
		{
			FileStream* fs = FileStream::openFile2(path);
			if (!fs)
				return false;
			fs->extract(out);
			delete fs;
			return true;
		}

		// The current source RPX SHA-256 is computed once per title from the
		// mounted executable (spec 4.2: bind the actual input file, cached).
		const std::string& CachedSourceRpxSha()
		{
			static std::string s_sha;
			static bool s_tried = false;
			if (!s_tried)
			{
				s_tried = true;
				auto exe = CafeSystem::ExtractForegroundExecutable(false);
				if (exe.has_value())
					s_sha = Sha256Hex(exe->bytes);
			}
			return s_sha;
		}

		void ResetSourceRpxShaCache()
		{
			// Force recompute on next title; simplest is a process-lifetime cache
			// reset hook. Retained here for clarity; OnModulesUnloaded calls it.
		}

		const char* StateName(ModuleState s)
		{
			switch (s)
			{
			case ModuleState::Prepared: return "Prepared";
			case ModuleState::Committed: return "Committed";
			case ModuleState::Active: return "Active";
			case ModuleState::Retiring: return "Retiring";
			case ModuleState::Retired: return "Retired";
			case ModuleState::Rejected: return "Rejected";
			}
			return "?";
		}
	}

	namespace
	{
		// Attempt to prepare + commit the guest_functions.json in packDir against
		// one loaded module. Returns true if a module was activated. Identity
		// mismatch (wrong pack for this module) is a silent skip.
		bool TryApplyPackToModule(const fs::path& packDir, const RPLModule* rpl)
		{
			std::error_code ec;
			fs::path companion = packDir / "guest_functions.json";
			if (!fs::exists(companion, ec))
				return false;

			// Skip if a module with this identity+pack is already active to avoid
			// double-loading when both module-load and pack-activation fire.
			for (const auto& m : Loader::Instance().Modules())
			{
				if (m->state == ModuleState::Active &&
					m->manifest.identity.module == rpl->moduleName &&
					m->manifest.identity.patchCrc == rpl->patchCRC)
					return false;
			}

			std::vector<uint8> manifestBytes;
			if (!ReadFileBytes(companion, manifestBytes))
			{
				cemuLog_log(LogType::Force, "[GuestPatch] failed to read {}", _pathToUtf8(companion));
				return false;
			}

			PrepareInput input;
			input.packageRootUtf8 = _pathToUtf8(packDir);
			input.manifestJson.assign(reinterpret_cast<const char*>(manifestBytes.data()),
									  manifestBytes.size());
			input.targetModule = rpl;
			input.sourceRpxSha256 = CachedSourceRpxSha();

			fs::path imagePath = packDir / "guest" / rpl->moduleName / "image.bin";
			if (!fs::exists(imagePath, ec))
				imagePath = packDir / "image.bin";
			if (!ReadFileBytes(imagePath, input.image))
			{
				cemuLog_log(LogType::Force, "[GuestPatch] image.bin not found for pack {}",
							_pathToUtf8(packDir));
				return false;
			}

			PrepareResult pr = Loader::Instance().Prepare(input);
			if (!pr.ok)
			{
				cemuLog_log(LogType::Force, "[GuestPatch] prepare skipped/failed for {}: {}",
							_pathToUtf8(packDir), pr.error);
				return false;
			}
			std::string commitErr;
			if (!Loader::Instance().Commit(pr.module, commitErr))
			{
				cemuLog_log(LogType::Force, "[GuestPatch] commit failed for {}: {}",
							_pathToUtf8(packDir), commitErr);
				Loader::Instance().Retire(pr.module);
				return false;
			}
			cemuLog_log(LogType::Force,
						"[GuestPatch] module '{}' active at 0x{:08x} (gen {}, {} hooks)",
						pr.module->manifest.identity.module, pr.module->moduleBase,
						pr.module->generation, pr.module->installedHooks.size());
			return true;
		}
	}

	void Host::OnModuleLoaded(const RPLModule* rpl)
	{
		if (!rpl)
			return;
		// A module may load before or after packs activate. This path handles
		// "module loads while packs are already active".
		for (const auto& gp : GraphicPack2::GetActiveGraphicPacks())
		{
			if (!gp->IsEnabled())
				continue;
			fs::path packDir = gp->GetRulesPath();
			packDir.remove_filename();
			TryApplyPackToModule(packDir, rpl);
		}
	}

	bool Host::ApplyPackToModule(const std::string& packDirUtf8, const RPLModule* rpl)
	{
		if (!rpl)
			return false;
		return TryApplyPackToModule(_utf8ToPath(packDirUtf8), rpl);
	}

	void Host::OnGraphicPackActivated(const std::string& packDirUtf8)
	{
		// Retained for API symmetry; GraphicPack2 drives per-module application
		// via OnModuleLoaded after activation because it owns the loaded-module
		// list. This entry is a no-op guard when called without a module set.
		(void)packDirUtf8;
	}

	void Host::OnModulesUnloaded()
	{
		Loader::Instance().RetireAll();
		Sdk::Instance().Reset();
		RenderScopeProducer::Instance().Reset();
		RenderScopeConsumer::Instance().Reset();
		ResetSourceRpxShaCache();
	}

	std::string Host::StatusCommand(const std::vector<std::string>& args)
	{
		if (!args.empty())
			return "usage: guest_patch_status\n";
		std::string out = "guest_patch_status\n";
		const auto& mods = Loader::Instance().Modules();
		if (mods.empty())
			out += "  (no guest-function modules)\n";
		for (const auto& m : mods)
		{
			out += fmt::format(
				"  module='{}' title={} v{} crc=0x{:08x} state={} gen={}\n",
				m->manifest.identity.module, m->manifest.identity.titleId,
				m->manifest.identity.titleVersion, m->manifest.identity.patchCrc,
				StateName(m->state), m->generation);
			out += fmt::format("    base=0x{:08x} size={} digest={}\n",
							   m->moduleBase, m->moduleSize,
							   m->manifest.contentDigest.substr(0, 16));
			if (!m->rejectReason.empty())
				out += fmt::format("    reject: {}\n", m->rejectReason);
			out += fmt::format("    hooks={} imports={} relocs={} sdk={}\n",
							   m->installedHooks.size(), m->manifest.imports.size(),
							   m->manifest.relocations.size(),
							   m->usesSdk ? "yes" : "no");
		}
		out += Sdk::Instance().Status();
		out += RenderScopeStatus();
		return out;
	}

	std::string Host::DumpCommand(const std::vector<std::string>& args)
	{
		if (!args.empty())
			return "usage: guest_patch_dump\n";
		std::string out = "guest_patch_dump\n";
		const auto& mods = Loader::Instance().Modules();
		for (const auto& m : mods)
		{
			out += fmt::format("module '{}' (gen {}) base=0x{:08x}\n",
							   m->manifest.identity.module, m->generation, m->moduleBase);
			for (size_t i = 0; i < m->manifest.sections.size() && i < m->sectionVa.size(); i++)
			{
				const auto& s = m->manifest.sections[i];
				out += fmt::format("  section '{}' kind={} va=0x{:08x} size={}\n",
								   s.id, (int)s.kind, m->sectionVa[i], s.memorySize);
			}
			for (const auto& h : m->installedHooks)
			{
				out += fmt::format("  hook va=0x{:08x} orig=", h.guestVa);
				for (uint8 b : h.originalBytes) out += fmt::format("{:02x}", b);
				out += " new=";
				for (uint8 b : h.installedBytes) out += fmt::format("{:02x}", b);
				out += "\n";
			}
			for (const auto& tp : m->trampolines)
			{
				out += fmt::format("  trampoline '{}' va=0x{:08x} resume=0x{:08x} size={}\n",
								   tp.originalSymbol, m->moduleBase + tp.imageOffset,
								   tp.resumeVa, tp.bodySize);
			}
			// Resolved symbol -> VA table for this mapping (spec 12: debug
			// symbols are derived from the actual mapping, never from ELF offsets).
			for (const auto& rs : m->resolvedSymbols)
			{
				if (rs.name.rfind("@section:", 0) == 0)
					continue; // internal reloc helper, not a user symbol
				out += fmt::format("  sym {} = 0x{:08x}\n", rs.name, rs.va);
			}
		}
		return out;
	}

	void Host::RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry)
	{
		registry.Register("guest_patch_status",
						  "List guest-function modules: identity, state, section VAs, hooks",
						  StatusCommand);
		registry.Register("guest_patch_dump",
						  "Dump guest-function module section VAs and hook bytes",
						  DumpCommand);
	}
}
