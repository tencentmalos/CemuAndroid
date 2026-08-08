#include "Cafe/Diagnostics/RenderDocGuestFrameCapture.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Common/platform.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>

#if BOOST_OS_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
	enum class CaptureState : std::uint32_t
	{
		Idle,
		Requested,
		Capturing,
		Completed,
		Failed,
		Unavailable,
	};

	using RenderDocGenericFunction = void (*)();
	using RenderDocStartFrameCapture = void (*)(void*, void*);
	using RenderDocIsFrameCapturing = std::uint32_t (*)();
	using RenderDocEndFrameCapture = std::uint32_t (*)(void*, void*);

	// RENDERDOC_API_1_6_0 is append-only. Only the stable prefix through
	// EndFrameCapture is declared here so Cemu does not take a build dependency on RenderDoc.
	struct RenderDocApi
	{
		RenderDocGenericFunction getApiVersion;
		RenderDocGenericFunction setCaptureOptionU32;
		RenderDocGenericFunction setCaptureOptionF32;
		RenderDocGenericFunction getCaptureOptionU32;
		RenderDocGenericFunction getCaptureOptionF32;
		RenderDocGenericFunction setFocusToggleKeys;
		RenderDocGenericFunction setCaptureKeys;
		RenderDocGenericFunction getOverlayBits;
		RenderDocGenericFunction maskOverlayBits;
		RenderDocGenericFunction removeHooks;
		RenderDocGenericFunction unloadCrashHandler;
		RenderDocGenericFunction setCaptureFilePathTemplate;
		RenderDocGenericFunction getCaptureFilePathTemplate;
		RenderDocGenericFunction getNumCaptures;
		RenderDocGenericFunction getCapture;
		RenderDocGenericFunction triggerCapture;
		RenderDocGenericFunction isTargetControlConnected;
		RenderDocGenericFunction launchReplayUi;
		RenderDocGenericFunction setActiveWindow;
		RenderDocStartFrameCapture startFrameCapture;
		RenderDocIsFrameCapturing isFrameCapturing;
		RenderDocEndFrameCapture endFrameCapture;
	};

	using RenderDocGetApi = int (*)(int, void**);
	constexpr int kRenderDocApiVersion = 10600;

	std::atomic<CaptureState> s_state{CaptureState::Idle};
	std::atomic<std::uint32_t> s_requestId{0};
	std::atomic<std::uint32_t> s_startFrame{0};
	std::atomic<std::uint32_t> s_endFrame{0};
	std::atomic<void*> s_captureDevice{nullptr};
	std::mutex s_apiMutex;
	RenderDocApi* s_api = nullptr;
	void* s_renderDocModule = nullptr;
	bool s_apiResolutionAttempted = false;

	const char* StateName(CaptureState state)
	{
		switch (state)
		{
		case CaptureState::Idle:
			return "idle";
		case CaptureState::Requested:
			return "requested";
		case CaptureState::Capturing:
			return "capturing";
		case CaptureState::Completed:
			return "completed";
		case CaptureState::Failed:
			return "failed";
		case CaptureState::Unavailable:
			return "unavailable";
		}
		return "unknown";
	}

	RenderDocApi* ResolveApi()
	{
		std::scoped_lock lock{s_apiMutex};
		if (s_apiResolutionAttempted)
			return s_api;
		s_apiResolutionAttempted = true;

#if BOOST_OS_WINDOWS
		const HMODULE module = GetModuleHandleA("renderdoc.dll");
		if (module == nullptr)
			return nullptr;
		auto getApi = reinterpret_cast<RenderDocGetApi>(GetProcAddress(module, "RENDERDOC_GetAPI"));
#else
		auto getApi = reinterpret_cast<RenderDocGetApi>(dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI"));
#if BOOST_PLAT_ANDROID
		if (getApi == nullptr)
		{
			s_renderDocModule = dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW | RTLD_NOLOAD);
			if (s_renderDocModule == nullptr)
				s_renderDocModule = dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW | RTLD_LOCAL);
			if (s_renderDocModule != nullptr)
				getApi = reinterpret_cast<RenderDocGetApi>(dlsym(s_renderDocModule, "RENDERDOC_GetAPI"));
		}
