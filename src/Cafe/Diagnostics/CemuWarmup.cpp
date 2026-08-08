#include "Cafe/Diagnostics/CemuWarmup.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/vpad/vpad.h"
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
		uint32_t delayMilliseconds = 15'000;
		uint32_t intervalMilliseconds = 5'000;
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

	enum class WarmupInputMode
	{
		Unresolved,
		Controller0,
		TemporaryVPAD,
		DiagnosticVPAD,
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

	std::string_view InputModeName(WarmupInputMode mode)
	{
		switch (mode)
		{
		case WarmupInputMode::Unresolved:
			return "unresolved";
		case WarmupInputMode::Controller0:
			return "controller_0";
		case WarmupInputMode::TemporaryVPAD:
			return "temporary_vpad";
		case WarmupInputMode::DiagnosticVPAD:
			return "diagnostic_vpad";
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
				m_inputMode = WarmupInputMode::Unresolved;
				m_state = WarmupState::WaitingForTitle;
			}
			vpad::ResetDiagnosticInputStats();
			PrepareInputController();
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
			out << "vpad_reads=" << vpad::GetDiagnosticReadCount() << "\n";
			out << "vpad_a_reads=" << vpad::GetDiagnosticAReadCount() << "\n";
			return out.str();
		}

		std::string Status() const
		{
			WarmupSettings settings;
			WarmupState state;
			uint32_t completedCount;
			std::string lastError;
			WarmupInputMode inputMode;
			{
				std::scoped_lock lock{m_stateMutex};
				settings = m_settings;
				state = m_state;
				completedCount = m_completedCount;
				lastError = m_lastError;
				inputMode = m_inputMode;
			}

			std::ostringstream out;
			out << "warmup_state=" << StateName(state) << "\n";
			out << "title_running=" << (CafeSystem::IsTitleRunning() ? "true" : "false") << "\n";
			out << "controller_ready=" << (GetController() ? "true" : "false") << "\n";
			out << "input_mode=" << InputModeName(inputMode) << "\n";
			out << "completed=" << completedCount << "\n";
			out << "count=" << settings.count << "\n";
			out << "delay_ms=" << settings.delayMilliseconds << "\n";
			out << "interval_ms=" << settings.intervalMilliseconds << "\n";
			out << "press_ms=" << settings.pressMilliseconds << "\n";
			out << "settle_ms=" << settings.settleMilliseconds << "\n";
			out << "vpad_reads=" << vpad::GetDiagnosticReadCount() << "\n";
			out << "vpad_a_reads=" << vpad::GetDiagnosticAReadCount() << "\n";
			if (!lastError.empty())
				out << "last_error=" << lastError << "\n";
			return out.str();
		}

		std::string Cancel()
		{
			std::scoped_lock workerLock{m_workerMutex};
			StopWorkerLocked();
			SetAButton(false);
			ReleaseTemporaryController();
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
			ReleaseTemporaryController();
		}

		void OnInputManagerReady()
		{
			std::scoped_lock workerLock{m_workerMutex};
			bool warmupActive = false;
			{
				std::scoped_lock lock{m_stateMutex};
				warmupActive = m_state == WarmupState::WaitingForTitle ||
					m_state == WarmupState::Delaying ||
					m_state == WarmupState::Running ||
					m_state == WarmupState::Settling;
			}
			if (warmupActive)
				PrepareInputController();
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

		void PrepareInputController()
		{
			auto controller = GetController();
			WarmupInputMode inputMode = WarmupInputMode::Controller0;
			if (!controller)
			{
				controller = InputManager::instance().set_controller(0, EmulatedController::Type::VPAD);
				m_temporaryController = controller;
				inputMode = controller ? WarmupInputMode::TemporaryVPAD : WarmupInputMode::DiagnosticVPAD;
			}
			std::scoped_lock lock{m_stateMutex};
			m_inputMode = inputMode;
		}

		void SetAButton(bool pressed)
		{
			// Mirror the synthetic press into VPADRead even when controller 0 exists. This
			// keeps headless warmup independent of controller-profile loading order while
			// the controller override below still covers titles using another controller
			// type through InputManager.
			vpad::SetDiagnosticButtonAOverride(pressed);
			WarmupInputMode inputMode = WarmupInputMode::DiagnosticVPAD;
			auto controller = GetController();
			if (!controller && pressed)
			{
				controller = InputManager::instance().set_controller(0, EmulatedController::Type::VPAD);
				m_temporaryController = controller;
			}
			if (controller)
			{
				controller->setButtonValue(kAButtonMapping, pressed);
				inputMode = controller == m_temporaryController ? WarmupInputMode::TemporaryVPAD : WarmupInputMode::Controller0;
			}
			std::scoped_lock lock{m_stateMutex};
			m_inputMode = inputMode;
		}

		void ReleaseTemporaryController()
		{
			if (!m_temporaryController)
				return;
			m_temporaryController->setButtonValue(kAButtonMapping, false);
			if (GetController() == m_temporaryController)
				InputManager::instance().delete_controller(0, false);
			m_temporaryController.reset();
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
			while (!CafeSystem::IsTitleRunning() && waitedForTitle < kTitleReadyTimeoutMilliseconds)
			{
				if (!WaitInterruptibly(100))
					return MarkCancelled();
				waitedForTitle += 100;
			}
			if (!CafeSystem::IsTitleRunning())
				return MarkFailed("title did not start within 180000ms");
			SetAButton(false);

			SetState(WarmupState::Delaying);
			if (!WaitInterruptibly(settings.delayMilliseconds))
				return MarkCancelled();

			SetState(WarmupState::Running);
			for (uint32_t index = 0; index < settings.count; ++index)
			{
				if (!CafeSystem::IsTitleRunning())
					return MarkFailed("title stopped during warmup");
				SetAButton(true);
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
			ReleaseTemporaryController();
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
			ReleaseTemporaryController();
			SetState(WarmupState::Cancelled);
		}

		void MarkFailed(std::string error)
		{
			SetAButton(false);
			ReleaseTemporaryController();
			std::scoped_lock lock{m_stateMutex};
			m_lastError = std::move(error);
			m_state = WarmupState::Failed;
		}

		mutable std::mutex m_stateMutex;
		std::mutex m_workerMutex;
		std::thread m_worker;
		std::atomic_bool m_stopRequested{false};
		EmulatedControllerPtr m_temporaryController;
		WarmupSettings m_settings;
		WarmupState m_state = WarmupState::Idle;
		uint32_t m_completedCount = 0;
		std::string m_lastError;
		WarmupInputMode m_inputMode = WarmupInputMode::Unresolved;
	};

	WarmupAutomation& GetWarmupAutomation()
	{
		static WarmupAutomation automation;
		return automation;
	}
}

void CemuWarmup::RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry)
{
	registry.Register("warmup_a", "Press A through controller 0 or diagnostic VPAD fallback, then wait for gameplay to settle", [](const std::vector<std::string>& args) {
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

void CemuWarmup::OnInputManagerReady()
{
	GetWarmupAutomation().OnInputManagerReady();
}

void CemuWarmup::Shutdown()
{
	GetWarmupAutomation().Shutdown();
}
