#include "Cafe/Diagnostics/CemuDiagnostics.h"
#include "Cafe/Diagnostics/CemuWarmup.h"
#include "Cafe/Diagnostics/GuestExecutableDump.h"
#include "Cafe/Diagnostics/GuestDebugger.h"
#include "Cafe/Diagnostics/GuestProfiler.h"

#include "Cafe/CafeSystem.h"

#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>

#include "spatial/core/FoundationApiVersion.h"
#include "spatial/core/imodules/IProfilerModule.hpp"
#include "spatial/debugbus/DebugCommandRegistry.h"
#include "spatial/debugbus/profiler/ProfilerCommands.h"
#include "spatial/profiler/Profiler.h"
#include "spatial/profiler/ProfilerModule.hpp"

#if BOOST_PLAT_ANDROID
#include "spatial/debugbus/DumpsysBridge.h"
#else
#include "spatial/debugbus/TcpServer.h"
#include "spatial/network/NetSystemModule.h"
#endif

#define CEMU_STRINGIFY_IMPL(value) #value
#define CEMU_STRINGIFY(value) CEMU_STRINGIFY_IMPL(value)

namespace
{
	constexpr uint16_t kDefaultDebugBusPort = 45987;
	uint16_t s_debugBusPort = 0;
	std::mutex s_lifecycleMutex;
	std::mutex s_hostCommandMutex;
	CemuDiagnostics::OpenLastGameHandler s_openLastGameHandler;

	static_assert(SPATIAL_FOUNDATION_API_VERSION >= 1, "Unsupported spatial foundation API version");

	std::unique_ptr<spatial::modules::ProfilerModule> s_profilerModule;

#if !BOOST_PLAT_ANDROID
	std::unique_ptr<spatial::modules::NetSystemModule> s_networkModule;
	std::unique_ptr<spatial::debugbus::TcpServer> s_debugBusServer;
#endif

	spatial::debugbus::DebugCommandRegistry& GetDebugCommandRegistry()
	{
		static spatial::debugbus::DebugCommandRegistry registry;
		static const bool initialized = [] {
			spatial::debugbus::profiler::RegisterProfilerCommands(registry);
			CemuWarmup::RegisterDebugCommands(registry);
			GuestExecutableDump::RegisterDebugCommands(registry);
			GuestDebugger::RegisterDebugCommands(registry);
			GuestProfiler::RegisterDebugCommands(registry);
			registry.Register("open_last_game", "Open the most recently launched game", [](const std::vector<std::string>& args) {
				if (!args.empty())
					return std::string{"usage: open_last_game\n"};
				CemuDiagnostics::OpenLastGameHandler handler;
				{
					std::scoped_lock lock{s_hostCommandMutex};
					handler = s_openLastGameHandler;
				}
				if (!handler)
					return std::string{"open_last_game unavailable: host has no recent-game launcher\n"};
				return handler();
			});
			registry.Register("status", "Dump Cemu debug status", [](const std::vector<std::string>&) {
				std::ostringstream out;
				out << "cemu_debug_status:\n";
				out << "native_debugbus=true\n";
				out << "emulator_hash=" << CEMU_STRINGIFY(EMULATOR_HASH) << "\n";
				out << "title_running=" << (CafeSystem::IsTitleRunning() ? "true" : "false") << "\n";
				out << "title_paused=" << (CafeSystem::IsTitlePaused() ? "true" : "false") << "\n";
				out << "profiler_backend=" << spatial::profiler::ProfilerGetBackendMode() << "\n";
				out << "profiler_connected=" << (spatial::profiler::IsProfilerConnected() ? "true" : "false") << "\n";
				out << "debugbus_port=" << s_debugBusPort << "\n";
				return out.str();
			});
			registry.Register("pause", "Pause emulation", [](const std::vector<std::string>&) {
				if (!CafeSystem::IsTitleRunning())
					return std::string{"pause unavailable: no title running\n"};
				if (CafeSystem::IsTitlePaused())
					return std::string{"pause unchanged: title already paused\n"};
				CafeSystem::PauseTitle();
				return CafeSystem::IsTitlePaused() ? std::string{"pause succeeded\n"}
											  : std::string{"pause failed: title remains active\n"};
			});
			registry.Register("resume", "Resume emulation", [](const std::vector<std::string>&) {
				if (!CafeSystem::IsTitleRunning())
					return std::string{"resume unavailable: no title running\n"};
				if (!CafeSystem::IsTitlePaused())
					return std::string{"resume unchanged: title already active\n"};
				CafeSystem::ResumeTitle();
				return CafeSystem::IsTitlePaused() ? std::string{"resume failed: title remains paused\n"}
											  : std::string{"resume succeeded\n"};
			});
			return true;
		}();
		(void)initialized;
		return registry;
	}

