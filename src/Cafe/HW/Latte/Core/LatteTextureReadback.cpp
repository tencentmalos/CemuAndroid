#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteFrameGraphShadow.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/Diagnostics/SurfaceResolutionDiagnostics.h"
#include "spatial/profiler/Profiler.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>
#include <vector>

#define LOG_READBACK_TIME

namespace
{
	struct FeedbackObservationState
	{
		std::atomic<uint64> boundaries{};
		std::atomic<uint64> signatureMatches{};
		std::atomic<uint64> signatureMismatches{};
		std::atomic<uint64> fastPath{};
		std::atomic<uint64> fastPathAlreadyVisible{};
		std::atomic<uint64> fastPathPreviousGeneration{};
		std::atomic<uint64> fallback{};
		std::atomic<uint64> fallbackObserveOnly{};
		std::atomic<uint64> fallbackNoPreviousGeneration{};
		std::atomic<uint64> fallbackSignatureMismatch{};
		std::atomic<uint64> fallbackGenerationGap{};
		std::atomic<uint64> fallbackCompletionUnavailable{};
		std::atomic<uint64> fallbackPartialPublication{};
		std::atomic<uint64> generationScheduled{};
		std::atomic<uint64> generationPublished{};
		std::atomic<uint64> generationConsumed{};
		std::atomic<uint64> generationHeld{};
		std::atomic<uint64> lastSignature{};
		std::atomic<uint64> lastCompletionPoint{};
		std::atomic<uint64> lastBytes{};
		std::atomic<uint32> lastJobs{};
		std::atomic<uint32> lastCompletedJobs{};
		std::atomic<uint32> lastActiveJobs{};
		std::atomic<uint32> last64x8Jobs{};
		std::atomic<uint32> last64x3Jobs{};
		std::atomic<uint32> last64x1Jobs{};
		std::atomic<uint32> lastGenerationAge{};
		std::atomic<uint32> heldJobs{};
		std::atomic<uint32> lastFallbackReason{};
		std::atomic_bool lastSignatureMatched{};
	};

	FeedbackObservationState s_feedbackObservation;

	enum class FeedbackFallbackReason : uint32
	{
		None = 0,
		ObserveOnly = 1,
		NoPreviousGeneration = 2,
		SignatureMismatch = 3,
		GenerationGap = 4,
		CompletionUnavailable = 5,
		PartialPublication = 6,
	};

	const char* GetFeedbackFallbackReasonName(FeedbackFallbackReason reason)
	{
		switch (reason)
		{
		case FeedbackFallbackReason::None:
			return "none";
		case FeedbackFallbackReason::ObserveOnly:
			return "observe_only";
		case FeedbackFallbackReason::NoPreviousGeneration:
			return "no_previous_generation";
		case FeedbackFallbackReason::SignatureMismatch:
			return "signature_mismatch";
		case FeedbackFallbackReason::GenerationGap:
			return "generation_gap";
		case FeedbackFallbackReason::CompletionUnavailable:
			return "completion_unavailable";
		case FeedbackFallbackReason::PartialPublication:
			return "partial_publication";
		}
		return "unknown";
	}

	struct FeedbackBatchStats
	{
		uint32 jobs{};
		uint64 bytes{};
		uint32 jobs64x8{};
		uint32 jobs64x3{};
		uint32 jobs64x1{};
		std::vector<uint64> jobSignatures;
	};

	std::mutex s_completedFeedbackCandidatesMutex;
	FeedbackBatchStats s_completedFeedbackCandidates;

	void AddFeedbackJob(FeedbackBatchStats& stats, const LatteTextureReadbackInfo& readbackInfo);
	bool IsExpectedFeedbackJob(const LatteTextureReadbackInfo& readbackInfo);

	FeedbackBatchStats TakeCompletedFeedbackCandidates()
	{
		std::lock_guard lock(s_completedFeedbackCandidatesMutex);
		FeedbackBatchStats result = std::move(s_completedFeedbackCandidates);
		s_completedFeedbackCandidates = {};
		return result;
	}

	void ResetCompletedFeedbackCandidates()
	{
		std::lock_guard lock(s_completedFeedbackCandidatesMutex);
		s_completedFeedbackCandidates = {};
	}

	void RecordCompletedFeedbackCandidate(const LatteTextureReadbackInfo& readbackInfo)
	{
		// Readbacks that have already reached Guest memory no longer constrain this
		// boundary. Keep only the three verified feedback shapes so unrelated async
		// completions cannot turn a safe current-generation publication into a false
		// signature mismatch. Unknown jobs that are still active remain part of the
		// strict boundary signature below and therefore force a full-sync fallback.
		if (!IsExpectedFeedbackJob(readbackInfo))
			return;
		std::lock_guard lock(s_completedFeedbackCandidatesMutex);
		AddFeedbackJob(s_completedFeedbackCandidates, readbackInfo);
	}

