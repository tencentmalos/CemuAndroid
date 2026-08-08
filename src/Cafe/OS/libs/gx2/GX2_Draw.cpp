#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "GX2.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Espresso/PPCState.h"

#include "Cafe/OS/common/OSCommon.h"

#include "GX2_Command.h"
#include "GX2_Draw.h"

#include <atomic>
#include <sstream>

#include "spatial/profiler/Profiler.h"

namespace
{
	constexpr uint64 kBotWTitleIdJp = 0x00050000101C9300ull;
	constexpr uint64 kBotWTitleIdUs = 0x00050000101C9400ull;
	constexpr uint64 kBotWTitleIdEu = 0x00050000101C9500ull;
	constexpr uint16 kBotWVersion = 208;

	std::atomic_bool s_structuredDrawFastPathEnabled{};
	std::atomic<uint64> s_structuredDrawEnableCount{};
	std::atomic<uint64> s_structuredDrawEmitted{};
	std::atomic<uint64> s_structuredDrawConsumed{};
	std::atomic<uint64> s_structuredDrawFallback{};
	std::atomic<uint64> s_structuredDrawLegacyWords{};
	std::atomic<uint64> s_structuredDrawPacketWords{};

	bool IsBotWTitle(uint64 titleId)
	{
		return titleId == kBotWTitleIdJp || titleId == kBotWTitleIdUs || titleId == kBotWTitleIdEu;
	}

	void ResetStructuredDrawCounters()
	{
		s_structuredDrawEmitted.store(0, std::memory_order_relaxed);
		s_structuredDrawConsumed.store(0, std::memory_order_relaxed);
		s_structuredDrawFallback.store(0, std::memory_order_relaxed);
		s_structuredDrawLegacyWords.store(0, std::memory_order_relaxed);
		s_structuredDrawPacketWords.store(0, std::memory_order_relaxed);
	}

	void RecordStructuredDrawEmission(uint32 legacyWords)
	{
		const uint64 emitted = s_structuredDrawEmitted.fetch_add(1, std::memory_order_relaxed) + 1;
		s_structuredDrawLegacyWords.fetch_add(legacyWords, std::memory_order_relaxed);
		s_structuredDrawPacketWords.fetch_add(IT_HLE_STRUCTURED_DRAW_WORDS + 1, std::memory_order_relaxed);
		if ((emitted & 0xFFFu) == 0)
		{
			SPATIAL_PROFILER_COUNTER_SET("cemu.structured_draw.emitted", emitted, "Cemu Structured Draw", "draws");
			SPATIAL_PROFILER_COUNTER_SET("cemu.structured_draw.words_avoided",
				s_structuredDrawLegacyWords.load(std::memory_order_relaxed) - s_structuredDrawPacketWords.load(std::memory_order_relaxed),
				"Cemu Structured Draw", "words");
		}
	}

	void SubmitStructuredDraw(GX2::GX2PrimitiveMode2 primitiveMode, uint32 count, GX2::GX2IndexType indexType,
		MPTR physicalIndexAddress, uint32 baseVertex, uint32 numInstances, uint32 baseInstance,
		bool indexed, bool hasBaseInstance, uint32 legacyWords)
	{
		GX2::GX2ReserveCmdSpace(IT_HLE_STRUCTURED_DRAW_WORDS + 1);
		uint32 control = static_cast<uint32>(primitiveMode) & IT_HLE_STRUCTURED_DRAW_PRIMITIVE_MASK;
		if (indexed)
			control |= IT_HLE_STRUCTURED_DRAW_INDEXED;
		if (hasBaseInstance)
			control |= IT_HLE_STRUCTURED_DRAW_HAS_BASE_INSTANCE;
		gx2WriteGather_submit(
			pm4HeaderType3(IT_HLE_STRUCTURED_DRAW, IT_HLE_STRUCTURED_DRAW_WORDS),
			control,
			count,
			static_cast<uint32>(indexType),
			physicalIndexAddress,
			baseVertex,
			numInstances,
			baseInstance);
		RecordStructuredDrawEmission(legacyWords);
	}
}

