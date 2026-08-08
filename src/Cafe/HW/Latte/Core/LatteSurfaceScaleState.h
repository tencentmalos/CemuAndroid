#pragma once

#include "Cafe/HW/Latte/Core/LatteSurfaceScale.h"

#include <cstdint>

struct LatteSurfaceScaleRuntimeSnapshot
{
	uint32 configuredRenderScalePercent{100};
	uint32 activeRenderScalePercent{100};
	uint32 configuredStaticTextureScaleFactor{1};
	uint32 activeStaticTextureScaleFactor{1};
	uint32 generation{};
	bool titleActive{};
	bool renderPendingRestart{};
	bool staticTexturePendingRestart{};
	bool pendingRestart{};
};

namespace LatteSurfaceScaleState
{
	constexpr LatteSurfaceScaleRuntimeSnapshot ResolveSnapshot(uint32 configuredRenderScalePercent,
		uint32 activeRenderScalePercent, uint32 configuredStaticTextureScaleFactor,
		uint32 activeStaticTextureScaleFactor, uint32 generation, bool titleActive)
	{
		LatteSurfaceScaleRuntimeSnapshot result;
		result.configuredRenderScalePercent =
			LatteSurfaceNormalizeRenderScalePercent(configuredRenderScalePercent);
		result.activeRenderScalePercent = titleActive ?
			LatteSurfaceNormalizeRenderScalePercent(activeRenderScalePercent) : 100;
		result.configuredStaticTextureScaleFactor =
			LatteSurfaceNormalizeStaticTextureScaleFactor(configuredStaticTextureScaleFactor);
		result.activeStaticTextureScaleFactor = titleActive ?
			LatteSurfaceNormalizeStaticTextureScaleFactor(activeStaticTextureScaleFactor) : 1;
		result.generation = generation;
		result.titleActive = titleActive;
		result.renderPendingRestart = titleActive &&
			result.configuredRenderScalePercent != result.activeRenderScalePercent;
		result.staticTexturePendingRestart = titleActive &&
			result.configuredStaticTextureScaleFactor != result.activeStaticTextureScaleFactor;
		result.pendingRestart = result.renderPendingRestart || result.staticTexturePendingRestart;
		return result;
	}

	void BeginTitle(uint32 configuredRenderScalePercent, uint32 configuredStaticTextureScaleFactor);
	void EndTitle();
	LatteSurfaceScaleRuntimeSnapshot GetSnapshot(uint32 configuredRenderScalePercent,
		uint32 configuredStaticTextureScaleFactor);
	uint32 GetActiveRenderScalePercent();
	uint32 GetActiveStaticTextureScaleFactor();
	uint32 GetGeneration();
	void ForceNativeAddress(uint32 guestAddress, LatteSurfaceFallbackReason reason);
	LatteSurfaceFallbackReason GetForcedNativeReason(uint32 guestAddress);
	bool TryReserveAdditionalBytes(uint64 bytes, uint64 budgetBytes, uint64 availableBytes);
	void ReleaseAdditionalBytes(uint64 bytes);
	uint64 GetReservedAdditionalBytes();
}

static_assert(!LatteSurfaceScaleState::ResolveSnapshot(200, 100, 2, 1, 3, false).pendingRestart);
static_assert(LatteSurfaceScaleState::ResolveSnapshot(200, 100, 1, 1, 3, true).renderPendingRestart);
static_assert(LatteSurfaceScaleState::ResolveSnapshot(100, 100, 2, 1, 3, true).staticTexturePendingRestart);
static_assert(!LatteSurfaceScaleState::ResolveSnapshot(50, 50, 2, 2, 3, true).pendingRestart);
