#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#define PERFORMANCE_MONITOR_TRACK_CYCLES	(5) // one cycle lasts one second

// todo - replace PPCTimer with HighResolutionTimer.h
uint64 PPCTimer_getRawTsc();
uint64 PPCTimer_tscToMicroseconds(uint64 us);

class LattePerfStatTimer
{
public:
	void beginMeasuring()
	{
		timerStart = PPCTimer_getRawTsc();
	}

	void endMeasuring()
	{
		uint64 dif = PPCTimer_getRawTsc() - timerStart;
		currentSum += dif;
	}

	void frameFinished()
	{
		previousFrame = currentSum;
		currentSum = 0;
	}

	uint64 getPreviousFrameValue()
	{
		return previousFrame;
	}

private:
	uint64 currentSum{};
	uint64 previousFrame{};
	uint64 timerStart{};
};

class LattePerfStatCounter
{
public:
	void increment()
	{
		m_value++;
	}

	void decrement()
	{
		cemu_assert_debug(m_value > 0);
		m_value--;
	}

	void decrement(uint32 count)
	{
		cemu_assert_debug(count <= m_value);
		m_value -= count;
	}

	uint32 get()
	{
		return m_value;
	}

	void reset()
	{
		m_value = 0;
	}

private:
	std::atomic_uint32_t m_value{};
};

typedef struct
{
	struct
	{
		// CPU
		uint64 lastCycleCount;
		uint64 skippedCycles;
		uint32 recompilerLeaveCount; // increased everytime the recompiler switches back to interpreter
		uint32 threadLeaveCount; // increased everytime a thread gives up it's timeslice
		// GPU
		uint32 lastUpdate;
		uint32 frameCounter;
		uint32 drawCallCounter;
		uint32 fastDrawCallCounter;
		uint32 shaderBindCount;
		uint64 vertexDataUploaded; // amount of vertex data uploaded to GPU (bytes)
		uint64 vertexDataCached; // amount of vertex data reused from GPU cache (bytes)
		uint64 uniformBankUploadedData; // amount of uniform buffer data (excluding remapped uniforms) uploaded to GPU
		uint64 uniformBankUploadedCount; // number of separate uploads for uniformBankDataUploaded
		uint64 indexDataUploaded;
		uint64 indexDataCached;
	}cycle[PERFORMANCE_MONITOR_TRACK_CYCLES];
	sint32 cycleIndex;
	// new stats
	LattePerfStatTimer gpuTime_frameTime;
	LattePerfStatTimer gpuTime_shaderCreate;
	LattePerfStatTimer gpuTime_idleTime; // time spent waiting for new commands from CPU
	LattePerfStatTimer gpuTime_fenceTime; // time spent waiting for fence condition

	LattePerfStatTimer gpuTime_dcStageTextures; // drawcall texture/mrt setup
	LattePerfStatTimer gpuTime_dcStageVertexMgr; // drawcall vertex setup and upload
	LattePerfStatTimer gpuTime_dcStageShaderAndUniformMgr; // drawcall shader setup and uniform management/upload
	LattePerfStatTimer gpuTime_dcStageIndexMgr; // drawcall index data setup and upload
	LattePerfStatTimer gpuTime_dcStageMRT; // drawcall render target API

	LattePerfStatTimer gpuTime_dcStageDrawcallAPI; // drawcall api call
	LattePerfStatTimer gpuTime_waitForAsync; // waiting for operations to complete (e.g. GX2DrawDone or force texture readback) Also includes texture readback and occlusion query polling logic

	// generic
	uint32 numCompiledVS; // number of compiled vertex shader programs
	uint32 numCompiledGS; // number of compiled geometry shader programs
	uint32 numCompiledPS; // number of compiled pixel shader programs

	// Vulkan
	struct  
	{
		LattePerfStatCounter numDescriptorSets;
		LattePerfStatCounter numDescriptorDynUniformBuffers;
		LattePerfStatCounter numDescriptorStorageBuffers;
		LattePerfStatCounter numDescriptorSamplerTextures;
		LattePerfStatCounter numGraphicPipelines;
		LattePerfStatCounter numImages;
		LattePerfStatCounter numImageViews;
		LattePerfStatCounter numSamplers;
		LattePerfStatCounter numRenderPass;
		LattePerfStatCounter numFramebuffer;

		// per frame
		LattePerfStatCounter numDrawBarriersPerFrame;
		LattePerfStatCounter numBeginRenderpassPerFrame;
	}vk;

	// calculated stats (per frame)
	struct
	{
		uint32 indexDataUploadPerFrame;
	}stats;
}performanceMonitor_t;

extern performanceMonitor_t performanceMonitor;

