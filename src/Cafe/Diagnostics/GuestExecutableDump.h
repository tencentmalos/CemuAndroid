#pragma once

namespace spatial::debugbus
{
	class DebugCommandRegistry;
}

namespace GuestExecutableDump
{
	void RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry);
}
