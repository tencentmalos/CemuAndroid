#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "WindowSystem.h"

#include "config/CemuConfig.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"

#include <imgui.h>
#include "imgui/imgui_extension.h"
#include "spatial/imgui/Layer.hpp"
#include "spatial/imgui/LayerManager.hpp"

#include "config/ActiveSettings.h"

std::unique_ptr<Renderer> g_renderer;

bool Renderer::GetVRAMInfo(int& usageInMB, int& totalInMB) const
{
	usageInMB = totalInMB = -1;
	
#if BOOST_OS_WINDOWS
	if (m_dxgi_wrapper)
	{
		DXGI_QUERY_VIDEO_MEMORY_INFO info{};
		if (m_dxgi_wrapper->QueryVideoMemoryInfo(info))
		{
			totalInMB = (info.Budget / 1000) / 1000;
			usageInMB = (info.CurrentUsage / 1000) / 1000;
			return true;
		}
	}
#endif

	return false;
}


void Renderer::Initialize()
{
	auto& layerManager = spatial::imgui::LayerManager::getInstance();
	layerManager.initialize();
	layerManager.setGlobalAsActiveContext();
	ImFontAtlas* sharedFontAtlas = ImGui::GetIO().Fonts;
	if (!sharedFontAtlas)
		throw std::runtime_error("foundation ImGui font atlas failed to initialize");

	m_imguiTVLayer = std::make_shared<spatial::imgui::Layer>(spatial::imgui::LayerType::Main, "CemuTVMain");
	m_imguiPadLayer = std::make_shared<spatial::imgui::Layer>(spatial::imgui::LayerType::Main, "CemuPadMain");
	m_imguiTVStatusLayer = std::make_shared<spatial::imgui::Layer>(spatial::imgui::LayerType::Statistics, "CemuTVStatistics");
	m_imguiPadStatusLayer = std::make_shared<spatial::imgui::Layer>(spatial::imgui::LayerType::Statistics, "CemuPadStatistics");
	m_imguiTVLayer->initialize(sharedFontAtlas);
	m_imguiPadLayer->initialize(sharedFontAtlas);
	m_imguiTVStatusLayer->initialize(sharedFontAtlas);
	m_imguiPadStatusLayer->initialize(sharedFontAtlas);

	auto setupContext = [](const std::shared_ptr<spatial::imgui::Layer>& layer) {
		layer->setAsActiveContext();
		ImGuiIO& io = ImGui::GetIO();
		io.WantSaveIniSettings = false;
		io.IniFilename = nullptr;
	};

	setupContext(m_imguiTVLayer);
	setupContext(m_imguiPadLayer);
	setupContext(m_imguiTVStatusLayer);
	setupContext(m_imguiPadStatusLayer);

	m_imguiTVStatusLayer->setRenderCallback([](spatial::imgui::Layer&) {
		LatteOverlay_renderStatusLayer(false);
	});
	m_imguiPadStatusLayer->setRenderCallback([](spatial::imgui::Layer&) {
		LatteOverlay_renderStatusLayer(true);
	});

	m_imguiTVLayer->setAsActiveContext();
}

void Renderer::Shutdown()
{
	if (m_imguiPadStatusLayer)
		m_imguiPadStatusLayer->shutdown();
	if (m_imguiTVStatusLayer)
		m_imguiTVStatusLayer->shutdown();
	if (m_imguiPadLayer)
		m_imguiPadLayer->shutdown();
	if (m_imguiTVLayer)
		m_imguiTVLayer->shutdown();

	m_imguiPadStatusLayer.reset();
	m_imguiPadLayer.reset();
	m_imguiTVStatusLayer.reset();
	m_imguiTVLayer.reset();
}

bool Renderer::ImguiBegin(bool mainWindow)
{
	sint32 w = 0, h = 0;
	if (mainWindow)
		WindowSystem::GetWindowPhysSize(w, h);
	else if (WindowSystem::IsPadWindowOpen())
		WindowSystem::GetPadWindowPhysSize(w, h);
	else
		return false;
		
	if (w == 0 || h == 0)
		return false;

	m_imguiMainWindow = mainWindow;
	m_imguiWidth = w;
	m_imguiHeight = h;

	auto& layerManager = spatial::imgui::LayerManager::getInstance();
	layerManager.setDisplaySize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
	const auto& layer = mainWindow ? m_imguiTVLayer : m_imguiPadLayer;
	if (!layer || !layer->getContext())
		return false;
	layer->setAsActiveContext();

	const Vector2f window_size{(float)w, (float)h};
	auto& io = ImGui::GetIO();
	io.DisplaySize = {window_size.x, window_size.y}; // should be only updated in the renderer and only when needed

	ImGui_PrecacheFonts();
	return true;
}