	void PublishFeedbackGeneration(uint64 generation)
	{
		if (generation == 0)
			return;
		uint64 published = s_feedbackObservation.generationPublished.load(std::memory_order_relaxed);
		while (published < generation &&
			!s_feedbackObservation.generationPublished.compare_exchange_weak(published, generation,
				std::memory_order_release, std::memory_order_relaxed))
		{
		}
		SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.generation_published",
			s_feedbackObservation.generationPublished.load(std::memory_order_relaxed),
			"Cemu Guest Feedback", "generation");
		uint64 heldGeneration = generation;
		if (s_feedbackObservation.generationHeld.compare_exchange_strong(heldGeneration, 0,
			std::memory_order_release, std::memory_order_relaxed))
		{
			s_feedbackObservation.heldJobs.store(0, std::memory_order_relaxed);
			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.generation_held", 0,
				"Cemu Guest Feedback", "generation");
			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.held_jobs", 0,
				"Cemu Guest Feedback", "jobs");
		}
	}

	uint64 MixFeedbackSignature(uint64 value)
	{
		value ^= value >> 30;
		value *= 0xBF58476D1CE4E5B9ull;
		value ^= value >> 27;
		value *= 0x94D049BB133111EBull;
		return value ^ (value >> 31);
	}

	uint64 GetFeedbackJobSignature(const LatteTextureReadbackInfo& readbackInfo)
	{
		const auto& texture = readbackInfo.hostTextureCopy;
		uint64 signature = static_cast<uint64>(texture.width) << 32;
		signature ^= static_cast<uint64>(texture.height) << 16;
		signature ^= static_cast<uint64>(texture.pitch);
		signature = MixFeedbackSignature(signature ^ (static_cast<uint64>(texture.format) << 32));
		signature ^= MixFeedbackSignature(static_cast<uint64>(readbackInfo.GetImageSizeBytes()) << 16 |
			static_cast<uint64>(readbackInfo.firstMip) << 8 | readbackInfo.firstSlice);
		return signature;
	}

	bool IsFeedbackJob(const LatteTextureReadbackInfo& readbackInfo, uint32 width, uint32 height,
		uint32 pitch, uint32 format, uint32 bytes)
	{
		const auto& texture = readbackInfo.hostTextureCopy;
		return texture.width == width && texture.height == height && texture.pitch == pitch &&
			static_cast<uint32>(texture.format) == format && readbackInfo.GetImageSizeBytes() == bytes &&
			readbackInfo.firstMip == 0 && readbackInfo.firstSlice == 0;
	}

	bool IsExpectedFeedbackJob(const LatteTextureReadbackInfo& readbackInfo)
	{
		return IsFeedbackJob(readbackInfo, 64, 8, 64, 0x823, 8192) ||
			IsFeedbackJob(readbackInfo, 64, 3, 64, 0x820, 1536) ||
			IsFeedbackJob(readbackInfo, 64, 1, 64, 0x823, 1024);
	}

	void AddFeedbackJob(FeedbackBatchStats& stats, const LatteTextureReadbackInfo& readbackInfo)
	{
		stats.jobs++;
		stats.bytes += readbackInfo.GetImageSizeBytes();
		stats.jobs64x8 += IsFeedbackJob(readbackInfo, 64, 8, 64, 0x823, 8192) ? 1 : 0;
		stats.jobs64x3 += IsFeedbackJob(readbackInfo, 64, 3, 64, 0x820, 1536) ? 1 : 0;
		stats.jobs64x1 += IsFeedbackJob(readbackInfo, 64, 1, 64, 0x823, 1024) ? 1 : 0;
		stats.jobSignatures.emplace_back(GetFeedbackJobSignature(readbackInfo));
	}

	uint64 GetFeedbackBatchSignature(FeedbackBatchStats& stats)
	{
		std::sort(stats.jobSignatures.begin(), stats.jobSignatures.end());
		uint64 signature = 0xCBF29CE484222325ull;
		for (uint64 jobSignature : stats.jobSignatures)
		{
			signature ^= jobSignature;
			signature *= 0x100000001B3ull;
		}
		return signature;
	}

	bool IsExpectedFeedbackBatch(const FeedbackBatchStats& stats)
	{
		return stats.jobs == 3 && stats.bytes == 10752 && stats.jobs64x8 == 1 &&
			stats.jobs64x3 == 1 && stats.jobs64x1 == 1;
	}
}

struct LatteTextureReadbackQueueEntry
{
	HRTick initiateTime;
	uint32 lastUpdateDrawcallIndex;
	LatteTextureView* textureView;
};

std::vector<LatteTextureReadbackQueueEntry> sTextureScheduledReadbacks; // readbacks that have been queued but the actual transfer has not yet been started
std::queue<LatteTextureReadbackInfo*> sTextureActiveReadbackQueue; // readbacks in flight

