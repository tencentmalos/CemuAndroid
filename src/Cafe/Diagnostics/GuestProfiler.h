#pragma once

namespace spatial::debugbus
{
	class DebugCommandRegistry;
}

struct PPCInterpreter_t;

namespace GuestProfiler
{
	void Initialize();
	void Shutdown();
	void RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry);
	void RecordGx2Submission(PPCInterpreter_t* hCPU, uint32 submittedWords);
	void RecordGpuFence(PPCInterpreter_t* hCPU);
	void RecordGx2DrawDone(PPCInterpreter_t* hCPU);
	void RecordGx2SwapScanBuffers(PPCInterpreter_t* hCPU);
	void ConsumeGpuTag(bool begin, uint32 sectionId, uint32 guestThreadId,
		uint32 guestLr, uint32 generation);
	uint32 GetActiveGpuTagSection();
	void RecordGpuTagDrawBatch(uint32 sectionId, uint32 drawCount, uint32 fastDrawCount);
	void Reset();
}
