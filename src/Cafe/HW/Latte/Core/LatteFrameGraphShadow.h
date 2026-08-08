#pragma once

#include <cstdint>
#include <string>

class LatteTextureView;

namespace LatteFrameGraphShadow
{
	enum class HardBarrierReason : std::uint8_t
	{
		WaitGuestMemory,
		GuestMemoryWrite,
		Semaphore,
		BottomOfPipe,
		GuestVisibility,
		DisplayOrdinal,
		WaitForFlip,
		SurfaceSync,
		UnknownCommand,
		Count,
	};

	void SetEnabled(bool enabled);
	[[nodiscard]] bool IsEnabled();
	void Reset();
	[[nodiscard]] std::string GetStatus();

	void BeginFrame();
	void EndFrame();

	void BeginRenderNode(std::uint32_t guestTag, bool fastDraw, std::uint32_t drawCount);
	void RecordSurfaceBinding(const LatteTextureView* view, std::uint32_t bindingSlot);
	void RecordBufferBinding(std::uint32_t bindingSlot, std::uint32_t address, std::uint32_t size);
	void RecordBufferRead(std::uint32_t address, std::uint32_t size);
	void EndRenderNode();

	void RecordTransfer(std::uint32_t sourceAddress, std::uint64_t sourceSize,
		std::uint32_t destinationAddress, std::uint64_t destinationSize);
	void RecordClear(std::uint32_t colorAddress, std::uint64_t colorSize,
		std::uint32_t depthAddress, std::uint64_t depthSize, std::uint32_t clearMask);
	void RecordQuery(std::uint32_t address, bool begin);
	void RecordReadback(const LatteTextureView* view);
	void RecordPresent(std::uint32_t address, std::uint64_t size);
	void RecordHardBarrier(HardBarrierReason reason, std::uint32_t address = 0,
		std::uint32_t size = 0);

	void RecordActualRenderPass();
	void RecordActualSubmit();
}