void LatteTextureReadback_StartTransfer(LatteTextureView* textureView)
{
	cemuLog_log(LogType::TextureReadback, "[TextureReadback-Start] PhysAddr {:08x} Res {}x{} Fmt {} Slice {} Mip {}", textureView->baseTexture->physAddress, textureView->baseTexture->width, textureView->baseTexture->height, textureView->baseTexture->format, textureView->firstSlice, textureView->firstMip);
	HRTick currentTick = HighResolutionTimer().now().getTick();
	LatteTexture* texture = textureView->baseTexture;
	const auto representation = texture->RepresentationsAlias() ? LatteTextureRepresentation::Render : LatteTextureRepresentation::GuestNative;
	const LatteSurfaceSubresourceRange range{static_cast<uint32>(textureView->firstMip), 1,
		static_cast<uint32>(textureView->firstSlice), 1};
	const auto syncResult = LatteTexture_EnsureRepresentationCurrent(texture, representation, range);
	if (!syncResult.succeeded)
	{
		SurfaceResolutionDiagnostics::RecordReadback(*texture, false);
		cemuLog_log(LogType::Force, "Texture readback representation sync failed for {:08x}", texture->physAddress);
		return;
	}
	// create info entry and store in ordered linked list
	LatteTextureReadbackInfo* readbackInfo = g_renderer->texture_createReadback(textureView, representation);
	SurfaceResolutionDiagnostics::RecordReadback(*textureView->baseTexture, readbackInfo != nullptr);
	if (!readbackInfo)
		return;
	sTextureActiveReadbackQueue.push(readbackInfo);
	readbackInfo->StartTransfer();
	readbackInfo->transferStartTime = currentTick;
}

/*
 * Checks for queued transfers and starts them if at least five drawcalls have passed since the last write
 * Called after a draw sequence is completed
 * Returns true if at least one transfer was started
 */
bool LatteTextureReadback_Update(bool forceStart)
{
	bool hasStartedTransfer = false;
	for (size_t i = 0; i < sTextureScheduledReadbacks.size(); i++)
	{
		LatteTextureReadbackQueueEntry& entry = sTextureScheduledReadbacks[i];
		uint32 numElapsedDrawcalls = LatteGPUState.drawCallCounter - entry.lastUpdateDrawcallIndex;
		if (forceStart || numElapsedDrawcalls >= 5)
		{
#ifdef LOG_READBACK_TIME
			double elapsedSecondsSinceInitiate = HighResolutionTimer::getTimeDiff(entry.initiateTime, HighResolutionTimer().now().getTick());
			cemuLog_log(LogType::TextureReadback, "[TextureReadback-Update] Starting transfer for {:08x} after {} elapsed drawcalls. Time since initiate: {:.4} Force-start: {}", entry.textureView->baseTexture->physAddress, numElapsedDrawcalls, elapsedSecondsSinceInitiate, forceStart?"yes":"no");
#endif
			LatteTextureReadback_StartTransfer(entry.textureView);
			// remove element
			vectorRemoveByIndex(sTextureScheduledReadbacks, i);
			i--;
			hasStartedTransfer = true;
		}
	}
	return hasStartedTransfer;
}

/*
 * Called when a texture is deleted
 */
void LatteTextureReadback_NotifyTextureDeletion(LatteTexture* texture)
{
	// delete from queue
	for (size_t i = 0; i < sTextureScheduledReadbacks.size(); i++)
	{
		LatteTextureReadbackQueueEntry& entry = sTextureScheduledReadbacks[i];
		if (entry.textureView->baseTexture == texture)
		{
			vectorRemoveByIndex(sTextureScheduledReadbacks, i);
			break;
		}
	}
}

void LatteTextureReadback_Initate(LatteTextureView* textureView)
{
	SurfaceResolutionDiagnostics::RecordUsage(*textureView->baseTexture, LatteSurfaceUsage::CpuReadback);
	// check if texture isn't already queued for transfer
	for (size_t i = 0; i < sTextureScheduledReadbacks.size(); i++)
	{
		LatteTextureReadbackQueueEntry& entry = sTextureScheduledReadbacks[i];
		if (entry.textureView == textureView)
		{
			entry.lastUpdateDrawcallIndex = LatteGPUState.drawCallCounter;
			return;
		}
	}
	LatteFrameGraphShadow::RecordReadback(textureView);
	// queue
	LatteTextureReadbackQueueEntry queueEntry;
	queueEntry.initiateTime = HighResolutionTimer().now().getTick();
	queueEntry.textureView = textureView;
	queueEntry.lastUpdateDrawcallIndex = LatteGPUState.drawCallCounter;
	sTextureScheduledReadbacks.emplace_back(queueEntry);
}

