#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace CemuDiagnostics
{
	void Initialize();
	void Shutdown();

	using OpenLastGameHandler = std::function<std::string()>;
	void SetOpenLastGameHandler(OpenLastGameHandler handler);

	std::string HandleDebugCommand(std::string_view command, const std::vector<std::string>& args);
	std::uint16_t GetDebugBusPort();
}
