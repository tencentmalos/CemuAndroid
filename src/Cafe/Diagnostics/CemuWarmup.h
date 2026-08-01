#pragma once

namespace spatial::debugbus
{
	class DebugCommandRegistry;
}

namespace CemuWarmup
{
	void RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry);
	void Shutdown();
}