void LatteTextureReadback_ResetFeedbackObservation()
{
	s_feedbackObservation.boundaries.store(0, std::memory_order_relaxed);
	s_feedbackObservation.signatureMatches.store(0, std::memory_order_relaxed);
	s_feedbackObservation.signatureMismatches.store(0, std::memory_order_relaxed);
	s_feedbackObservation.fastPath.store(0, std::memory_order_relaxed);
	s_feedbackObservation.fastPathAlreadyVisible.store(0, std::memory_order_relaxed);
	s_feedbackObservation.fastPathPreviousGeneration.store(0, std::memory_order_relaxed);
	s_feedbackObservation.fallback.store(0, std::memory_order_relaxed);
	s_feedbackObservation.fallbackObserveOnly.store(0, std::memory_order_relaxed);
	s_feedbackObservation.fallbackNoPreviousGeneration.store(0, std::memory_order_relaxed);
	s_feedbackObservation.fallbackSignatureMismatch.store(0, std::memory_order_relaxed);
	s_feedbackObservation.fallbackGenerationGap.store(0, std::memory_order_relaxed);
	s_feedbackObservation.fallbackCompletionUnavailable.store(0, std::memory_order_relaxed);
	s_feedbackObservation.fallbackPartialPublication.store(0, std::memory_order_relaxed);
	s_feedbackObservation.generationScheduled.store(0, std::memory_order_relaxed);
	s_feedbackObservation.generationPublished.store(0, std::memory_order_relaxed);
	s_feedbackObservation.generationConsumed.store(0, std::memory_order_relaxed);
	s_feedbackObservation.generationHeld.store(0, std::memory_order_release);
	s_feedbackObservation.lastSignature.store(0, std::memory_order_relaxed);
	s_feedbackObservation.lastCompletionPoint.store(0, std::memory_order_relaxed);
	s_feedbackObservation.lastBytes.store(0, std::memory_order_relaxed);
	s_feedbackObservation.lastJobs.store(0, std::memory_order_relaxed);
	s_feedbackObservation.lastCompletedJobs.store(0, std::memory_order_relaxed);
	s_feedbackObservation.lastActiveJobs.store(0, std::memory_order_relaxed);
	s_feedbackObservation.last64x8Jobs.store(0, std::memory_order_relaxed);
	s_feedbackObservation.last64x3Jobs.store(0, std::memory_order_relaxed);
	s_feedbackObservation.last64x1Jobs.store(0, std::memory_order_relaxed);
	s_feedbackObservation.lastGenerationAge.store(0, std::memory_order_relaxed);
	s_feedbackObservation.heldJobs.store(0, std::memory_order_relaxed);
	s_feedbackObservation.lastFallbackReason.store(static_cast<uint32>(FeedbackFallbackReason::None), std::memory_order_relaxed);
	s_feedbackObservation.lastSignatureMatched.store(false, std::memory_order_relaxed);
	ResetCompletedFeedbackCandidates();
}

void LatteTextureReadback_RecordFeedbackConsumed()
{
	const uint64 scheduled = s_feedbackObservation.generationScheduled.load(std::memory_order_acquire);
	const uint64 published = s_feedbackObservation.generationPublished.load(std::memory_order_acquire);
	const uint64 consumed = std::min(scheduled, published);
	const uint32 age = static_cast<uint32>(scheduled >= consumed ? scheduled - consumed : 0);
	s_feedbackObservation.generationConsumed.store(consumed, std::memory_order_relaxed);
	s_feedbackObservation.lastGenerationAge.store(age, std::memory_order_relaxed);
	SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.generation_consumed", consumed,
		"Cemu Guest Feedback", "generation");
	SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.generation_age", age,
		"Cemu Guest Feedback", "frames");
}

LatteGuestFeedbackSnapshot LatteTextureReadback_GetFeedbackSnapshot()
{
	return {
		.fastPath = s_feedbackObservation.fastPath.load(std::memory_order_relaxed),
		.fastPathAlreadyVisible = s_feedbackObservation.fastPathAlreadyVisible.load(std::memory_order_relaxed),
		.fastPathPreviousGeneration = s_feedbackObservation.fastPathPreviousGeneration.load(std::memory_order_relaxed),
		.fallback = s_feedbackObservation.fallback.load(std::memory_order_relaxed),
		.generationScheduled = s_feedbackObservation.generationScheduled.load(std::memory_order_relaxed),
		.generationPublished = s_feedbackObservation.generationPublished.load(std::memory_order_relaxed),
		.generationConsumed = s_feedbackObservation.generationConsumed.load(std::memory_order_relaxed),
		.generationHeld = s_feedbackObservation.generationHeld.load(std::memory_order_relaxed),
		.generationAge = s_feedbackObservation.lastGenerationAge.load(std::memory_order_relaxed),
		.heldJobs = s_feedbackObservation.heldJobs.load(std::memory_order_relaxed),
		.fallbackReason = s_feedbackObservation.lastFallbackReason.load(std::memory_order_relaxed),
		.signatureMatched = s_feedbackObservation.lastSignatureMatched.load(std::memory_order_relaxed),
	};
}

