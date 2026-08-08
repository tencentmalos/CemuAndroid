#pragma once

#include <string>

struct PPCInterpreter_t;

struct GX2GuestFeedbackPolicySnapshot
{
	bool enabled{};
	bool legacyDeferralEnabled{};
	uint32 mode{};
};

namespace GX2
{
	void GX2Init_event();
	void GX2EventResetToDefaultState();

	void GX2EventInit();
	void GX2WaitForVsync();
	void GX2WaitForFlip();
	bool GX2DrawDone();
	bool GX2WaitDisplayOrdinal(uint32 targetOrdinal, uint32 feedbackFrameId);
	std::string GX2GetDisplayOrdinalDependencyStatus();
	std::string GX2GetDrawDoneVisibilityDeferralStatus();
	std::string GX2GetGuestFeedbackStatus();
	GX2GuestFeedbackPolicySnapshot GX2GetGuestFeedbackPolicySnapshot();

	enum class GX2CallbackEventType
	{
		TIMESTAMP_TOP = 0,
		TIMESTAMP_BOTTOM = 1,
		VSYNC = 2,
		FLIP = 3,
		// 4 is buffer overrun?
	};
	inline constexpr size_t GX2CallbackEventTypeCount = 5;

	// notification callbacks for GPU
	void __GX2NotifyNewRetirementTimestamp(uint64 tsRetire);
	void __GX2NotifyEvent(GX2CallbackEventType eventType);

}

void gx2Export_hook_RegisterDisplayOrdinalCounter(PPCInterpreter_t* hCPU);
void gx2Export_hook_RegisterDrawDoneVisibilityDeferral(PPCInterpreter_t* hCPU);
void gx2Export_hook_RegisterGuestFeedbackPolicy(PPCInterpreter_t* hCPU);