namespace GX2
{
	void GX2SetAttribBuffer(uint32 bufferIndex, uint32 sizeInBytes, uint32 stride, void* data)
	{
		GX2ReserveCmdSpace(9);
		MPTR physicalAddress = memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(data));
		// write PM4 command
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_RESOURCE, 8),
			0x8C0 + bufferIndex * 7,
			physicalAddress,
			sizeInBytes - 1, // size
			(stride & 0xFFFF) << 11, // stride
			0, // ukn
			0, // ukn
			0, // ukn
			0xC0000000); // ukn
	}

	void GX2DrawIndexedEx(GX2PrimitiveMode2 primitiveMode, uint32 count, GX2IndexType indexType, void* indexData, uint32 baseVertex, uint32 numInstances)
	{
		if (GX2IsStructuredDrawFastPathEnabled())
		{
			SubmitStructuredDraw(primitiveMode, count, indexType,
				memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(indexData)),
				baseVertex, numInstances, 0, true, false, 16);
			return;
		}
		GX2ReserveCmdSpace(3 + 3 + 2 + 2 + 6);
		gx2WriteGather_submit(
			// IT_SET_CTL_CONST
			pm4HeaderType3(IT_SET_CTL_CONST, 2), 0,
			baseVertex,
			// IT_SET_CONFIG_REG
			pm4HeaderType3(IT_SET_CONFIG_REG, 2), Latte::REGADDR::VGT_PRIMITIVE_TYPE - 0x2000,
			(uint32)primitiveMode,
			// IT_INDEX_TYPE
			pm4HeaderType3(IT_INDEX_TYPE, 1),
			(uint32)indexType,
			// IT_NUM_INSTANCES
			pm4HeaderType3(IT_NUM_INSTANCES, 1),
			numInstances,
			// IT_DRAW_INDEX_2
			pm4HeaderType3(IT_DRAW_INDEX_2, 5) | 0x00000001,
			-1,
			memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(indexData)),
			0,
			count,
			0);
	}

	void GX2DrawIndexedEx2(GX2PrimitiveMode2 primitiveMode, uint32 count, GX2IndexType indexType, void* indexData, uint32 baseVertex, uint32 numInstances, uint32 baseInstance)
	{
		if (GX2IsStructuredDrawFastPathEnabled())
		{
			SubmitStructuredDraw(primitiveMode, count, indexType,
				memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(indexData)),
				baseVertex, numInstances, baseInstance, true, true, 22);
			return;
		}
		GX2ReserveCmdSpace(3 + 3 + 3 + 2 + 2 + 6 + 3);
		gx2WriteGather_submit(
			// IT_SET_CTL_CONST
			pm4HeaderType3(IT_SET_CTL_CONST, 2), 0,
			baseVertex,
			// set base instance
			pm4HeaderType3(IT_SET_CTL_CONST, 2), 1,
			baseInstance,
			// IT_SET_CONFIG_REG
			pm4HeaderType3(IT_SET_CONFIG_REG, 2), Latte::REGADDR::VGT_PRIMITIVE_TYPE - 0x2000,
			(uint32)primitiveMode,
			// IT_INDEX_TYPE
			pm4HeaderType3(IT_INDEX_TYPE, 1),
			(uint32)indexType,
			// IT_NUM_INSTANCES
			pm4HeaderType3(IT_NUM_INSTANCES, 1),
			numInstances,
			// IT_DRAW_INDEX_2
			pm4HeaderType3(IT_DRAW_INDEX_2, 5) | 0x00000001,
			-1,
			memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(indexData)),
			0,
			count,
			0,
			// reset base instance
			pm4HeaderType3(IT_SET_CTL_CONST, 2), 1,
			0 // baseInstance
		);
	}

	void GX2DrawEx(GX2PrimitiveMode2 primitiveMode, uint32 count, uint32 baseVertex, uint32 numInstances)
	{
		if (GX2IsStructuredDrawFastPathEnabled())
		{
			SubmitStructuredDraw(primitiveMode, count, GX2IndexType::U32_BE, MPTR_NULL,
				baseVertex, numInstances, 0, false, false, 13);
			return;
		}
		GX2ReserveCmdSpace(3 + 3 + 2 + 2 + 6);
		gx2WriteGather_submit(
			// IT_SET_CTL_CONST
			pm4HeaderType3(IT_SET_CTL_CONST, 2), 0,
			baseVertex,
			// IT_SET_CONFIG_REG
			pm4HeaderType3(IT_SET_CONFIG_REG, 2), Latte::REGADDR::VGT_PRIMITIVE_TYPE - 0x2000,
			(uint32)primitiveMode,
			// IT_INDEX_TYPE
			pm4HeaderType3(IT_INDEX_TYPE, 1),
			(uint32)GX2IndexType::U32_BE,
			// IT_NUM_INSTANCES
			pm4HeaderType3(IT_NUM_INSTANCES, 1),
			numInstances,
			// IT_DRAW_INDEX_2
			pm4HeaderType3(IT_DRAW_INDEX_AUTO, 2) | 0x00000001,
			count,
			0 // DRAW_INITIATOR
		);
	}

	void GX2DrawIndexedImmediateEx(GX2PrimitiveMode2 primitiveMode, uint32 count, GX2IndexType indexType, void* indexData, uint32 baseVertex, uint32 numInstances)
	{
		if (GX2IsStructuredDrawFastPathEnabled())
			GX2RecordStructuredDrawFallback();
		uint32* indexDataU32 = (uint32*)indexData;
		uint32 numIndexU32s;
		bool use32BitIndices = false;
		if (indexType == GX2IndexType::U16_BE || indexType == GX2IndexType::U16_LE)
		{
			// 16bit indices
			numIndexU32s = (count + 1) / 2;
		}
		else if (indexType == GX2IndexType::U32_BE || indexType == GX2IndexType::U32_LE)
		{
			// 32bit indices
			numIndexU32s = count;
			use32BitIndices = true;
		}
		else
		{
			cemu_assert_unimplemented();
		}

		GX2ReserveCmdSpace(3 + 3 + 3 + 2 + 2 + 6 + 3 + numIndexU32s);

		if (numIndexU32s > 0x4000 - 2)
		{
			cemuLog_log(LogType::Force, "GX2DrawIndexedImmediateEx(): Draw exceeds maximum PM4 command size. Keep index size below 16KiB minus 8 byte");
			return;
		}

		// set base vertex
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CTL_CONST, 2));
		gx2WriteGather_submitU32AsBE(0);
		gx2WriteGather_submitU32AsBE(baseVertex);
		// set primitive mode
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONFIG_REG, 2));
		gx2WriteGather_submitU32AsBE(Latte::REGADDR::VGT_PRIMITIVE_TYPE - 0x2000);
		gx2WriteGather_submitU32AsBE((uint32)primitiveMode);
		// set index type
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_INDEX_TYPE, 1));
		gx2WriteGather_submitU32AsBE((uint32)indexType);
		// set number of instances
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_NUM_INSTANCES, 1));
		gx2WriteGather_submitU32AsBE((uint32)numInstances);
		// request indexed draw with indices embedded into command buffer
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_DRAW_INDEX_IMMD, 2 + numIndexU32s) | 0x00000001);
		gx2WriteGather_submitU32AsBE(count);
		gx2WriteGather_submitU32AsBE(0); // ukn
		if (use32BitIndices)
		{
			for (uint32 i = 0; i < numIndexU32s; i++)
			{
				gx2WriteGather_submitU32AsLE(indexDataU32[i]);
			}
		}
		else
		{
			for (uint32 i = 0; i < numIndexU32s; i++)
			{
				uint32 indexPair = indexDataU32[i];
				// swap index pair
				indexPair = (indexPair >> 16) | (indexPair << 16);
				gx2WriteGather_submitU32AsLE(indexPair);
			}
		}

	}

	struct GX2DispatchComputeParam
	{
		/* +0x00 */ uint32be worksizeX;
		/* +0x04 */ uint32be worksizeY;
		/* +0x08 */ uint32be worksizeZ;
	};

	void GX2DispatchCompute(GX2DispatchComputeParam* dispatchParam)
	{
		GX2ReserveCmdSpace(9 + 10);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_RESOURCE, 8),
			(mmSQ_CS_DISPATCH_PARAMS - mmSQ_TEX_RESOURCE_WORD0),
			memory_virtualToPhysical(MEMPTR<GX2DispatchComputeParam>(dispatchParam).GetMPTR()),
			0xF,
			0x862000,
			1,
			0xABCD1234,
			0xABCD1234,
			0xC0000000);

		// IT_EVENT_WRITE with RST_VTX_CNT?

		// set primitive mode
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONFIG_REG, 2));
		gx2WriteGather_submitU32AsBE(Latte::REGADDR::VGT_PRIMITIVE_TYPE - 0x2000);
		gx2WriteGather_submitU32AsBE(1); // mode
		// set number of instances
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_NUM_INSTANCES, 1));
		gx2WriteGather_submitU32AsBE(1); // numInstances

		uint32 workCount = (uint32)dispatchParam->worksizeX * (uint32)dispatchParam->worksizeY * (uint32)dispatchParam->worksizeZ;

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_DRAW_INDEX_AUTO, 2) | 0x00000001);
		gx2WriteGather_submitU32AsBE(workCount);
		gx2WriteGather_submitU32AsBE(0); // DRAW_INITIATOR (has source select for index generator + other unknown info)
	}

	bool GX2IsStructuredDrawFastPathEnabled()
	{
		return s_structuredDrawFastPathEnabled.load(std::memory_order_acquire);
	}

	void GX2RecordStructuredDrawConsumed()
	{
		const uint64 consumed = s_structuredDrawConsumed.fetch_add(1, std::memory_order_relaxed) + 1;
		if ((consumed & 0xFFFu) == 0)
			SPATIAL_PROFILER_COUNTER_SET("cemu.structured_draw.consumed", consumed, "Cemu Structured Draw", "draws");
	}

	void GX2RecordStructuredDrawFallback()
	{
		s_structuredDrawFallback.fetch_add(1, std::memory_order_relaxed);
	}

	std::string GX2GetStructuredDrawFastPathStatus()
	{
		const uint64 legacyWords = s_structuredDrawLegacyWords.load(std::memory_order_relaxed);
		const uint64 packetWords = s_structuredDrawPacketWords.load(std::memory_order_relaxed);
		std::ostringstream out;
		out << "structured_draw_fast_path:\n";
		out << "enabled=" << (GX2IsStructuredDrawFastPathEnabled() ? "true" : "false") << "\n";
		out << "enable_count=" << s_structuredDrawEnableCount.load(std::memory_order_relaxed) << "\n";
		out << "emitted=" << s_structuredDrawEmitted.load(std::memory_order_relaxed) << "\n";
		out << "consumed=" << s_structuredDrawConsumed.load(std::memory_order_relaxed) << "\n";
		out << "fallback=" << s_structuredDrawFallback.load(std::memory_order_relaxed) << "\n";
		out << "legacy_words=" << legacyWords << "\n";
		out << "packet_words=" << packetWords << "\n";
		out << "words_avoided=" << (legacyWords >= packetWords ? legacyWords - packetWords : 0) << "\n";
		return out.str();
	}

	void GX2DrawResetToDefaultState()
	{
		s_structuredDrawFastPathEnabled.store(false, std::memory_order_release);
		ResetStructuredDrawCounters();
	}

	void GX2DrawInit()
	{
		cafeExportRegister("gx2", GX2SetAttribBuffer, LogType::GX2);
		cafeExportRegister("gx2", GX2DrawIndexedEx, LogType::GX2);
		cafeExportRegister("gx2", GX2DrawIndexedEx2, LogType::GX2);
		cafeExportRegister("gx2", GX2DrawEx, LogType::GX2);
		cafeExportRegister("gx2", GX2DrawIndexedImmediateEx, LogType::GX2);
		cafeExportRegister("gx2", GX2DispatchCompute, LogType::GX2);
	}

}