std::string LatteTextureReadback_GetFeedbackObservationStatus()
{
	std::ostringstream out;
	out << "boundaries=" << s_feedbackObservation.boundaries.load(std::memory_order_relaxed) << "\n";
	out << "signature_matches=" << s_feedbackObservation.signatureMatches.load(std::memory_order_relaxed) << "\n";
	out << "signature_mismatches=" << s_feedbackObservation.signatureMismatches.load(std::memory_order_relaxed) << "\n";
	out << "fast_path=" << s_feedbackObservation.fastPath.load(std::memory_order_relaxed) << "\n";
	out << "fast_path_already_visible=" << s_feedbackObservation.fastPathAlreadyVisible.load(std::memory_order_relaxed) << "\n";
	out << "fast_path_previous_generation=" << s_feedbackObservation.fastPathPreviousGeneration.load(std::memory_order_relaxed) << "\n";
	out << "fallback=" << s_feedbackObservation.fallback.load(std::memory_order_relaxed) << "\n";
	out << "fallback_observe_only=" << s_feedbackObservation.fallbackObserveOnly.load(std::memory_order_relaxed) << "\n";
	out << "fallback_no_previous_generation=" << s_feedbackObservation.fallbackNoPreviousGeneration.load(std::memory_order_relaxed) << "\n";
	out << "fallback_signature_mismatch=" << s_feedbackObservation.fallbackSignatureMismatch.load(std::memory_order_relaxed) << "\n";
	out << "fallback_generation_gap=" << s_feedbackObservation.fallbackGenerationGap.load(std::memory_order_relaxed) << "\n";
	out << "fallback_completion_unavailable=" << s_feedbackObservation.fallbackCompletionUnavailable.load(std::memory_order_relaxed) << "\n";
	out << "fallback_partial_publication=" << s_feedbackObservation.fallbackPartialPublication.load(std::memory_order_relaxed) << "\n";
	out << "jobs=" << s_feedbackObservation.lastJobs.load(std::memory_order_relaxed) << "\n";
	out << "completed_jobs=" << s_feedbackObservation.lastCompletedJobs.load(std::memory_order_relaxed) << "\n";
	out << "active_jobs=" << s_feedbackObservation.lastActiveJobs.load(std::memory_order_relaxed) << "\n";
	out << "bytes=" << s_feedbackObservation.lastBytes.load(std::memory_order_relaxed) << "\n";
	out << "jobs_64x8=" << s_feedbackObservation.last64x8Jobs.load(std::memory_order_relaxed) << "\n";
	out << "jobs_64x3=" << s_feedbackObservation.last64x3Jobs.load(std::memory_order_relaxed) << "\n";
	out << "jobs_64x1=" << s_feedbackObservation.last64x1Jobs.load(std::memory_order_relaxed) << "\n";
	out << "signature=0x" << std::hex << s_feedbackObservation.lastSignature.load(std::memory_order_relaxed) << std::dec << "\n";
	out << "signature_matched=" << (s_feedbackObservation.lastSignatureMatched.load(std::memory_order_relaxed) ? "true" : "false") << "\n";
	out << "completion_point=" << s_feedbackObservation.lastCompletionPoint.load(std::memory_order_relaxed) << "\n";
	out << "generation_scheduled=" << s_feedbackObservation.generationScheduled.load(std::memory_order_relaxed) << "\n";
	out << "generation_published=" << s_feedbackObservation.generationPublished.load(std::memory_order_relaxed) << "\n";
	out << "generation_consumed=" << s_feedbackObservation.generationConsumed.load(std::memory_order_relaxed) << "\n";
	out << "generation_held=" << s_feedbackObservation.generationHeld.load(std::memory_order_relaxed) << "\n";
	out << "generation_age=" << s_feedbackObservation.lastGenerationAge.load(std::memory_order_relaxed) << "\n";
	out << "held_jobs=" << s_feedbackObservation.heldJobs.load(std::memory_order_relaxed) << "\n";
	out << "fallback_reason=" << GetFeedbackFallbackReasonName(static_cast<FeedbackFallbackReason>(
		s_feedbackObservation.lastFallbackReason.load(std::memory_order_relaxed))) << "\n";
	return out.str();
}

