#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

class LatteTextureReadbackInfo
{
public:
	LatteTextureReadbackInfo(LatteTextureView* textureView)
		: hostTextureCopy(textureView->baseTexture), firstMip(textureView->firstMip), firstSlice(textureView->firstSlice),
		m_textureView(textureView)
	{}

	virtual ~LatteTextureReadbackInfo() = default;

	virtual void StartTransfer() = 0;
	virtual bool IsFinished() = 0;
	virtual void ForceFinish() {};
	// Non-zero values identify an ordered renderer completion domain. Waiting for
	// a later point must also complete every earlier point in the same domain.
	// Backends without that guarantee return zero and keep the per-job fallback.
	virtual uint64 GetOrderedCompletionPoint() const { return 0; }

	virtual uint8* GetData() = 0;
	virtual void ReleaseData() {};

	uint32 GetImageSizeBytes() const { return m_image_size; }
	void SetFeedbackGeneration(uint64 generation) { m_feedbackGeneration = generation; }
	uint64 GetFeedbackGeneration() const { return m_feedbackGeneration; }
	void SetPublishesFeedbackGeneration(bool publishes) { m_publishesFeedbackGeneration = publishes; }
	bool PublishesFeedbackGeneration() const { return m_publishesFeedbackGeneration; }
	void SetFeedbackPublicationHeld(bool held) { m_feedbackPublicationHeld = held; }
	bool IsFeedbackPublicationHeld() const { return m_feedbackPublicationHeld; }

	HRTick transferStartTime;
	HRTick waitStartTime;
	bool forceFinish{ false }; // set to true if not finished in time for dependent operation
	// texture info
	LatteTextureDefinition hostTextureCopy{};
	uint32 firstMip{};
	uint32 firstSlice{};

protected:
	LatteTextureView* m_textureView;
	uint32 m_image_size = 0;
	uint64 m_feedbackGeneration = 0;
	bool m_publishesFeedbackGeneration = false;
	bool m_feedbackPublicationHeld = false;
};
