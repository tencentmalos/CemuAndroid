#include "Cafe/Diagnostics/CemuWarmup.h"

#include "Cafe/CafeSystem.h"
#include "input/InputManager.h"
#include "input/emulated/ClassicController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/VPADController.h"
#include "input/emulated/WiimoteController.h"

#include "spatial/debugbus/DebugCommandRegistry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	constexpr uint64 kAButtonMapping = 1;
	constexpr uint32_t kTitleReadyTimeoutMilliseconds = 180'000;

	static_assert(VPADController::kButtonId_A == kAButtonMapping);
	static_assert(ProController::kButtonId_A == kAButtonMapping);
	static_assert(ClassicController::kButtonId_A == kAButtonMapping);
	static_assert(WiimoteController::kButtonId_A == kAButtonMapping);

	struct WarmupSettings
	{
		uint32_t count = 6;
		uint32_t delayMilliseconds = 35'000;
		uint32_t intervalMilliseconds = 10'000;
		uint32_t pressMilliseconds = 250;
		uint32_t settleMilliseconds = 60'000;
	};

	enum class WarmupState
	{
		Idle,
		WaitingForTitle,
		Delaying,
		Running,
		Settling,
		Completed,
		Cancelled,
		Failed,
	};

	std::string_view StateName(WarmupState state)
	{
		switch (state)
		{
		case WarmupState::Idle:
			return "idle";
		case WarmupState::WaitingForTitle:
			return "waiting_for_title";
		case WarmupState::Delaying:
			return "delaying";
		case WarmupState::Running:
			return "running";
		case WarmupState::Settling:
			return "settling";
		case WarmupState::Completed:
			return "completed";
		case WarmupState::Cancelled:
			return "cancelled";
		case WarmupState::Failed:
			return "failed";
		}
		return "unknown";
	}

	bool ParseUnsigned(std::string_view value, uint32_t minimum, uint32_t maximum, uint32_t& out)
	{
		try
		{
			size_t parsedCharacters = 0;
			const unsigned long parsed = std::stoul(std::string{value}, &parsedCharacters, 10);
			if (parsedCharacters != value.size() || parsed < minimum || parsed > maximum)
				return false;
			out = static_cast<uint32_t>(parsed);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	class WarmupAutomation
	{
	public:
		~WarmupAutomation()
		{
			StopWorker();
		}

		std::string Start(const std::vector<std::string>& args)
		{
			WarmupSettings settings;
			if (args.size() > 5)
				return Usage();
			if (args.size() >= 1 && !ParseUnsigned(args[0], 1, 1'000, settings.count))
				return "warmup_a: count must be in [1, 1000]\n";
			if (args.size() >= 2 && !ParseUnsigned(args[1], 0, 300'000, settings.delayMilliseconds))
				return "warmup_a: delay_ms must be in [0, 300000]\n";
			if (args.size() >= 3 && !ParseUnsigned(args[2], 1, 60'000, settings.intervalMilliseconds))
				return "warmup_a: interval_ms must be in [1, 60000]\n";
			if (args.size() >= 4 && !ParseUnsigned(args[3], 1, 5'000, settings.pressMilliseconds))
				return "warmup_a: press_ms must be in [1, 5000]\n";
			if (args.size() >= 5 && !ParseUnsigned(args[4], 0, 600'000, settings.settleMilliseconds))
				return "warmup_a: settle_ms must be in [0, 600000]\n";

			std::scoped_lock workerLock{m_workerMutex};
			StopWorkerLocked();
			{
				std::scoped_lock lock{m_stateMutex};
				m_settings = settings;
				m_completedCount = 0;
				m_lastError.clear();
				m_state = WarmupState::WaitingForTitle;
			}
			m_stopRequested.store(false);
			m_worker = std::thread([this, settings] {
				Run(settings);
			});

			std::ostringstream out;
			out << "warmup_a scheduled\n";
			out << "count=" << settings.count << "\n";
			out << "delay_ms=" << settings.delayMilliseconds << "\n";
			out << "interval_ms=" << settings.intervalMilliseconds << "\n";
			out << "press_ms=" << settings.pressMilliseconds << "\n";
			out << "settle_ms=" << settings.settleMilliseconds << "\n";
			return out.str();
		}

		std::string Status() const
		{
			WarmupSettings settings;
			WarmupState state;
			uint32_t completedCount;
			std::string lastError;
			{
				std::scoped_lock lock{m_stateMutex};
				settings = m_settings;
				state = m_state;
				completedCount = m_completedCount;
				lastError = m_lastError;
			}

			std::ostringstream out;
			out << "warmup_state=" << StateName(state) << "\n";
			out << "title_running=" << (CafeSystem::IsTitleRunning() ? "true" : "false") << "\n";
			out << "controller_ready=" << (GetController() ? "true" : "false") << "\n";
			out << "completed=" << completedCount << "\n";
			out << "count=" << settings.count << "\n";
			out << "delay_ms=" << settings.delayMilliseconds << "\n";
			out << "interval_ms=" << settings.intervalMilliseconds << "\n";
			out << "press_ms=" << settings.pressMilliseconds << "\n";
			out << "settle_ms=" << settings.settleMilliseconds << "\n";
			if (!lastError.empty())
				out << "last_error=" << lastError << "\n";
			return out.str();
		}

		std::string Cancel()
		{
			std::scoped_lock workerLock{m_workerMutex};
			StopWorkerLocked();
			SetAButton(false);
			{
				std::scoped_lock lock{m_stateMutex};
				if (m_state == WarmupState::Idle || m_state == WarmupState::Completed || m_state == WarmupState::Failed)
					return "warmup_cancel unchanged: no active warmup\n";
				m_state = WarmupState::Cancelled;
			}
			return "warmup_cancel succeeded\n";
		}

		void Shutdown()
		{
			StopWorker();
			SetAButton(false);
		}

	private:
		static std::string Usage()
		{
			return "usage: warmup_a [count] [delay_ms] [interval_ms] [press_ms] [settle_ms]\n";
		}

		static EmulatedControllerPtr GetController()
		{
			return InputManager::instance().get_controller(0);
		}

		static bool SetAButton(bool pressed)
		{
			auto controller = GetController();
			if (!controller)
				return false;
			controller->setButtonValue(kAButtonMapping, pressed);
			return true;
		}

		bool WaitInterruptibly(uint32_t milliseconds) const
		{
			uint32_t remaining = milliseconds;
			while (remaining > 0 && !m_stopRequested.load())
			{
				const uint32_t slice = std::min<uint32_t>(remaining, 25);
				std::this_thread::sleep_for(std::chrono::milliseconds{slice});
				remaining -= slice;
			}
			return !m_stopRequested.load();
		}

		void Run(WarmupSettings settings)
		{
			uint32_t waitedForTitle = 0;
			while ((!CafeSystem::IsTitleRunning() || !GetController()) && waitedForTitle < kTitleReadyTimeoutMilliseconds)
			{
				if (!WaitInterruptibly(100))
					return MarkCancelled();
				waitedForTitle += 100;
			}
			if (!CafeSystem::IsTitleRunning())
				return MarkFailed("title did not start within 180000ms");
			if (!GetController())
				return MarkFailed("controller 0 did not become ready within 180000ms");

			SetState(WarmupState::Delaying);
			if (!WaitInterruptibly(settings.delayMilliseconds))
				return MarkCancelled();

			SetState(WarmupState::Running);
			for (uint32_t index = 0; index < settings.count; ++index)
			{
				if (!CafeSystem::IsTitleRunning())
					return MarkFailed("title stopped during warmup");
				if (!SetAButton(true))
					return MarkFailed("controller 0 became unavailable");
				if (!WaitInterruptibly(settings.pressMilliseconds))
				{
					SetAButton(false);
					return MarkCancelled();
				}
				SetAButton(false);
				{
					std::scoped_lock lock{m_stateMutex};
					m_completedCount = index + 1;
				}
				if (index + 1 < settings.count)
				{
					const uint32_t restMilliseconds = settings.intervalMilliseconds > settings.pressMilliseconds
						? settings.intervalMilliseconds - settings.pressMilliseconds
						: 0;
					if (!WaitInterruptibly(restMilliseconds))
						return MarkCancelled();
				}
			}

			SetState(WarmupState::Settling);
			if (!WaitInterruptibly(settings.settleMilliseconds))
				return MarkCancelled();
			if (!CafeSystem::IsTitleRunning())
				return MarkFailed("title stopped while warmup was settling");
			SetState(WarmupState::Completed);
		}

		void StopWorker()
		{
			std::scoped_lock lock{m_workerMutex};
			StopWorkerLocked();
		}

		void StopWorkerLocked()
		{
			if (!m_worker.joinable())
				return;
			m_stopRequested.store(true);
			m_worker.join();
		}

		void SetState(WarmupState state)
		{
			std::scoped_lock lock{m_stateMutex};
			m_state = state;
		}

		void MarkCancelled()
		{
			SetAButton(false);
			SetState(WarmupState::Cancelled);
		}

		void MarkFailed(std::string error)
		{
			SetAButton(false);
			std::scoped_lock lock{m_stateMutex};
			m_lastError = std::move(error);
			m_state = WarmupState::Failed;
		}

		mutable std::mutex m_stateMutex;
		std::mutex m_workerMutex;
		std::thread m_worker;
		std::atomic_bool m_stopRequested{false};
		WarmupSettings m_settings;
		WarmupState m_state = WarmupState::Idle;
		uint32_t m_completedCount = 0;
		std::string m_lastError;
	};

	WarmupAutomation& GetWarmupAutomation()
	{
		static WarmupAutomation automation;
		return automation;
	}
}

void CemuWarmup::RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry)
{
	registry.Register("warmup_a", "Press controller 0 A slowly, then wait for gameplay to settle", [](const std::vector<std::string>& args) {
		return GetWarmupAutomation().Start(args);
	});
	registry.Register("warmup_status", "Report asynchronous warmup progress", [](const std::vector<std::string>& args) {
		if (!args.empty())
			return std::string{"usage: warmup_status\n"};
		return GetWarmupAutomation().Status();
	});
	registry.Register("warmup_cancel", "Cancel asynchronous warmup and release A", [](const std::vector<std::string>& args) {
		if (!args.empty())
			return std::string{"usage: warmup_cancel\n"};
		return GetWarmupAutomation().Cancel();
	});
}

void CemuWarmup::Shutdown()
{
	GetWarmupAutomation().Shutdown();
}