void LatteTextureReadback_UpdateFinishedTransfers(bool forceFinish, LatteGuestFeedbackMode feedbackMode)
{
	const bool feedbackBoundary = feedbackMode != LatteGuestFeedbackMode::None;
	const bool allowFeedbackDeferral = feedbackMode == LatteGuestFeedbackMode::GuardedPreviousGeneration;
	uint64 feedbackGeneration = 0;
	bool deferCurrentFeedback = false;
	bool publishCurrentFeedbackWithoutWaiting = false;
	if (forceFinish)
	{
		// start any delayed transfers
		LatteTextureReadback_Update(true);

		// Vulkan readbacks share one ordered graphics queue. Waiting for the newest
		// command-buffer serial completes every earlier readback too, so publish the
		// whole visibility batch after one renderer completion wait. Backends that
		// cannot expose an ordered point keep the per-readback fallback below.
		auto readbacks = sTextureActiveReadbackQueue;
		LatteTextureReadbackInfo* latestReadback = nullptr;
		uint64 latestCompletionPoint = 0;
		bool orderedCompletionAvailable = true;
		uint32 unfinishedActiveJobs = 0;
		uint32 visibilityJobCount = 0;
		uint64 visibilityBytes = 0;
		std::vector<LatteTextureReadbackInfo*> activeReadbacks;
		FeedbackBatchStats activeFeedbackStats;
		FeedbackBatchStats completedFeedbackStats;
		if (feedbackBoundary)
		{
			feedbackGeneration = s_feedbackObservation.generationScheduled.fetch_add(1, std::memory_order_relaxed) + 1;
			s_feedbackObservation.boundaries.fetch_add(1, std::memory_order_relaxed);
			completedFeedbackStats = TakeCompletedFeedbackCandidates();
		}
		const HRTick waitStartTime = HighResolutionTimer().now().getTick();
		while (!readbacks.empty())
		{
			LatteTextureReadbackInfo* readbackInfo = readbacks.front();
			readbacks.pop();
			visibilityJobCount++;
			visibilityBytes += readbackInfo->GetImageSizeBytes();
			if (feedbackBoundary)
			{
				activeReadbacks.emplace_back(readbackInfo);
				readbackInfo->SetFeedbackGeneration(feedbackGeneration);
				readbackInfo->SetPublishesFeedbackGeneration(false);
				readbackInfo->SetFeedbackPublicationHeld(false);
				AddFeedbackJob(activeFeedbackStats, *readbackInfo);
			}
			readbackInfo->waitStartTime = waitStartTime;
			const uint64 completionPoint = readbackInfo->GetOrderedCompletionPoint();
			orderedCompletionAvailable &= completionPoint != 0;
			const bool finished = readbackInfo->IsFinished();
			if (!finished)
			{
				unfinishedActiveJobs++;
				if (completionPoint > latestCompletionPoint)
				{
					latestCompletionPoint = completionPoint;
					latestReadback = readbackInfo;
				}
			}
			else
				latestCompletionPoint = std::max(latestCompletionPoint, completionPoint);
		}
		if (feedbackBoundary && !activeReadbacks.empty())
			activeReadbacks.back()->SetPublishesFeedbackGeneration(true);

		SPATIAL_PROFILER_COUNTER_SET("cemu.readback.visibility_batch_jobs", visibilityJobCount,
			"Cemu Guest Visibility", "jobs");
		SPATIAL_PROFILER_COUNTER_SET("cemu.readback.visibility_batch_bytes", visibilityBytes,
			"Cemu Guest Visibility", "bytes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.readback.visibility_batch_completion_waits", latestReadback ? 1 : 0,
			"Cemu Guest Visibility", "waits");

		if (feedbackBoundary)
		{
			const uint32 completedJobs = completedFeedbackStats.jobs;
			// The active queue is the exact set whose Guest visibility this boundary
			// controls. Already-published jobs are diagnostics only when an active
			// batch exists; folding them in would make unrelated periodic feedback
			// completions cause a false fallback. If the active queue is empty, a
			// complete historical batch means the current data is already visible.
			FeedbackBatchStats feedbackStats = visibilityJobCount != 0
				? std::move(activeFeedbackStats)
				: std::move(completedFeedbackStats);
			const uint64 feedbackSignature = GetFeedbackBatchSignature(feedbackStats);
			const bool signatureMatched = IsExpectedFeedbackBatch(feedbackStats);
			s_feedbackObservation.lastJobs.store(feedbackStats.jobs, std::memory_order_relaxed);
			s_feedbackObservation.lastCompletedJobs.store(completedJobs, std::memory_order_relaxed);
			s_feedbackObservation.lastActiveJobs.store(visibilityJobCount, std::memory_order_relaxed);
			s_feedbackObservation.lastBytes.store(feedbackStats.bytes, std::memory_order_relaxed);
			s_feedbackObservation.last64x8Jobs.store(feedbackStats.jobs64x8, std::memory_order_relaxed);
			s_feedbackObservation.last64x3Jobs.store(feedbackStats.jobs64x3, std::memory_order_relaxed);
			s_feedbackObservation.last64x1Jobs.store(feedbackStats.jobs64x1, std::memory_order_relaxed);
			s_feedbackObservation.lastSignature.store(feedbackSignature, std::memory_order_relaxed);
			s_feedbackObservation.lastCompletionPoint.store(latestCompletionPoint, std::memory_order_relaxed);
			s_feedbackObservation.lastSignatureMatched.store(signatureMatched, std::memory_order_relaxed);
			if (signatureMatched)
				s_feedbackObservation.signatureMatches.fetch_add(1, std::memory_order_relaxed);
			else
				s_feedbackObservation.signatureMismatches.fetch_add(1, std::memory_order_relaxed);

			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.boundaries",
				s_feedbackObservation.boundaries.load(std::memory_order_relaxed), "Cemu Guest Feedback", "boundaries");
			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.jobs", feedbackStats.jobs, "Cemu Guest Feedback", "jobs");
			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.completed_jobs", completedJobs, "Cemu Guest Feedback", "jobs");
			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.active_jobs", visibilityJobCount, "Cemu Guest Feedback", "jobs");
			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.bytes", feedbackStats.bytes, "Cemu Guest Feedback", "bytes");
			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.signature", feedbackSignature, "Cemu Guest Feedback", "hash");
			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.signature_matched", signatureMatched ? 1 : 0, "Cemu Guest Feedback", "bool");
			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.generation_scheduled", feedbackGeneration, "Cemu Guest Feedback", "generation");

			const uint64 previousGeneration = s_feedbackObservation.generationPublished.load(std::memory_order_acquire);
			FeedbackFallbackReason fallbackReason = FeedbackFallbackReason::None;
			if (!allowFeedbackDeferral)
				fallbackReason = FeedbackFallbackReason::ObserveOnly;
			else if (!signatureMatched)
				fallbackReason = FeedbackFallbackReason::SignatureMismatch;
			else if (unfinishedActiveJobs == 0)
				publishCurrentFeedbackWithoutWaiting = true;
			else if (previousGeneration == 0)
				fallbackReason = FeedbackFallbackReason::NoPreviousGeneration;
			else if (previousGeneration + 1 != feedbackGeneration)
				fallbackReason = FeedbackFallbackReason::GenerationGap;
			else if (!orderedCompletionAvailable)
				fallbackReason = FeedbackFallbackReason::CompletionUnavailable;
			else
				deferCurrentFeedback = true;

			s_feedbackObservation.lastFallbackReason.store(static_cast<uint32>(fallbackReason), std::memory_order_relaxed);
			if (deferCurrentFeedback || publishCurrentFeedbackWithoutWaiting)
			{
				const uint64 fastPath = s_feedbackObservation.fastPath.fetch_add(1, std::memory_order_relaxed) + 1;
				if (deferCurrentFeedback)
					s_feedbackObservation.fastPathPreviousGeneration.fetch_add(1, std::memory_order_relaxed);
				else
					s_feedbackObservation.fastPathAlreadyVisible.fetch_add(1, std::memory_order_relaxed);
				SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.fast_path", fastPath,
					"Cemu Guest Feedback", "boundaries");
			}
			else if (allowFeedbackDeferral)
			{
				const uint64 fallback = s_feedbackObservation.fallback.fetch_add(1, std::memory_order_relaxed) + 1;
				if (fallbackReason == FeedbackFallbackReason::NoPreviousGeneration)
					s_feedbackObservation.fallbackNoPreviousGeneration.fetch_add(1, std::memory_order_relaxed);
				else if (fallbackReason == FeedbackFallbackReason::SignatureMismatch)
					s_feedbackObservation.fallbackSignatureMismatch.fetch_add(1, std::memory_order_relaxed);
				else if (fallbackReason == FeedbackFallbackReason::GenerationGap)
					s_feedbackObservation.fallbackGenerationGap.fetch_add(1, std::memory_order_relaxed);
				else if (fallbackReason == FeedbackFallbackReason::CompletionUnavailable)
					s_feedbackObservation.fallbackCompletionUnavailable.fetch_add(1, std::memory_order_relaxed);
				else if (fallbackReason == FeedbackFallbackReason::PartialPublication)
					s_feedbackObservation.fallbackPartialPublication.fetch_add(1, std::memory_order_relaxed);
				SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.fallback", fallback,
					"Cemu Guest Feedback", "boundaries");
				SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.fallback_reason", static_cast<uint32>(fallbackReason),
					"Cemu Guest Feedback", "enum");
			}
			else
			{
				s_feedbackObservation.fallbackObserveOnly.fetch_add(1, std::memory_order_relaxed);
			}
		}

		if (deferCurrentFeedback)
		{
			for (LatteTextureReadbackInfo* readbackInfo : activeReadbacks)
				readbackInfo->SetFeedbackPublicationHeld(true);
			s_feedbackObservation.generationHeld.store(feedbackGeneration, std::memory_order_release);
			s_feedbackObservation.heldJobs.store(static_cast<uint32>(activeReadbacks.size()), std::memory_order_relaxed);
			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.generation_held", feedbackGeneration,
				"Cemu Guest Feedback", "generation");
			SPATIAL_PROFILER_COUNTER_SET("cemu.feedback.held_jobs", activeReadbacks.size(),
				"Cemu Guest Feedback", "jobs");
			return;
		}

		if (latestReadback)
		{
			SPATIAL_PROFILER_AUTO_SCOPE_NAME("latte.completion.guest_memory_visibility.batch_wait");
			latestReadback->forceFinish = true;
			latestReadback->ForceFinish();
		}
	}
	performanceMonitor.gpuTime_waitForAsync.beginMeasuring();
	while (!sTextureActiveReadbackQueue.empty())
	{
		LatteTextureReadbackInfo* readbackInfo = sTextureActiveReadbackQueue.front();
		const uint64 heldGeneration = s_feedbackObservation.generationHeld.load(std::memory_order_acquire);
		const bool publicationHeld = readbackInfo->IsFeedbackPublicationHeld() && heldGeneration != 0 &&
			readbackInfo->GetFeedbackGeneration() == heldGeneration;
		if (!forceFinish && publicationHeld)
			break;
		if (readbackInfo->IsFeedbackPublicationHeld())
			readbackInfo->SetFeedbackPublicationHeld(false);
		if (forceFinish)
		{
			if (!readbackInfo->IsFinished())
			{
				readbackInfo->waitStartTime = HighResolutionTimer().now().getTick();
#ifdef LOG_READBACK_TIME
				if (cemuLog_isLoggingEnabled(LogType::TextureReadback))
				{
					double elapsedSecondsTransfer = HighResolutionTimer::getTimeDiff(readbackInfo->transferStartTime, HighResolutionTimer().now().getTick());
					cemuLog_log(LogType::TextureReadback, "[Texture-Readback] Force-finish: {:08x} Res {:}/{:} TM {:} FMT {:04x} Transfer time so far: {:.4}ms", readbackInfo->hostTextureCopy.physAddress, readbackInfo->hostTextureCopy.width, readbackInfo->hostTextureCopy.height, readbackInfo->hostTextureCopy.tileMode, (uint32)readbackInfo->hostTextureCopy.format, elapsedSecondsTransfer * 1000.0);
				}
#endif
				readbackInfo->forceFinish = true;
				readbackInfo->ForceFinish();
				// rerun logic since ->ForceFinish() can recurively call this function and thus modify the queue
				continue;
			}
		}
		else
		{
			if (!readbackInfo->IsFinished())
				break;
			readbackInfo->waitStartTime = HighResolutionTimer().now().getTick();
		}
		// performance testing
#ifdef LOG_READBACK_TIME
		if (cemuLog_isLoggingEnabled(LogType::TextureReadback))
		{
			HRTick currentTick = HighResolutionTimer().now().getTick();
			double elapsedSecondsTransfer = HighResolutionTimer::getTimeDiff(readbackInfo->transferStartTime, currentTick);
			double elapsedSecondsWaiting = HighResolutionTimer::getTimeDiff(readbackInfo->waitStartTime, currentTick);
			cemuLog_log(LogType::TextureReadback, "[Texture-Readback] {:08x} Res {}/{} TM {} FMT {:04x} ReadbackLatency: {:6.3}ms WaitTime: {:6.3}ms ForcedWait {}", readbackInfo->hostTextureCopy.physAddress, readbackInfo->hostTextureCopy.width, readbackInfo->hostTextureCopy.height, readbackInfo->hostTextureCopy.tileMode, (uint32)readbackInfo->hostTextureCopy.format, elapsedSecondsTransfer * 1000.0, elapsedSecondsWaiting * 1000.0, readbackInfo->forceFinish ? "yes" : "no");
		}
#endif
		uint8* pixelData = readbackInfo->GetData();
		LatteTextureLoader_writeReadbackTextureToMemory(&readbackInfo->hostTextureCopy, readbackInfo->firstSlice,
			readbackInfo->firstMip, pixelData);
		if (readbackInfo->GetFeedbackGeneration() == 0)
			RecordCompletedFeedbackCandidate(*readbackInfo);
		if (readbackInfo->PublishesFeedbackGeneration())
			PublishFeedbackGeneration(readbackInfo->GetFeedbackGeneration());
		readbackInfo->ReleaseData();
		// get the original texture if it still exists and invalidate the current data hash
		LatteTextureView* origTexView = LatteTextureViewLookupCache::lookupSlice(readbackInfo->hostTextureCopy.physAddress,
			readbackInfo->hostTextureCopy.width, readbackInfo->hostTextureCopy.height, readbackInfo->hostTextureCopy.pitch,
			readbackInfo->firstMip, readbackInfo->firstSlice, readbackInfo->hostTextureCopy.format);
		if (origTexView)
		{
			LatteTexture_TrackGuestReadback(origTexView->baseTexture,
				{readbackInfo->firstMip, 1, readbackInfo->firstSlice, 1});
			LatteTC_ResetTextureChangeTracker(origTexView->baseTexture, true);
		}
		delete readbackInfo;
		// remove from queue
		cemu_assert_debug(!sTextureActiveReadbackQueue.empty());
		cemu_assert_debug(readbackInfo == sTextureActiveReadbackQueue.front());
		sTextureActiveReadbackQueue.pop();
	}
	performanceMonitor.gpuTime_waitForAsync.endMeasuring();
	if (feedbackBoundary && !deferCurrentFeedback)
	{
		// A full visibility boundary also completes an empty generation. This keeps
		// the next guarded boundary recoverable after loading or a signature change.
		PublishFeedbackGeneration(feedbackGeneration);
	}
}