	uint16_t GetConfiguredDebugBusPort()
	{
		const char* value = std::getenv("CEMU_DEBUGBUS_PORT");
		if (value == nullptr)
			return kDefaultDebugBusPort;

		try
		{
			const unsigned long port = std::stoul(value);
			if (port > 0 && port <= std::numeric_limits<uint16_t>::max())
				return static_cast<uint16_t>(port);
		}
		catch (...)
		{
		}
		return kDefaultDebugBusPort;
	}

	void InitializeProfiler()
	{
		if (s_profilerModule)
			return;

		if (const char* backend = std::getenv("CEMU_PROFILER_BACKEND"))
			spatial::profiler::ProfilerSetBackendMode(backend);
		else if (std::getenv("AZAHAR_PROFILER_BACKEND") == nullptr)
			spatial::profiler::ProfilerSetBackendMode("tracy");

		auto profilerModule = spatial::modules::CreateProfilerModule();
		profilerModule->preCreate();
		if (profilerModule->initModule() != spatial::ModuleCallReturnStatus::Succeed)
		{
			spatial::modules::setProfiler(nullptr);
			return;
		}
		if (profilerModule->startModule() != spatial::ModuleCallReturnStatus::Succeed)
		{
			profilerModule->stopModule();
			return;
		}

		spatial::profiler::ProfilerSetCurrentThreadName("Cemu Main");
		spatial::profiler::ProfilerNotifyThisThreadName();
		s_profilerModule = std::move(profilerModule);
	}
}

void CemuDiagnostics::Initialize()
{
	std::scoped_lock lock{s_lifecycleMutex};
	InitializeProfiler();
	GuestProfiler::Initialize();
	auto& registry = GetDebugCommandRegistry();

#if BOOST_PLAT_ANDROID
	spatial::debugbus::SetDumpsysRegistry(&registry);
#else
	if (!s_networkModule)
	{
		auto networkModule = std::make_unique<spatial::modules::NetSystemModule>();
		if (networkModule->initModule() == spatial::ModuleCallReturnStatus::Succeed &&
			networkModule->startModule() == spatial::ModuleCallReturnStatus::Succeed)
		{
			s_networkModule = std::move(networkModule);
			s_debugBusServer = std::make_unique<spatial::debugbus::TcpServer>(
				GetConfiguredDebugBusPort(), registry,
				spatial::debugbus::TcpServerOptions{
					.greeting = "Cemu debugbus ready\n",
					.response_end_marker = ".\n",
				});
			if (s_debugBusServer->IsListening())
				s_debugBusPort = s_debugBusServer->Port();
			else
				s_debugBusServer.reset();
		}
	}
#endif
}

void CemuDiagnostics::Shutdown()
{
	CemuWarmup::Shutdown();
	GuestProfiler::Shutdown();
	std::scoped_lock lock{s_lifecycleMutex};
#if BOOST_PLAT_ANDROID
	spatial::debugbus::SetDumpsysRegistry(nullptr);
#else
	s_debugBusServer.reset();
	if (s_networkModule)
	{
		s_networkModule->stopModule();
		s_networkModule->releaseModule();
		s_networkModule.reset();
	}
	s_debugBusPort = 0;
#endif

	if (s_profilerModule)
	{
		s_profilerModule->stopModule();
		s_profilerModule->releaseModule();
		s_profilerModule.reset();
	}
}

void CemuDiagnostics::SetOpenLastGameHandler(OpenLastGameHandler handler)
{
	std::scoped_lock lock{s_hostCommandMutex};
	s_openLastGameHandler = std::move(handler);
}

std::string CemuDiagnostics::HandleDebugCommand(std::string_view command, const std::vector<std::string>& args)
{
#if BOOST_PLAT_ANDROID
	return spatial::debugbus::HandleDumpsysRequest(command, args);
#else
	return GetDebugCommandRegistry().Handle(command, args);
#endif
}

uint16_t CemuDiagnostics::GetDebugBusPort()
{
	return s_debugBusPort;
}
