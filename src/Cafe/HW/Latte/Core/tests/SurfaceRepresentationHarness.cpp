#include "Cafe/HW/Latte/Core/LatteSurfaceScale.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceScaleState.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
	[[noreturn]] void Fail(std::string_view expression, uint32 line)
	{
		std::cerr << "Surface representation harness failed at line " << line << ": " << expression << '\n';
		std::exit(1);
	}

#define REQUIRE(expression) do { if (!(expression)) Fail(#expression, __LINE__); } while (false)

	enum class BackendKind
	{
		Vulkan,
		Metal,
	};

	struct RepresentationBackendFixture
	{
		BackendKind kind{};
		bool representationsAlias{};
		bool failAllocation{};
		bool failResample{};
		bool companionCreated{};
		uint64 companionBytes{};
		uint32 createCount{};
		uint32 releaseCount{};
		uint32 syncCount{};

		LatteSurfaceOperationResult EnsureGuestNative()
		{
			if (representationsAlias)
				return LatteSurfaceOperationResult::Success();
			if (companionCreated)
				return LatteSurfaceOperationResult::Success(companionBytes);
			if (failAllocation)
				return LatteSurfaceOperationResult::Failure(LatteSurfaceFallbackReason::AllocationFailed);
			companionCreated = true;
			companionBytes = kind == BackendKind::Vulkan ? 0x200000 : 0x210000;
			createCount++;
			return LatteSurfaceOperationResult::Success(companionBytes);
		}

		LatteSurfaceOperationResult Resample()
		{
			if (failResample)
				return LatteSurfaceOperationResult::Failure(LatteSurfaceFallbackReason::BackendOperationUnsupported);
			syncCount++;
			return LatteSurfaceOperationResult::Success();
		}

		void Release()
		{
			if (!companionCreated)
				return;
			companionCreated = false;
			companionBytes = 0;
			releaseCount++;
		}
	};

	template<typename T>
	struct Plane
	{
		uint32 width{};
		uint32 height{};
		uint32 rowPitch{};
		std::vector<T> data;

		Plane(uint32 width, uint32 height, uint32 rowPitch, T initial = {})
			: width(width), height(height), rowPitch(rowPitch), data(static_cast<size_t>(rowPitch) * height, initial)
		{
			REQUIRE(rowPitch >= width);
		}

		T& At(uint32 x, uint32 y) { return data[static_cast<size_t>(y) * rowPitch + x]; }
		const T& At(uint32 x, uint32 y) const { return data[static_cast<size_t>(y) * rowPitch + x]; }
	};

	template<typename T>
	void ResampleNearest(const Plane<T>& source, Plane<T>& destination)
	{
		for (uint32 y = 0; y < destination.height; ++y)
		{
			const uint32 sourceY = std::min(source.height - 1,
				static_cast<uint32>((static_cast<uint64>(y) * 2 + 1) * source.height / (static_cast<uint64>(destination.height) * 2)));
			for (uint32 x = 0; x < destination.width; ++x)
			{
				const uint32 sourceX = std::min(source.width - 1,
					static_cast<uint32>((static_cast<uint64>(x) * 2 + 1) * source.width / (static_cast<uint64>(destination.width) * 2)));
				destination.At(x, y) = source.At(sourceX, sourceY);
			}
		}
	}

	template<typename T>
	void CopyRect(const Plane<T>& source, Plane<T>& destination, uint32 sourceX, uint32 sourceY,
		uint32 destinationX, uint32 destinationY, uint32 width, uint32 height)
	{
		REQUIRE(sourceX + width <= source.width);
		REQUIRE(sourceY + height <= source.height);
		REQUIRE(destinationX + width <= destination.width);
		REQUIRE(destinationY + height <= destination.height);
		for (uint32 y = 0; y < height; ++y)
		{
			for (uint32 x = 0; x < width; ++x)
				destination.At(destinationX + x, destinationY + y) = source.At(sourceX + x, sourceY + y);
		}
	}

	void ResampleLinear(const Plane<float>& source, Plane<float>& destination)
	{
		for (uint32 y = 0; y < destination.height; ++y)
		{
			const float sourceY = (static_cast<float>(y) + 0.5f) * source.height / destination.height - 0.5f;
			const uint32 y0 = static_cast<uint32>(std::max(0.0f, std::floor(sourceY)));
			const uint32 y1 = std::min(source.height - 1, y0 + 1);
			const float fy = std::clamp(sourceY - std::floor(sourceY), 0.0f, 1.0f);
			for (uint32 x = 0; x < destination.width; ++x)
			{
				const float sourceX = (static_cast<float>(x) + 0.5f) * source.width / destination.width - 0.5f;
				const uint32 x0 = static_cast<uint32>(std::max(0.0f, std::floor(sourceX)));
				const uint32 x1 = std::min(source.width - 1, x0 + 1);
				const float fx = std::clamp(sourceX - std::floor(sourceX), 0.0f, 1.0f);
				const float top = source.At(x0, y0) * (1.0f - fx) + source.At(x1, y0) * fx;
				const float bottom = source.At(x0, y1) * (1.0f - fx) + source.At(x1, y1) * fx;
				destination.At(x, y) = top * (1.0f - fy) + bottom * fy;
			}
		}
	}

	void TestContentSerials()
	{
		LatteSurfaceContentSerials serials;
		serials.MarkGuestWrite(10);
		REQUIRE(!LatteSurfacePlanRepresentationSync(serials, LatteTextureRepresentation::GuestNative, false).available);

		serials.MarkGuestUpload(10, false);
		auto upscale = LatteSurfacePlanRepresentationSync(serials, LatteTextureRepresentation::Render, false);
		REQUIRE(upscale.available && upscale.requiresBackendOperation);
		const auto beforeFailure = serials;
		const bool injectedBackendSuccess = false;
		if (injectedBackendSuccess)
			serials.MarkSynchronized(upscale.source, upscale.destination);
		REQUIRE(serials.guestRam == beforeFailure.guestRam && serials.guestNativeImage == beforeFailure.guestNativeImage &&
			serials.renderImage == beforeFailure.renderImage);

		serials.MarkSynchronized(upscale.source, upscale.destination);
		serials.MarkRenderWrite(11, false);
		auto downscale = LatteSurfacePlanRepresentationSync(serials, LatteTextureRepresentation::GuestNative, false);
		REQUIRE(downscale.available && downscale.requiresBackendOperation && downscale.source == LatteTextureRepresentation::Render);
		serials.MarkSynchronized(downscale.source, downscale.destination);
		serials.MarkGuestReadback(LatteTextureRepresentation::GuestNative);
		REQUIRE(serials.guestRam == 11 && serials.guestNativeImage == 11 && serials.renderImage == 11);

		LatteSurfaceContentSerials aliasSerials;
		aliasSerials.MarkGuestUpload(20, true);
		REQUIRE(aliasSerials.guestNativeImage == aliasSerials.renderImage);
		REQUIRE(!LatteSurfacePlanRepresentationSync(aliasSerials, LatteTextureRepresentation::Render, true).requiresBackendOperation);

		LatteSurfaceContentSerials directScaledUpload;
		directScaledUpload.MarkGuestDirectRenderUpload(21);
		REQUIRE(directScaledUpload.guestRam == 21 && directScaledUpload.guestNativeImage == 0 &&
			directScaledUpload.renderImage == 21);
		const auto directReadback = LatteSurfacePlanRepresentationSync(directScaledUpload,
			LatteTextureRepresentation::GuestNative, false);
		REQUIRE(directReadback.available && directReadback.requiresBackendOperation &&
			directReadback.source == LatteTextureRepresentation::Render);
	}

	void TestSubresourceSerialIsolation()
	{
		std::array<LatteSurfaceContentSerials, 4> subresources{};
		auto& mip0Slice0 = subresources[0];
		auto& mip1Slice1 = subresources[3];
		mip0Slice0.MarkGuestUpload(30, false);
		mip0Slice0.MarkSynchronized(LatteTextureRepresentation::GuestNative,
			LatteTextureRepresentation::Render);
		mip1Slice1.MarkGuestUpload(31, false);
		mip1Slice1.MarkSynchronized(LatteTextureRepresentation::GuestNative,
			LatteTextureRepresentation::Render);
		mip1Slice1.MarkRenderWrite(32, false);

		REQUIRE(mip0Slice0.guestRam == 30);
		REQUIRE(mip0Slice0.guestNativeImage == 30);
		REQUIRE(mip0Slice0.renderImage == 30);
		REQUIRE(mip1Slice1.guestRam == 31);
		REQUIRE(mip1Slice1.guestNativeImage == 31);
		REQUIRE(mip1Slice1.renderImage == 32);
		REQUIRE(subresources[1].Latest() == 0 && subresources[2].Latest() == 0);

		LatteSurfaceContentSerials compatibleAlias = mip1Slice1;
		REQUIRE(compatibleAlias.guestRam == 31);
		REQUIRE(compatibleAlias.guestNativeImage == 31);
		REQUIRE(compatibleAlias.renderImage == 32);
		REQUIRE(LatteSurfaceSelectCopyPath(true, false, false) == LatteSurfaceCopyPath::DirectRender);
		REQUIRE(LatteSurfaceSelectCopyPath(false, false, false) == LatteSurfaceCopyPath::NativeBoundary);
		REQUIRE(LatteSurfaceSelectCopyPath(true, true, false) == LatteSurfaceCopyPath::NativeBoundary);
		REQUIRE(LatteSurfaceSelectCopyPath(true, false, true) == LatteSurfaceCopyPath::NativeBoundary);
	}

	void TestBackendFailureFallbackAndCompanionLifetime()
	{
		for (const auto kind : {BackendKind::Vulkan, BackendKind::Metal})
		{
			RepresentationBackendFixture successBackend{kind, false};
			REQUIRE(successBackend.EnsureGuestNative().succeeded);
			REQUIRE(successBackend.Resample().succeeded && successBackend.syncCount == 1);
			REQUIRE(successBackend.companionCreated && successBackend.companionBytes != 0);
			successBackend.Release();
			REQUIRE(successBackend.createCount == 1 && successBackend.releaseCount == 1);

			for (const bool failAllocation : {false, true})
			{
				RepresentationBackendFixture backend{kind, false, failAllocation, !failAllocation};
				auto resolution = LatteSurfaceResolutionPolicy::Resolve({{1280, 720, 1}, true, {2560, 1440, 1}, false});
				auto result = backend.EnsureGuestNative();
				if (result.succeeded)
					result = backend.Resample();
				if (!result.succeeded)
					resolution = LatteSurfaceResolutionPolicy::ApplyBackendFallback(resolution, result.reason);
				REQUIRE(resolution.hostExtent.width == 1280 && resolution.hostExtent.height == 720);
				REQUIRE(resolution.source == LatteSurfaceScaleSource::SafetyFallback);
				REQUIRE(resolution.activeScalePercent == 100);
				REQUIRE(resolution.requestedHostExtent.width == 2560 && resolution.requestedHostExtent.height == 1440);
				REQUIRE(resolution.fallbackReason == (failAllocation ? LatteSurfaceFallbackReason::AllocationFailed :
					LatteSurfaceFallbackReason::BackendOperationUnsupported));
				backend.Release();
				REQUIRE(backend.companionBytes == 0);
				REQUIRE(backend.createCount == backend.releaseCount);
			}

			RepresentationBackendFixture nativeBackend{kind, true};
			const auto nativeResult = nativeBackend.EnsureGuestNative();
			REQUIRE(nativeResult.succeeded && !nativeBackend.companionCreated && nativeBackend.createCount == 0);
		}
	}

	void TestP3ResolutionPolicy()
	{
		const auto scalable = LatteSurfaceClassify({LatteSurfaceUsage::ColorAttachment});
		REQUIRE(scalable.scaleClass == LatteSurfaceScaleClass::ScalableRenderFamily);
		REQUIRE(scalable.reason == LatteSurfaceFallbackReason::None);

		const auto scaled = LatteSurfaceResolutionPolicy::Resolve({
			{1280, 720, 6}, false, {}, false, 200, 200, 1, 1, 17,
			scalable.scaleClass, scalable.reason});
		REQUIRE(scaled.guestExtent.width == 1280 && scaled.guestExtent.height == 720);
		REQUIRE(scaled.requestedHostExtent.width == 2560 && scaled.requestedHostExtent.height == 1440);
		REQUIRE(scaled.hostExtent.width == 2560 && scaled.hostExtent.height == 1440);
		REQUIRE(scaled.guestExtent.depth == 6 && scaled.hostExtent.depth == 6);
		REQUIRE(scaled.configuredScalePercent == 200 && scaled.activeScalePercent == 200 &&
			scaled.scaleGeneration == 17);
		REQUIRE(scaled.requestedSource == LatteSurfaceScaleSource::RenderSurfaceScale);
		REQUIRE(scaled.source == LatteSurfaceScaleSource::RenderSurfaceScale);

		const auto halfResolution = LatteSurfaceResolutionPolicy::Resolve({
			{1280, 720, 1}, false, {}, false, 50, 50, 1, 1, 17,
			scalable.scaleClass, scalable.reason});
		REQUIRE(halfResolution.hostExtent.width == 640 && halfResolution.hostExtent.height == 360);
		REQUIRE(halfResolution.source == LatteSurfaceScaleSource::RenderSurfaceScale);

		const auto staticTexture = LatteSurfaceClassify({LatteSurfaceUsage::Sampled});
		REQUIRE(staticTexture.scaleClass == LatteSurfaceScaleClass::Unknown);
		REQUIRE(staticTexture.reason == LatteSurfaceFallbackReason::StaticSampledTexture);
		const auto nativeStaticTexture = LatteSurfaceResolutionPolicy::Resolve({
			{1024, 1024, 1}, false, {}, false, 100, 100, 1, 1, 17,
			staticTexture.scaleClass, staticTexture.reason});
		REQUIRE(nativeStaticTexture.hostExtent.width == 1024 && nativeStaticTexture.hostExtent.height == 1024);
		REQUIRE(nativeStaticTexture.requestedHostExtent.width == 1024 &&
			nativeStaticTexture.requestedHostExtent.height == 1024);
		REQUIRE(nativeStaticTexture.source == LatteSurfaceScaleSource::Native);
		const auto scaledStaticTexture = LatteSurfaceResolutionPolicy::Resolve({
			{1024, 1024, 1}, false, {}, false, 50, 50, 2, 2, 17,
			staticTexture.scaleClass, staticTexture.reason});
		REQUIRE(scaledStaticTexture.hostExtent.width == 2048 &&
			scaledStaticTexture.hostExtent.height == 2048);
		REQUIRE(scaledStaticTexture.source == LatteSurfaceScaleSource::StaticTextureScale);

		const auto graphicPackFixed = LatteSurfaceResolutionPolicy::Resolve({
			{1280, 720, 1}, true, {1920, 1080, 1}, false, 200, 200, 2, 2, 18,
			scalable.scaleClass, scalable.reason});
		REQUIRE(graphicPackFixed.hostExtent.width == 1920 && graphicPackFixed.hostExtent.height == 1080);
		REQUIRE(graphicPackFixed.source == LatteSurfaceScaleSource::GraphicPackFixed);

		REQUIRE(LatteSurfaceClassify({LatteSurfaceUsage::ColorAttachment, true}).reason ==
			LatteSurfaceFallbackReason::CompressedFormat);
		REQUIRE(LatteSurfaceClassify({LatteSurfaceUsage::ColorAttachment, false, true}).reason ==
			LatteSurfaceFallbackReason::LinearLayout);
		REQUIRE(LatteSurfaceClassify({LatteSurfaceUsage::ColorAttachment, false, false, true}).reason ==
			LatteSurfaceFallbackReason::MsaaUnsupported);
		REQUIRE(LatteSurfaceClassify({LatteSurfaceUsage::ColorAttachment, false, false, false, false}).reason ==
			LatteSurfaceFallbackReason::DimensionUnsupported);

		const auto fallback = LatteSurfaceResolutionPolicy::ApplyBackendFallback(scaled,
			LatteSurfaceFallbackReason::FormatUnsupported);
		REQUIRE(fallback.hostExtent.width == 1280 && fallback.hostExtent.height == 720);
		REQUIRE(fallback.requestedHostExtent.width == 2560 && fallback.requestedHostExtent.height == 1440);
		REQUIRE(fallback.activeScalePercent == 200 && fallback.configuredScalePercent == 200);
		REQUIRE(fallback.source == LatteSurfaceScaleSource::SafetyFallback);
		REQUIRE(fallback.fallbackReason == LatteSurfaceFallbackReason::FormatUnsupported);
		REQUIRE(fallback.scaleClass == LatteSurfaceScaleClass::ForceNative);

		const auto pending = LatteSurfaceScaleState::ResolveSnapshot(200, 100, 2, 1, 19, true);
		REQUIRE(pending.configuredRenderScalePercent == 200 &&
			pending.activeRenderScalePercent == 100 && pending.renderPendingRestart &&
			pending.staticTexturePendingRestart);
		const auto stopped = LatteSurfaceScaleState::ResolveSnapshot(50, 50, 2, 2, 19, false);
		REQUIRE(stopped.activeRenderScalePercent == 100 &&
			stopped.activeStaticTextureScaleFactor == 1 && !stopped.pendingRestart);

		LatteSurfaceScaleState::BeginTitle(200, 2);
		LatteSurfaceScaleState::ForceNativeAddress(0x12340000,
			LatteSurfaceFallbackReason::FormatUnsupported);
		LatteSurfaceScaleState::ForceNativeAddress(0x12340000,
			LatteSurfaceFallbackReason::AllocationFailed);
		REQUIRE(LatteSurfaceScaleState::GetForcedNativeReason(0x12340000) ==
			LatteSurfaceFallbackReason::FormatUnsupported);
		LatteSurfaceScaleState::EndTitle();
		REQUIRE(LatteSurfaceScaleState::GetForcedNativeReason(0x12340000) ==
			LatteSurfaceFallbackReason::None);
	}

	void TestColorRoundTrip()
	{
		Plane<float> guest(2, 2, 4, -99.0f);
		guest.At(0, 0) = 1.0f;
		guest.At(1, 0) = 2.0f;
		guest.At(0, 1) = 3.0f;
		guest.At(1, 1) = 4.0f;
		Plane<float> render(4, 4, 6, -77.0f);
		ResampleNearest(guest, render);
		REQUIRE(render.At(0, 0) == 1.0f && render.At(1, 0) == 1.0f && render.At(2, 0) == 2.0f);
		REQUIRE(render.At(0, 2) == 3.0f && render.At(3, 3) == 4.0f);
		for (uint32 y = 0; y < render.height; ++y)
			REQUIRE(render.data[static_cast<size_t>(y) * render.rowPitch + 4] == -77.0f);

		Plane<float> readback(2, 2, 5, -55.0f);
		ResampleLinear(render, readback);
		REQUIRE(readback.At(0, 0) == 1.0f && readback.At(1, 0) == 2.0f && readback.At(0, 1) == 3.0f && readback.At(1, 1) == 4.0f);
		for (uint32 y = 0; y < readback.height; ++y)
			for (uint32 x = readback.width; x < readback.rowPitch; ++x)
				REQUIRE(readback.data[static_cast<size_t>(y) * readback.rowPitch + x] == -55.0f);
	}

	void TestLinearDownsample()
	{
		Plane<float> render(4, 4, 4);
		for (uint32 y = 0; y < render.height; ++y)
			for (uint32 x = 0; x < render.width; ++x)
				render.At(x, y) = static_cast<float>(y * 4 + x);
		Plane<float> guest(2, 2, 2);
		ResampleLinear(render, guest);
		REQUIRE(std::abs(guest.At(0, 0) - 2.5f) < 0.001f);
		REQUIRE(std::abs(guest.At(1, 0) - 4.5f) < 0.001f);
		REQUIRE(std::abs(guest.At(0, 1) - 10.5f) < 0.001f);
		REQUIRE(std::abs(guest.At(1, 1) - 12.5f) < 0.001f);
	}

	void TestDepthMipAndArraySlices()
	{
		for (uint32 slice = 0; slice < 2; ++slice)
		{
			for (uint32 mip = 0; mip < 2; ++mip)
			{
				const uint32 guestSize = mip == 0 ? 2 : 1;
				Plane<uint32> guest(guestSize, guestSize, guestSize, 0);
				for (uint32 y = 0; y < guest.height; ++y)
					for (uint32 x = 0; x < guest.width; ++x)
						guest.At(x, y) = 0x10000000u * (slice + 1) + 0x10000u * mip + y * guest.width + x;
				Plane<uint32> render(guestSize * 2, guestSize * 2, guestSize * 2, 0xFFFFFFFFu);
				ResampleNearest(guest, render);
				Plane<uint32> readback(guestSize, guestSize, guestSize, 0);
				ResampleNearest(render, readback);
				for (uint32 y = 0; y < guest.height; ++y)
					for (uint32 x = 0; x < guest.width; ++x)
						REQUIRE(readback.At(x, y) == guest.At(x, y));
			}
		}
	}

	void TestGpuCpuGpuRoundTripAndRowPitch()
	{
		Plane<float> guest(3, 2, 5, -10.0f);
		for (uint32 y = 0; y < guest.height; ++y)
		{
			for (uint32 x = 0; x < guest.width; ++x)
				guest.At(x, y) = static_cast<float>(10 + y * guest.width + x);
		}

		Plane<float> firstRender(6, 4, 8, -20.0f);
		ResampleNearest(guest, firstRender);
		Plane<float> guestNativeReadback(3, 2, 3, -30.0f);
		ResampleLinear(firstRender, guestNativeReadback);

		const auto layout = LatteSurfaceResolveLinearCopyLayout(guestNativeReadback.width,
			guestNativeReadback.height, 3, 2, 5, sizeof(float));
		REQUIRE(layout.valid);
		REQUIRE(layout.sourceRowBytes == 12);
		REQUIRE(layout.copyRowBytes == 12);
		REQUIRE(layout.destinationStrideBytes == 20);
		REQUIRE(!LatteSurfaceResolveLinearCopyLayout(3, 2, 4, 2, 5, sizeof(float)).valid);
		REQUIRE(!LatteSurfaceResolveLinearCopyLayout(3, 2, 3, 2, 2, sizeof(float)).valid);

		std::vector<float> cpuReadback(12, -40.0f);
		for (uint32 y = 0; y < guestNativeReadback.height; ++y)
		{
			std::memcpy(reinterpret_cast<uint8*>(cpuReadback.data()) + y * layout.destinationStrideBytes,
				reinterpret_cast<const uint8*>(guestNativeReadback.data.data()) + y * layout.sourceRowBytes,
				layout.copyRowBytes);
		}
		REQUIRE(cpuReadback[3] == -40.0f && cpuReadback[4] == -40.0f);
		REQUIRE(cpuReadback[8] == -40.0f && cpuReadback[9] == -40.0f);

		Plane<float> uploadedNative(3, 2, 3);
		for (uint32 y = 0; y < uploadedNative.height; ++y)
		{
			for (uint32 x = 0; x < uploadedNative.width; ++x)
				uploadedNative.At(x, y) = cpuReadback[static_cast<size_t>(y) * 5 + x];
		}
		Plane<float> secondRender(6, 4, 6);
		ResampleNearest(uploadedNative, secondRender);
		for (uint32 y = 0; y < guest.height; ++y)
		{
			for (uint32 x = 0; x < guest.width; ++x)
			{
				REQUIRE(uploadedNative.At(x, y) == guest.At(x, y));
				REQUIRE(secondRender.At(x * 2, y * 2) == guest.At(x, y));
			}
		}
	}

	void TestNativeBoundaryCopyOneToTwoX()
	{
		Plane<uint32> sourceNative(3, 2, 3);
		for (uint32 y = 0; y < sourceNative.height; ++y)
		{
			for (uint32 x = 0; x < sourceNative.width; ++x)
				sourceNative.At(x, y) = 0x100u + y * sourceNative.width + x;
		}
		Plane<uint32> sourceRender(6, 4, 6);
		ResampleNearest(sourceNative, sourceRender);

		Plane<uint32> recoveredSourceNative(3, 2, 3);
		ResampleNearest(sourceRender, recoveredSourceNative);
		Plane<uint32> destinationNative(3, 2, 4, 0xFFFFFFFFu);
		CopyRect(recoveredSourceNative, destinationNative, 0, 0, 0, 0, 3, 2);
		Plane<uint32> destinationRender(6, 4, 7, 0xEEEEEEEEu);
		ResampleNearest(destinationNative, destinationRender);

		for (uint32 y = 0; y < sourceNative.height; ++y)
		{
			for (uint32 x = 0; x < sourceNative.width; ++x)
			{
				REQUIRE(destinationNative.At(x, y) == sourceNative.At(x, y));
				REQUIRE(destinationRender.At(x * 2, y * 2) == sourceNative.At(x, y));
			}
		}
		REQUIRE(destinationNative.At(3, 0) == 0xFFFFFFFFu);
		REQUIRE(destinationRender.At(6, 0) == 0xEEEEEEEEu);
	}

	void TestSameScaleCopyMipSliceAndRect()
	{
		std::array<std::array<Plane<uint32>, 2>, 2> source{{
			{Plane<uint32>(4, 4, 4), Plane<uint32>(2, 2, 2)},
			{Plane<uint32>(4, 4, 4), Plane<uint32>(2, 2, 2)},
		}};
		std::array<std::array<Plane<uint32>, 2>, 2> destination{{
			{Plane<uint32>(4, 4, 5, 0xDEADBEEFu), Plane<uint32>(2, 2, 3, 0xDEADBEEFu)},
			{Plane<uint32>(4, 4, 5, 0xDEADBEEFu), Plane<uint32>(2, 2, 3, 0xDEADBEEFu)},
		}};
		for (uint32 slice = 0; slice < 2; ++slice)
		{
			for (uint32 mip = 0; mip < 2; ++mip)
			{
				auto& plane = source[slice][mip];
				for (uint32 y = 0; y < plane.height; ++y)
				{
					for (uint32 x = 0; x < plane.width; ++x)
						plane.At(x, y) = 0x10000000u * (slice + 1) + 0x10000u * mip + y * plane.width + x;
				}
			}
		}

		CopyRect(source[1][0], destination[1][0], 1, 1, 0, 0, 2, 2);
		REQUIRE(destination[1][0].At(0, 0) == source[1][0].At(1, 1));
		REQUIRE(destination[1][0].At(1, 1) == source[1][0].At(2, 2));
		REQUIRE(destination[1][0].At(2, 0) == 0xDEADBEEFu);
		REQUIRE(destination[0][0].At(0, 0) == 0xDEADBEEFu);
		REQUIRE(destination[1][1].At(0, 0) == 0xDEADBEEFu);
		REQUIRE(destination[1][0].At(4, 0) == 0xDEADBEEFu);
	}
}

int main()
{
	TestContentSerials();
	TestSubresourceSerialIsolation();
	TestBackendFailureFallbackAndCompanionLifetime();
	TestP3ResolutionPolicy();
	TestColorRoundTrip();
	TestLinearDownsample();
	TestDepthMipAndArraySlices();
	TestGpuCpuGpuRoundTripAndRowPitch();
	TestNativeBoundaryCopyOneToTwoX();
	TestSameScaleCopyMipSliceAndRect();
	std::cout << "Surface representation harness passed\n";
	return 0;
}
