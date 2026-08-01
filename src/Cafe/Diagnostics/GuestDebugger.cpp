#include "Cafe/Diagnostics/GuestDebugger.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Espresso/Debugger/GDBStub.h"

#include "spatial/debugbus/DebugCommandRegistry.h"

namespace
{
	constexpr uint16 kDefaultGuestDebuggerPort = 1337;
	std::mutex s_debuggerMutex;

	std::string BuildDebuggerStatus()
	{
		std::ostringstream out;
		out << "guest_debugger_status:\n";
		out << "protocol=gdb-remote\n";
		out << "architecture=powerpc:common\n";
		out << "address_model=guest-effective-address\n";
		out << "prepared=" << (g_gdbstub ? "true" : "false") << "\n";
		out << "listening=" << (g_gdbstub && g_gdbstub->IsInitialized() ? "true" : "false") << "\n";
		out << "connected=" << (g_gdbstub && g_gdbstub->IsConnected() ? "true" : "false") << "\n";
		out << "port=" << (g_gdbstub ? g_gdbstub->GetPort() : 0) << "\n";
		out << "title_running=" << (CafeSystem::IsTitleRunning() ? "true" : "false") << "\n";
		return out.str();
	}

	std::string HandleStart(const std::vector<std::string>& args)
	{
		if (args.size() > 1)
			return "usage: guest_debugger_start [port]\n";

		uint16 port = kDefaultGuestDebuggerPort;
		if (!args.empty())
		{
			try
			{
				const unsigned long parsedPort = std::stoul(args[0]);
				if (parsedPort == 0 || parsedPort > std::numeric_limits<uint16>::max())
					return "guest_debugger_start failed: port must be 1..65535\n";
				port = static_cast<uint16>(parsedPort);
			}
			catch (...)
			{
				return "guest_debugger_start failed: invalid port\n";
			}
		}

		std::scoped_lock lock{s_debuggerMutex};
		if (g_gdbstub)
			return "guest_debugger_start unchanged: debugger already prepared\n" + BuildDebuggerStatus();

		auto debugger = std::make_unique<GDBServer>(port);
		if (CafeSystem::IsTitleRunning() && !debugger->Initialize())
			return fmt::format("guest_debugger_start failed: cannot listen on port {}\n", port);
		g_gdbstub = std::move(debugger);

		std::ostringstream out;
		out << "guest_debugger_start succeeded\n";
		out << "state=" << (CafeSystem::IsTitleRunning() ? "listening" : "prepared-for-next-title") << "\n";
		out << "port=" << port << "\n";
		out << "adb_forward=adb forward tcp:" << port << " tcp:" << port << "\n";
		if (!CafeSystem::IsTitleRunning())
			out << "next_action=open_last_game\n";
		return out.str();
	}

	std::string HandleStop(const std::vector<std::string>& args)
	{
		if (!args.empty())
			return "usage: guest_debugger_stop\n";
		if (CafeSystem::IsTitleRunning())
			return "guest_debugger_stop unavailable: stop the title first so active Guest breakpoints can be restored safely\n";

		std::scoped_lock lock{s_debuggerMutex};
		if (!g_gdbstub)
			return "guest_debugger_stop unchanged: debugger is not prepared\n";
		g_gdbstub.reset();
		return "guest_debugger_stop succeeded\n";
	}

	std::string HandleDebuggerStatus(const std::vector<std::string>& args)
	{
		if (!args.empty())
			return "usage: guest_debugger_status\n";
		std::scoped_lock lock{s_debuggerMutex};
		return BuildDebuggerStatus();
	}
}

void GuestDebugger::RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry)
{
	registry.Register("guest_debugger_start", "Start or prepare the Guest PowerPC GDB remote stub", HandleStart);
	registry.Register("guest_debugger_stop", "Stop a prepared Guest debugger after the title exits", HandleStop);
	registry.Register("guest_debugger_status", "Show Guest debugger protocol, port, and connection state", HandleDebuggerStatus);
}
