#pragma once

// GuestPatchHost -- integration glue between the Graphic Pack layer and the
// GuestPatch loader, plus the debug command surface.
//
// On module load, ScanAndApply() walks active graphic packs for a
// guest_functions.json companion whose identity matches the loaded module, then
// drives Loader::Prepare + Commit. On unload it retires everything.
//
// Debug commands (spec 12): guest_patch_status, guest_patch_dump. Registered
// into the shared DebugCommandRegistry so Android and desktop share semantics.

#include <string>
#include <vector>

struct RPLModule;

namespace spatial::debugbus { class DebugCommandRegistry; }

namespace GuestPatch
{
	class Host
	{
	public:
		// Called from GraphicPack2::NotifyModuleLoaded for each loaded module.
		// Applies any active pack's guest_functions.json that matches.
		static void OnModuleLoaded(const RPLModule* rpl);

		// Apply one specific pack directory's guest_functions.json to one loaded
		// module. Used during pack activation, when the pack is not yet in the
		// active set. Identity mismatch is a silent skip.
		static bool ApplyPackToModule(const std::string& packDirUtf8, const RPLModule* rpl);

		// Called when a graphic pack is activated (after its patches are
		// enabled). A pack often activates AFTER modules are already loaded, so
		// this scans already-loaded modules for a companion match. packDirUtf8
		// is the pack directory (rules.txt parent).
		static void OnGraphicPackActivated(const std::string& packDirUtf8);

		// Called when the title/modules unload.
		static void OnModulesUnloaded();

		static void RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry);

	private:
		static std::string StatusCommand(const std::vector<std::string>& args);
		static std::string DumpCommand(const std::vector<std::string>& args);
	};
}