enum class LatteCommandPacketCategory : std::uint8_t
{
	Draw,
	RegisterContext,
	RegisterResource,
	RegisterConstant,
	RegisterSampler,
	RegisterConfig,
	RegisterOther,
	IndirectBuffer,
	Synchronization,
	Surface,
	Filler,
	Other,
	Count,
};

enum class LatteCommandHostTimeCategory : std::uint8_t
{
	ConsumeSubmission,
	DrawTranslate,
	DrawSequenceBegin,
	DrawSequenceEnd,
	SequenceShader,
	SequenceFramebuffer,
	SequenceTextures,
	SequenceApplyRenderTarget,
	SequenceViewportScissor,
	FullDrawPrelude,
	FullDrawUniforms,
	FullDrawIndices,
	FullDrawBuffers,
	FullDrawPipeline,
	FullDrawDescriptors,
	FullDrawHostState,
	FullDrawApi,
	SequenceEndTrackUpdates,
	SequenceEndReadback,
	SequenceEndSubmit,
	Count,
};

enum class LatteDrawPassEndReason : std::uint8_t
{
	CommandStreamEnd,
	ResourceChange,
	ContextChange,
	SamplerChange,
	UnsupportedCommand,
	Streamout,
	Explicit,
	Count,
};

enum class LatteVulkanSubmitReason : std::uint8_t
{
	DrawThreshold,
	Readback,
	OcclusionQuery,
	CommandProcessorIdle,
	ExplicitFlush,
	CompletionWait,
	FrameBoundary,
	SwapchainAcquire,
	Present,
	TextureDump,
	SwapchainRecreate,
	Shutdown,
	Other,
	Count,
};

enum class LatteVulkanRenderPassEndReason : std::uint8_t
{
	FramebufferChange,
	SelfDependency,
	GenericBarrier,
	Query,
	Readback,
	SurfaceCopy,
	CommandBufferSubmit,
	ImGui,
	Clear,
	Present,
	TextureOperation,
	BufferOperation,
	DepthStoreUpgrade,
	Other,
	Count,
};

enum class LatteBufferCacheUploadSource : std::uint8_t
{
	Vertex,
	VertexUniform,
	GeometryUniform,
	PixelUniform,
	Other,
	Count,
};

void LattePerformanceMonitor_recordGuestCommandSubmission(uint32 words);
void LattePerformanceMonitor_recordHostCommandSubmission(uint32 words);
void LattePerformanceMonitor_recordHostCommandPacket(LatteCommandPacketCategory category, uint32 words);
void LattePerformanceMonitor_recordHostCommandTime(LatteCommandHostTimeCategory category, uint64 nanoseconds);
void LattePerformanceMonitor_recordHostRegisterPacketOutcome(LatteCommandPacketCategory category,
	bool changed, uint32 words, uint32 elidedRegisterStores);
void LattePerformanceMonitor_recordHostDrawPass();
void LattePerformanceMonitor_recordHostDraw(bool fastDraw);
void LattePerformanceMonitor_recordHostDrawPassEnd(LatteDrawPassEndReason reason);
void LattePerformanceMonitor_recordHostContextDrawPassBreak(uint32 registerStart, uint32 registerEnd);
void LattePerformanceMonitor_recordHostVulkanSubmit(LatteVulkanSubmitReason reason,
	uint32 recordedDrawPasses, uint64 cpuNanoseconds);
void LattePerformanceMonitor_recordHostVulkanRenderPassEnd(LatteVulkanRenderPassEndReason reason,
	uint32 drawCount);
void LattePerformanceMonitor_recordHostVulkanDepthStoreOmittedPass();
void LattePerformanceMonitor_recordHostVulkanSelfDependencySplit(bool hasNonPixelDependency);
void LattePerformanceMonitor_recordHostBufferCacheUpload(LatteBufferCacheUploadSource source,
	uint32 bytes);
void LattePerformanceMonitor_recordHostBufferCacheUploadBatch(uint32 regions,
	uint32 copyCommands);
void LattePerformanceMonitor_recordHostBufferCacheCopy(uint32 bytes);
void LattePerformanceMonitor_recordHostVertexBufferBind(uint32 bytes);
void LattePerformanceMonitor_recordHostUniformRingBankBind(uint32 bytes, bool reused);
void LattePerformanceMonitor_recordHostUniformRingBankUpload(uint32 bytes);
std::string LattePerformanceMonitor_getCommandTranslationStatus();
void LattePerformanceMonitor_resetCommandTranslationStatus();

void LattePerformanceMonitor_frameEnd();
void LattePerformanceMonitor_frameBegin();

#define beginPerfMonProfiling(__obj) if( THasProfiling ) __obj.beginMeasuring()
#define endPerfMonProfiling(__obj) if( THasProfiling ) __obj.endMeasuring()