void gx2Export_hook_EnableStructuredDrawFastPath(PPCInterpreter_t* hCPU)
{
	const bool requested = hCPU->gpr[3] != 0;
	if (!requested)
	{
		s_structuredDrawFastPathEnabled.store(false, std::memory_order_release);
		cemuLog_log(LogType::Force, "Structured draw fast path disabled by Guest patch");
		osLib_returnFromFunction(hCPU, 1);
		return;
	}

	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	const uint16 titleVersion = CafeSystem::GetForegroundTitleVersion();
	if (!IsBotWTitle(titleId) || titleVersion != kBotWVersion)
	{
		cemuLog_log(LogType::Force,
			"Structured draw fast path rejected for title {:016x} v{}; expected BotW v{}",
			titleId, titleVersion, kBotWVersion);
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	ResetStructuredDrawCounters();
	s_structuredDrawEnableCount.fetch_add(1, std::memory_order_relaxed);
	s_structuredDrawFastPathEnabled.store(true, std::memory_order_release);
	SPATIAL_PROFILER_COUNTER_SET("cemu.structured_draw.enabled", 1, "Cemu Structured Draw", "bool");
	cemuLog_log(LogType::Force, "Structured draw fast path enabled for BotW title {:016x} v{}", titleId, titleVersion);
	osLib_returnFromFunction(hCPU, 1);
}
