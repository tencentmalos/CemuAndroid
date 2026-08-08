#pragma once

#include <cstdint>
#include <string>

namespace RenderDocGuestFrameCapture
{
	std::string Request();
	std::string GetStatus();
	void OnGuestFrameBegin(std::uint32_t frameCounter);
	void OnGuestFramePresented(std::uint32_t frameCounter);
	void Shutdown();
}
