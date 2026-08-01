#pragma once

namespace spatial::debugbus
{
	class DebugCommandRegistry;
}

namespace GuestDebugger
{
	void RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry);
}
