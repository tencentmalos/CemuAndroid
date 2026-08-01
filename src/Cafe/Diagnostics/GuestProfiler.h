#pragma once

namespace spatial::debugbus
{
	class DebugCommandRegistry;
}

namespace GuestProfiler
{
	void Initialize();
	void Shutdown();
	void RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry);
}