ImDrawData* Renderer::ImguiRenderStatusLayer()
{
	const auto& config = GetConfig();
	const auto& statusLayer = m_imguiMainWindow ? m_imguiTVStatusLayer : m_imguiPadStatusLayer;
	const auto& mainLayer = m_imguiMainWindow ? m_imguiTVLayer : m_imguiPadLayer;
	if (!statusLayer || !mainLayer || m_imguiWidth <= 0 || m_imguiHeight <= 0)
		return nullptr;

	spatial::imgui::LayerSettings settings{};
	sint32 statusWidth{}, statusHeight{};
	LatteOverlay_getStatusLayerCanvasSize(!m_imguiMainWindow, statusWidth, statusHeight);
	settings.widthContent = std::to_string(statusWidth);
	settings.heightContent = std::to_string(statusHeight);
	settings.margins = spatial::math::Vector4i(10, 10, 10, 10);
	settings.bgColor = spatial::math::color(0.05f, 0.05f, 0.05f, 0.65f);
	settings.borderColor = spatial::math::color(0.2f, 0.2f, 0.2f, 0.8f);
	settings.borderShadowColor = spatial::math::color(0.0f, 0.0f, 0.0f, 0.0f);
	settings.windowTitle = m_imguiMainWindow ? "Cemu TV status" : "Cemu Pad status";
	settings.needBasicWindowContainer = true;
	settings.showWindowTitle = false;
	settings.withScrollBar = false;

	const ScreenPosition statusPosition = config.overlay.position == ScreenPosition::kDisabled
		? ScreenPosition::kTopLeft
		: config.overlay.position;
	switch (statusPosition)
	{
	case ScreenPosition::kTopLeft:
		settings.horizontalAlign = spatial::imgui::HorizontalAlignment::Left;
		settings.verticalAlign = spatial::imgui::VerticalAlignment::Top;
		break;
	case ScreenPosition::kTopCenter:
		settings.horizontalAlign = spatial::imgui::HorizontalAlignment::Center;
		settings.verticalAlign = spatial::imgui::VerticalAlignment::Top;
		break;
	case ScreenPosition::kTopRight:
		settings.horizontalAlign = spatial::imgui::HorizontalAlignment::Right;
		settings.verticalAlign = spatial::imgui::VerticalAlignment::Top;
		break;
	case ScreenPosition::kBottomLeft:
		settings.horizontalAlign = spatial::imgui::HorizontalAlignment::Left;
		settings.verticalAlign = spatial::imgui::VerticalAlignment::Bottom;
		break;
	case ScreenPosition::kBottomCenter:
		settings.horizontalAlign = spatial::imgui::HorizontalAlignment::Center;
		settings.verticalAlign = spatial::imgui::VerticalAlignment::Bottom;
		break;
	case ScreenPosition::kBottomRight:
		settings.horizontalAlign = spatial::imgui::HorizontalAlignment::Right;
		settings.verticalAlign = spatial::imgui::VerticalAlignment::Bottom;
		break;
	default:
		settings.horizontalAlign = spatial::imgui::HorizontalAlignment::Left;
		settings.verticalAlign = spatial::imgui::VerticalAlignment::Top;
		break;
	}

	statusLayer->applyLayerSettings(settings);
	auto& layerManager = spatial::imgui::LayerManager::getInstance();
	layerManager.setDisplaySize(static_cast<uint32_t>(m_imguiWidth), static_cast<uint32_t>(m_imguiHeight));

	mainLayer->setAsActiveContext();
	const float deltaTime = ImGui::GetIO().DeltaTime;
	statusLayer->setAsActiveContext();
	auto& statusIo = ImGui::GetIO();
	statusIo.DisplaySize = ImVec2(static_cast<float>(m_imguiWidth), static_cast<float>(m_imguiHeight));
	statusIo.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	statusIo.DeltaTime = deltaTime;

	spatial::imgui::LayerInputEvent input{};
	bool captureMouse = false;
	bool captureKeyboard = false;
	ImDrawData* drawData = statusLayer->doFrame(input, captureMouse, captureKeyboard);
	mainLayer->setAsActiveContext();
	return drawData;
}

uint8 Renderer::SRGBComponentToRGB(uint8 ci)
{
	const float cs = (float)ci / 255.0f;
	float cl;
	if (cs <= 0.04045)
		cl = cs / 12.92f;
	else
		cl = std::pow((cs + 0.055f) / 1.055f, 2.4f);
	cl = std::min(cl, 1.0f);
	return (uint8)(cl * 255.0f);
}

uint8 Renderer::RGBComponentToSRGB(uint8 cli)
{
	const float cl = (float)cli / 255.0f;
	float cs;
	if (cl < 0.0031308)
		cs = 12.92f * cl;
	else
		cs = 1.055f * std::pow(cl, 0.41666f) - 0.055f;
	cs = std::max(std::min(cs, 1.0f), 0.0f);
	return (uint8)(cs * 255.0f);
}

void Renderer::RequestScreenshot(ScreenshotSaveFunction onSaveScreenshot)
{
	m_screenshot_requested = true;
	m_on_save_screenshot = onSaveScreenshot;
}

void Renderer::CancelScreenshotRequest()
{
	m_screenshot_requested = false;
	m_on_save_screenshot = {};
}


void Renderer::SaveScreenshot(const std::vector<uint8>& rgb_data, int width, int height, bool mainWindow)
{
	std::thread(
		[=, screenshotRequested = std::exchange(m_screenshot_requested, false), onSaveScreenshot = std::exchange(m_on_save_screenshot, {})]() {
			if (screenshotRequested && onSaveScreenshot)
			{
				auto notificationMessage = onSaveScreenshot(rgb_data, width, height, mainWindow);
				if (notificationMessage.has_value())
					LatteOverlay_pushNotification(notificationMessage.value(), 2500);
			}
		})
		.detach();
}
