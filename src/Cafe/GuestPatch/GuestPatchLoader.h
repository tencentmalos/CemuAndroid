#pragma once

// GuestPatchLoader -- prepare / commit / retire of guest-function modules.
//
// Lifecycle (spec 7):
//   Prepare  : read + hash assets, validate identity/imports/hooks/caps,
//              compute layout, allocate an owned codecave arena, relocate into
//              a Host temporary buffer, build hooks + trampolines + context.
//   Conflict : compare actual write ranges against ASM packs and other modules.
//   Commit   : at a startup-safe point re-check original bytes, write module +
//              hooks, invalidate the executable ranges, publish Active.
//   Retire   : restore hooks, invalidate, drain in-flight, release allocation.
//
// This class owns the module registry. The Graphic Pack layer calls
// PrepareFromPackage() when a matching module loads, and RetireAll() on unload.

#include <memory>
#include <string>
#include <vector>

#include "Cafe/GuestPatch/GuestPatchModule.h"

struct RPLModule;

namespace GuestPatch
{
	struct PrepareInput
	{
		std::string packageRootUtf8;   // directory containing guest_functions.json
		std::string manifestJson;      // contents of guest_functions.json
		std::vector<uint8> image;      // image.bin bytes
		const RPLModule* targetModule = nullptr; // matched loaded module
		std::string sourceRpxSha256;   // computed by the caller from the RPX
	};

	struct PrepareResult
	{
		bool ok = false;
		std::string error;
		LoadedModule* module = nullptr; // owned by the loader on success
	};

	class Loader
	{
	public:
		static Loader& Instance();

		// Parse + validate + lay out + relocate into a Host buffer and allocate
		// the arena. Does not write guest memory yet.
		PrepareResult Prepare(const PrepareInput& input);

		// Write the module + hooks to guest memory, invalidate JIT, publish
		// Active. Must be called at a safe point (guest not in game code).
		bool Commit(LoadedModule* module, std::string& errorOut);

		// Restore hooks, invalidate, release. Idempotent.
		void Retire(LoadedModule* module);
		void RetireAll();

		const std::vector<std::unique_ptr<LoadedModule>>& Modules() const { return m_modules; }

		// Resolve a manifest symbol name to a guest VA against a prepared module
		// (internal symbol or loader-reserved). Returns false if unknown.
		static bool ResolveSymbolVa(const LoadedModule& m, const std::string& name,
									uint32& vaOut);

	private:
		Loader() = default;

		bool ParseManifest(const std::string& json, ModuleManifest& out, std::string& err);
		bool VerifyIdentity(const ModuleManifest& m, const RPLModule* rpl,
							const std::string& sourceRpxSha, std::string& err);
		bool LayoutAndRelocate(LoadedModule& lm, const std::vector<uint8>& image,
							   std::string& err);
		bool ResolveImports(LoadedModule& lm, std::string& err);
		bool BuildHooks(LoadedModule& lm, std::string& err);
		bool CheckConflicts(const LoadedModule& lm, std::string& err);

		void ReleaseAllocation(LoadedModule& lm);

		std::vector<std::unique_ptr<LoadedModule>> m_modules;
		uint32 m_nextGeneration = 1;
	};
}
