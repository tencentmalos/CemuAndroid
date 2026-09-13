/*
 * BotW Wii U JP v208 -- preCalc probe wrapper (G4).
 *
 * Hooks the true System::preCalc entry at 0x02C57E4C (mflr r0), verified from
 * the RPX text section and runtime. The hook is an ASM bridge (precalc_bridge.S)
 * that preserves the full incoming argument register state, calls this no-arg
 * observer, restores the registers, and tail-calls the original trampoline so
 * gameplay behaviour is preserved exactly.
 *
 * This C file only holds the observer logic and the SDK probe; it must not
 * touch the game's argument registers, which is why the entry point is asm.
 *
 * Addresses are specific to this exact RPX (source_rpx_sha256 ba58da5b95ce...);
 * they must not be reused for other versions (spec 4.2).
 */

#include "cemu/sdk_v1.h"

// SDK dispatcher gateway shim (sdk_call.S). r3..r8 register protocol.
extern "C" unsigned int cemu_sdk_call(unsigned int exportId,
									  unsigned int contextHandle,
									  const void* request, unsigned int requestSize,
									  void* response, unsigned int responseCapacity);

// Loader-reserved data symbol holding this module's SDK context handle.
extern "C" volatile unsigned int __cemu_custom_context;

// Observable validation state (readable via guest memory / debug dump).
extern "C" {
	volatile unsigned int g_precalc_hit_count = 0;
	volatile unsigned int g_sdk_ok_count = 0;
	volatile unsigned int g_sdk_last_status = 0xFFFFFFFF;
	volatile float g_sdk_last_x = 0.0f;
}

using namespace cemu_sdk_v1;

struct ProbeReq
{
	TransformBatchRequest header;
	float point[4];
};

static void RunSdkProbe()
{
	ProbeReq req;
	req.header.structSize = sizeof(TransformBatchRequest);
	req.header.elementCount = 1;
	req.header.stride = 16;
	req.header.flags = 0;
	for (int i = 0; i < 16; i++)
		req.header.matrix[i] = 0.0f;
	req.header.matrix[0] = 1.0f;
	req.header.matrix[5] = 1.0f;
	req.header.matrix[10] = 1.0f;
	req.header.matrix[15] = 1.0f;
	req.header.matrix[3] = 10.0f;
	req.header.matrix[7] = 20.0f;
	req.header.matrix[11] = 30.0f;
	req.point[0] = 1.0f;
	req.point[1] = 2.0f;
	req.point[2] = 3.0f;
	req.point[3] = 1.0f;

	float out[4] = {0, 0, 0, 0};
	const unsigned int status = cemu_sdk_call(
		Export_MathTransformBatch, __cemu_custom_context,
		&req, sizeof(req), out, sizeof(out));
	g_sdk_last_status = status;
	if (status == Status_Ok)
	{
		g_sdk_last_x = out[0];
		if (out[0] == 11.0f && out[1] == 22.0f && out[2] == 33.0f)
			g_sdk_ok_count++;
	}
}

// No-argument observer called from the ASM bridge. Must not rely on or disturb
// the game's argument registers -- the bridge saves/restores them around this.
extern "C" void precalc_observe()
{
	g_precalc_hit_count++;
	// Exercise the SDK once every 128 entries to keep overhead negligible.
	if ((g_precalc_hit_count & 127u) == 1u)
		RunSdkProbe();
}