bool LatteTextureReadback_ReadbackToLinearBlocking(LatteTextureView* sourceView, uint8* dstPtr, uint32 dstWidth, uint32 dstHeight, uint32 dstPitch)
{
	if (!sourceView || !sourceView->baseTexture || !dstPtr)
		return false;
	LatteTexture* texture = sourceView->baseTexture;
	if (Latte::IsCompressedFormat(texture->format))
		return false;
	const uint32 formatBits = Latte::GetFormatBits(texture->format);
	if ((formatBits % 8) != 0)
		return false;
	const uint32 bytesPerPixel = formatBits / 8;
	const auto sourceExtent = texture->GetGuestExtent(static_cast<uint32>(sourceView->firstMip));
	const auto copyLayout = LatteSurfaceResolveLinearCopyLayout(sourceExtent.width, sourceExtent.height,
		dstWidth, dstHeight, dstPitch, bytesPerPixel);
	if (!copyLayout.valid)
		return false;
	const auto representation = texture->RepresentationsAlias() ? LatteTextureRepresentation::Render : LatteTextureRepresentation::GuestNative;
	const LatteSurfaceSubresourceRange range{static_cast<uint32>(sourceView->firstMip), 1,
		static_cast<uint32>(sourceView->firstSlice), 1};
	const auto syncResult = LatteTexture_EnsureRepresentationCurrent(texture, representation, range);
	if (!syncResult.succeeded)
	{
		SurfaceResolutionDiagnostics::RecordReadback(*texture, false);
		return false;
	}
	LatteTextureReadbackInfo* info = g_renderer->texture_createReadback(sourceView, representation);
	SurfaceResolutionDiagnostics::RecordReadback(*sourceView->baseTexture, info != nullptr);
	if (!info)
		return false;

	info->StartTransfer();
	info->ForceFinish();
	cemu_assert(info->IsFinished());

	uint8* data = info->GetData(); // returned pixel format should match Latte format
	for (uint32 y = 0; y < dstHeight; y++)
	{
		memcpy(dstPtr + static_cast<size_t>(y) * copyLayout.destinationStrideBytes,
			data + static_cast<size_t>(y) * copyLayout.sourceRowBytes,
			static_cast<size_t>(copyLayout.copyRowBytes));
	}

	info->ReleaseData();
	delete info;
	LatteTexture_TrackGuestReadback(texture, range);
	return true;
}