#endif
#endif
		if (getApi == nullptr)
			return nullptr;

		void* api = nullptr;
		if (getApi(kRenderDocApiVersion, &api) != 1 || api == nullptr)
			return nullptr;
		s_api = static_cast<RenderDocApi*>(api);
		return s_api;
	}
}

std::string RenderDocGuestFrameCapture::Request()
{
	CaptureState state = s_state.load(std::memory_order_acquire);
	while (state != CaptureState::Requested && state != CaptureState::Capturing)
	{
		if (s_state.compare_exchange_weak(state, CaptureState::Requested, std::memory_order_acq_rel))
		{
			s_startFrame.store(0, std::memory_order_relaxed);
			s_endFrame.store(0, std::memory_order_relaxed);
			s_captureDevice.store(nullptr, std::memory_order_relaxed);
			const auto requestId = s_requestId.fetch_add(1, std::memory_order_relaxed) + 1;
			std::ostringstream out;
			out << "renderdoc_guest_capture scheduled\n";
			out << "request_id=" << requestId << "\n";
			return out.str();
		}
	}
	return "renderdoc_guest_capture unchanged: capture already pending\n";
}

std::string RenderDocGuestFrameCapture::GetStatus()
{
	std::ostringstream out;
	out << "renderdoc_guest_capture_state=" << StateName(s_state.load(std::memory_order_acquire)) << "\n";
	out << "request_id=" << s_requestId.load(std::memory_order_relaxed) << "\n";
	out << "start_guest_frame=" << s_startFrame.load(std::memory_order_relaxed) << "\n";
	out << "end_guest_frame=" << s_endFrame.load(std::memory_order_relaxed) << "\n";
	out << "explicit_graphics_device="
		<< (s_captureDevice.load(std::memory_order_relaxed) != nullptr ? "true" : "false") << "\n";
	return out.str();
}

void RenderDocGuestFrameCapture::OnGuestFrameBegin(std::uint32_t frameCounter)
{
	CaptureState expected = CaptureState::Requested;
	if (!s_state.compare_exchange_strong(expected, CaptureState::Capturing, std::memory_order_acq_rel))
		return;

	RenderDocApi* api = ResolveApi();
	if (api == nullptr || api->startFrameCapture == nullptr || api->isFrameCapturing == nullptr)
	{
		s_state.store(CaptureState::Unavailable, std::memory_order_release);
		return;
	}

	void* captureDevice = g_renderer ? g_renderer->GetRenderDocDevicePointer() : nullptr;
	s_captureDevice.store(captureDevice, std::memory_order_release);
	api->startFrameCapture(captureDevice, nullptr);
	if (api->isFrameCapturing() == 0)
	{
		s_captureDevice.store(nullptr, std::memory_order_release);
		s_state.store(CaptureState::Failed, std::memory_order_release);
		return;
	}
	s_startFrame.store(frameCounter, std::memory_order_relaxed);
}

void RenderDocGuestFrameCapture::OnGuestFramePresented(std::uint32_t frameCounter)
{
	if (s_state.load(std::memory_order_acquire) != CaptureState::Capturing)
		return;

	RenderDocApi* api = ResolveApi();
	if (api == nullptr || api->endFrameCapture == nullptr)
	{
		s_state.store(CaptureState::Failed, std::memory_order_release);
		return;
	}

	void* captureDevice = s_captureDevice.load(std::memory_order_acquire);
	const bool succeeded = api->endFrameCapture(captureDevice, nullptr) != 0;
	s_endFrame.store(frameCounter, std::memory_order_relaxed);
	s_state.store(succeeded ? CaptureState::Completed : CaptureState::Failed, std::memory_order_release);
}

void RenderDocGuestFrameCapture::Shutdown()
{
	if (s_state.load(std::memory_order_acquire) == CaptureState::Capturing && s_api != nullptr &&
		s_api->endFrameCapture != nullptr)
		s_api->endFrameCapture(s_captureDevice.load(std::memory_order_acquire), nullptr);
	s_captureDevice.store(nullptr, std::memory_order_release);
	s_state.store(CaptureState::Idle, std::memory_order_release);
}
