#include "Cafe/HW/Latte/Core/LatteSurfaceScaleState.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace
{
	std::atomic<uint32> s_renderSurfaceScaleActivePercent{100};
	std::atomic<uint32> s_staticTextureScaleActiveFactor{1};
	std::atomic<uint32> s_surfaceScaleGeneration{};
	std::atomic<bool> s_surfaceScaleTitleActive{};
	std::atomic<uint64> s_surfaceScaleReservedAdditionalBytes{};
	std::mutex s_surfaceScaleForcedNativeMutex;
	std::unordered_map<uint32, LatteSurfaceFallbackReason> s_surfaceScaleForcedNativeAddresses;

	void ClearForcedNativeAddresses()
	{
		std::scoped_lock lock{s_surfaceScaleForcedNativeMutex};
		s_surfaceScaleForcedNativeAddresses.clear();
	}
}

void LatteSurfaceScaleState::BeginTitle(uint32 configuredRenderScalePercent,
	uint32 configuredStaticTextureScaleFactor)
{
	s_renderSurfaceScaleActivePercent.store(
		LatteSurfaceNormalizeRenderScalePercent(configuredRenderScalePercent),
		std::memory_order_relaxed);
	s_staticTextureScaleActiveFactor.store(
		LatteSurfaceNormalizeStaticTextureScaleFactor(configuredStaticTextureScaleFactor),
		std::memory_order_relaxed);
	s_surfaceScaleGeneration.fetch_add(1, std::memory_order_relaxed);
	s_surfaceScaleReservedAdditionalBytes.store(0, std::memory_order_relaxed);
	ClearForcedNativeAddresses();
	s_surfaceScaleTitleActive.store(true, std::memory_order_release);
}

void LatteSurfaceScaleState::EndTitle()
{
	s_surfaceScaleTitleActive.store(false, std::memory_order_release);
	s_renderSurfaceScaleActivePercent.store(100, std::memory_order_relaxed);
	s_staticTextureScaleActiveFactor.store(1, std::memory_order_relaxed);
	s_surfaceScaleReservedAdditionalBytes.store(0, std::memory_order_relaxed);
	ClearForcedNativeAddresses();
}

LatteSurfaceScaleRuntimeSnapshot LatteSurfaceScaleState::GetSnapshot(
	uint32 configuredRenderScalePercent, uint32 configuredStaticTextureScaleFactor)
{
	const bool titleActive = s_surfaceScaleTitleActive.load(std::memory_order_acquire);
	return ResolveSnapshot(configuredRenderScalePercent,
		s_renderSurfaceScaleActivePercent.load(std::memory_order_relaxed),
		configuredStaticTextureScaleFactor,
		s_staticTextureScaleActiveFactor.load(std::memory_order_relaxed),
		s_surfaceScaleGeneration.load(std::memory_order_relaxed), titleActive);
}

uint32 LatteSurfaceScaleState::GetActiveRenderScalePercent()
{
	return s_surfaceScaleTitleActive.load(std::memory_order_acquire) ?
		LatteSurfaceNormalizeRenderScalePercent(
			s_renderSurfaceScaleActivePercent.load(std::memory_order_relaxed)) : 100;
}

uint32 LatteSurfaceScaleState::GetActiveStaticTextureScaleFactor()
{
	return s_surfaceScaleTitleActive.load(std::memory_order_acquire) ?
		LatteSurfaceNormalizeStaticTextureScaleFactor(
			s_staticTextureScaleActiveFactor.load(std::memory_order_relaxed)) : 1;
}

uint32 LatteSurfaceScaleState::GetGeneration()
{
	return s_surfaceScaleGeneration.load(std::memory_order_relaxed);
}

void LatteSurfaceScaleState::ForceNativeAddress(uint32 guestAddress,
	LatteSurfaceFallbackReason reason)
{
	if (guestAddress == 0 || reason == LatteSurfaceFallbackReason::None)
		return;
	std::scoped_lock lock{s_surfaceScaleForcedNativeMutex};
	s_surfaceScaleForcedNativeAddresses.try_emplace(guestAddress, reason);
}

LatteSurfaceFallbackReason LatteSurfaceScaleState::GetForcedNativeReason(uint32 guestAddress)
{
	std::scoped_lock lock{s_surfaceScaleForcedNativeMutex};
	const auto it = s_surfaceScaleForcedNativeAddresses.find(guestAddress);
	return it == s_surfaceScaleForcedNativeAddresses.end() ? LatteSurfaceFallbackReason::None : it->second;
}

bool LatteSurfaceScaleState::TryReserveAdditionalBytes(uint64 bytes, uint64 budgetBytes,
	uint64 availableBytes)
{
	if (bytes == 0)
		return true;
	if (bytes > availableBytes || bytes > budgetBytes)
		return false;
	uint64 reserved = s_surfaceScaleReservedAdditionalBytes.load(std::memory_order_relaxed);
	while (reserved <= budgetBytes - bytes)
	{
		if (s_surfaceScaleReservedAdditionalBytes.compare_exchange_weak(reserved, reserved + bytes,
			std::memory_order_relaxed))
		{
			return true;
		}
	}
	return false;
}

void LatteSurfaceScaleState::ReleaseAdditionalBytes(uint64 bytes)
{
	if (bytes == 0)
		return;
	uint64 reserved = s_surfaceScaleReservedAdditionalBytes.load(std::memory_order_relaxed);
	while (true)
	{
		const uint64 next = bytes > reserved ? 0 : reserved - bytes;
		if (s_surfaceScaleReservedAdditionalBytes.compare_exchange_weak(reserved, next,
			std::memory_order_relaxed))
		{
			return;
		}
	}
}

uint64 LatteSurfaceScaleState::GetReservedAdditionalBytes()
{
	return s_surfaceScaleReservedAdditionalBytes.load(std::memory_order_relaxed);
}
