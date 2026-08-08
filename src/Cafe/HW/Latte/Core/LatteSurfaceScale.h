#pragma once

#include <cstdint>
#include <limits>

struct LatteSurfaceExtent
{
	uint32 width{};
	uint32 height{};
	uint32 depth{1};
};

struct LatteSurfaceRect
{
	sint32 x{};
	sint32 y{};
	sint32 width{};
	sint32 height{};
};

enum class LatteSurfaceUsage : uint8
{
	Unknown,
	Sampled,
	ColorAttachment,
	DepthStencilAttachment,
	CopySource,
	CopyDestination,
	ResolveSource,
	ResolveDestination,
	PresentSource,
	GuestUpload,
	CpuReadback,
	LinearStaging,
	Count,
};

enum class LatteSurfaceEdgeType : uint8
{
	AttachmentPair,
	CompatibleCopy,
	Resolve,
	CompatibleAlias,
	Conflict,
};

enum class LatteSurfaceScaleClass : uint8
{
	Unknown,
	ScalableRenderFamily,
	Conditional,
	ForceNative,
};

enum class LatteSurfaceScaleSource : uint8
{
	Native,
	RenderSurfaceScale,
	StaticTextureScale,
	TitlePolicy,
	GraphicPackFixed,
	SafetyFallback,
};

enum class LatteSurfaceFallbackReason : uint8
{
	None,
	UnknownUsage,
	StaticSampledTexture,
	CompressedFormat,
	CpuReadable,
	LinearLayout,
	VideoSurface,
	AliasConflict,
	FormatReinterpret,
	CopyScaleConflict,
	GraphicPackConflict,
	MsaaUnsupported,
	DimensionUnsupported,
	FormatUnsupported,
	MemoryBudgetExceeded,
	AllocationFailed,
	BackendOperationUnsupported,
};

enum class LatteSurfaceScaleCompatibility : uint8
{
	Compatible,
	WidthMismatch,
	HeightMismatch,
	WidthAndHeightMismatch,
};

// A Latte texture always exposes the Render role to normal GPU consumers. When the
// render extent differs from the guest extent, GuestNative is a lazily-created
// companion used as the stable upload, readback and incompatible-copy boundary.
enum class LatteTextureRepresentation : uint8
{
	Render,
	GuestNative,
};

enum class LatteSurfaceResampleFilter : uint8
{
	Nearest,
	Linear,
};

enum class LatteSurfaceCopyPath : uint8
{
	DirectRender,
	NativeBoundary,
};

constexpr LatteSurfaceCopyPath LatteSurfaceSelectCopyPath(bool scaleCompatible,
	bool formatReinterpretation, bool aliasConflict)
{
	return scaleCompatible && !formatReinterpretation && !aliasConflict ?
		LatteSurfaceCopyPath::DirectRender : LatteSurfaceCopyPath::NativeBoundary;
}

struct LatteSurfaceSubresourceRange
{
	uint32 firstMip{};
	uint32 mipCount{1};
	uint32 firstSlice{};
	uint32 sliceCount{1};
};

struct LatteSurfaceOperationResult
{
	bool succeeded{};
	LatteSurfaceFallbackReason reason{LatteSurfaceFallbackReason::None};
	uint64 bytes{};

	static constexpr LatteSurfaceOperationResult Success(uint64 bytes = 0)
	{
		return {true, LatteSurfaceFallbackReason::None, bytes};
	}

	static constexpr LatteSurfaceOperationResult Failure(LatteSurfaceFallbackReason reason)
	{
		return {false, reason, 0};
	}
};

struct LatteSurfaceLinearCopyLayout
{
	bool valid{};
	uint64 sourceRowBytes{};
	uint64 copyRowBytes{};
	uint64 destinationStrideBytes{};
};

constexpr LatteSurfaceLinearCopyLayout LatteSurfaceResolveLinearCopyLayout(uint32 sourceWidth,
	uint32 sourceHeight, uint32 destinationWidth, uint32 destinationHeight, uint32 destinationPitch,
	uint32 bytesPerPixel)
{
	if (bytesPerPixel == 0 || destinationWidth == 0 || destinationHeight == 0 ||
		destinationWidth > sourceWidth || destinationHeight > sourceHeight || destinationPitch < destinationWidth)
	{
		return {};
	}
	return {
		true,
		static_cast<uint64>(sourceWidth) * bytesPerPixel,
		static_cast<uint64>(destinationWidth) * bytesPerPixel,
		static_cast<uint64>(destinationPitch) * bytesPerPixel,
	};
}

// Content authority is tracked per mip/slice. The maximum serial is authoritative;
// equal serials mean that the corresponding representations contain the same data.
// These values deliberately reuse Latte's global texture update event counter.
struct LatteSurfaceContentSerials
{
	uint64 guestRam{};
	uint64 guestNativeImage{};
	uint64 renderImage{};

	constexpr uint64 Get(LatteTextureRepresentation representation) const
	{
		return representation == LatteTextureRepresentation::Render ? renderImage : guestNativeImage;
	}

	constexpr uint64& Get(LatteTextureRepresentation representation)
	{
		return representation == LatteTextureRepresentation::Render ? renderImage : guestNativeImage;
	}

	constexpr uint64 Latest() const
	{
		const uint64 imageLatest = guestNativeImage > renderImage ? guestNativeImage : renderImage;
		return guestRam > imageLatest ? guestRam : imageLatest;
	}

	constexpr bool IsCurrent(LatteTextureRepresentation representation) const
	{
		return Get(representation) != 0 && Get(representation) == Latest();
	}

	constexpr void MarkGuestWrite(uint64 serial)
	{
		guestRam = serial;
	}

	constexpr void MarkGuestUpload(uint64 serial, bool representationsAlias)
	{
		guestRam = serial;
		guestNativeImage = serial;
		if (representationsAlias)
			renderImage = serial;
	}

	constexpr void MarkGuestDirectRenderUpload(uint64 serial)
	{
		guestRam = serial;
		guestNativeImage = 0;
		renderImage = serial;
	}

	constexpr void MarkRenderWrite(uint64 serial, bool representationsAlias)
	{
		renderImage = serial;
		if (representationsAlias)
			guestNativeImage = serial;
	}

	constexpr void MarkSynchronized(LatteTextureRepresentation source, LatteTextureRepresentation destination)
	{
		Get(destination) = Get(source);
	}

	constexpr void MarkGuestReadback(LatteTextureRepresentation source)
	{
		guestRam = Get(source);
	}
};

struct LatteSurfaceRepresentationSyncPlan
{
	bool available{};
	bool requiresBackendOperation{};
	LatteTextureRepresentation source{LatteTextureRepresentation::Render};
	LatteTextureRepresentation destination{LatteTextureRepresentation::Render};
};

constexpr LatteSurfaceRepresentationSyncPlan LatteSurfacePlanRepresentationSync(
	const LatteSurfaceContentSerials& serials, LatteTextureRepresentation destination, bool representationsAlias)
{
	if (serials.IsCurrent(destination))
		return {true, false, destination, destination};
	const auto source = destination == LatteTextureRepresentation::Render ?
		LatteTextureRepresentation::GuestNative : LatteTextureRepresentation::Render;
	if (!serials.IsCurrent(source))
		return {false, false, source, destination};
	return {true, !representationsAlias, source, destination};
}

struct LatteSurfaceScaleCompatibilityResult
{
	LatteSurfaceScaleCompatibility reason{LatteSurfaceScaleCompatibility::Compatible};
	bool compatible{true};
};

struct LatteSurfaceResolutionInfo
{
	LatteSurfaceExtent guestExtent{};
	LatteSurfaceExtent requestedHostExtent{};
	LatteSurfaceExtent hostExtent{};
	LatteSurfaceScaleClass scaleClass{LatteSurfaceScaleClass::Unknown};
	LatteSurfaceScaleSource requestedSource{LatteSurfaceScaleSource::Native};
	LatteSurfaceScaleSource source{LatteSurfaceScaleSource::Native};
	LatteSurfaceFallbackReason fallbackReason{LatteSurfaceFallbackReason::None};
	uint64 familyId{};
	uint32 scaleGeneration{};
	uint32 configuredScalePercent{100};
	uint32 activeScalePercent{100};
	bool cpuReadable{};
};

struct LatteSurfaceResolutionInput
{
	LatteSurfaceExtent guestExtent{};
	bool hasGraphicPackExtent{};
	LatteSurfaceExtent graphicPackExtent{};
	bool cpuReadable{};
	uint32 configuredRenderScalePercent{100};
	uint32 activeRenderScalePercent{100};
	uint32 configuredStaticTextureScaleFactor{1};
	uint32 activeStaticTextureScaleFactor{1};
	uint32 scaleGeneration{};
	LatteSurfaceScaleClass scaleClass{LatteSurfaceScaleClass::Unknown};
	LatteSurfaceFallbackReason nativeReason{LatteSurfaceFallbackReason::UnknownUsage};
};

struct LatteSurfaceClassificationInput
{
	LatteSurfaceUsage initialUsage{LatteSurfaceUsage::Unknown};
	bool compressed{};
	bool linearLayout{};
	bool msaa{};
	bool supportedDimension{true};
};

struct LatteSurfaceClassificationResult
{
	LatteSurfaceScaleClass scaleClass{LatteSurfaceScaleClass::Unknown};
	LatteSurfaceFallbackReason reason{LatteSurfaceFallbackReason::UnknownUsage};
};

constexpr LatteSurfaceClassificationResult LatteSurfaceClassify(const LatteSurfaceClassificationInput& input)
{
	if (input.compressed)
		return {LatteSurfaceScaleClass::ForceNative, LatteSurfaceFallbackReason::CompressedFormat};
	if (input.linearLayout)
		return {LatteSurfaceScaleClass::ForceNative, LatteSurfaceFallbackReason::LinearLayout};
	if (input.msaa)
		return {LatteSurfaceScaleClass::ForceNative, LatteSurfaceFallbackReason::MsaaUnsupported};
	if (!input.supportedDimension)
		return {LatteSurfaceScaleClass::ForceNative, LatteSurfaceFallbackReason::DimensionUnsupported};
	if (input.initialUsage == LatteSurfaceUsage::CpuReadback)
		return {LatteSurfaceScaleClass::ForceNative, LatteSurfaceFallbackReason::CpuReadable};
	if (input.initialUsage == LatteSurfaceUsage::ColorAttachment ||
		input.initialUsage == LatteSurfaceUsage::DepthStencilAttachment)
	{
		return {LatteSurfaceScaleClass::ScalableRenderFamily, LatteSurfaceFallbackReason::None};
	}
	if (input.initialUsage == LatteSurfaceUsage::Sampled || input.initialUsage == LatteSurfaceUsage::GuestUpload)
		return {LatteSurfaceScaleClass::Unknown, LatteSurfaceFallbackReason::StaticSampledTexture};
	return {};
}

constexpr uint32 LatteSurfaceMipDimension(uint32 value, uint32 mipLevel)
{
	if (mipLevel >= 31)
		return 1;
	const uint32 mipValue = value >> mipLevel;
	return mipValue == 0 ? 1 : mipValue;
}

constexpr LatteSurfaceExtent LatteSurfaceMipExtent(const LatteSurfaceExtent& extent, uint32 mipLevel, bool scaleDepth)
{
	return {
		LatteSurfaceMipDimension(extent.width, mipLevel),
		LatteSurfaceMipDimension(extent.height, mipLevel),
		scaleDepth ? LatteSurfaceMipDimension(extent.depth, mipLevel) : extent.depth,
	};
}

constexpr uint32 LatteSurfaceNormalizeRenderScalePercent(uint32 percent)
{
	return percent == 50 || percent == 200 ? percent : 100;
}

constexpr uint32 LatteSurfaceNormalizeStaticTextureScaleFactor(uint32 factor)
{
	return factor == 2 ? 2 : 1;
}

struct LatteSurfaceScaledExtentResult
{
	bool valid{};
	LatteSurfaceExtent extent{};
};

constexpr LatteSurfaceScaledExtentResult LatteSurfaceScaleExtentByPercent(
	const LatteSurfaceExtent& guestExtent, uint32 percent)
{
	percent = LatteSurfaceNormalizeRenderScalePercent(percent);
	const uint64 scaledWidth = (static_cast<uint64>(guestExtent.width) * percent + 99) / 100;
	const uint64 scaledHeight = (static_cast<uint64>(guestExtent.height) * percent + 99) / 100;
	if (scaledWidth == 0 || scaledHeight == 0 ||
		scaledWidth > std::numeric_limits<uint32>::max() ||
		scaledHeight > std::numeric_limits<uint32>::max())
	{
		return {};
	}
	return {true, {static_cast<uint32>(scaledWidth), static_cast<uint32>(scaledHeight),
		guestExtent.depth}};
}

constexpr LatteSurfaceScaleCompatibilityResult LatteSurfaceCompareScale(const LatteSurfaceExtent& firstGuest, const LatteSurfaceExtent& firstHost,
	const LatteSurfaceExtent& secondGuest, const LatteSurfaceExtent& secondHost)
{
	const bool widthMatches = static_cast<uint64>(firstHost.width) * secondGuest.width == static_cast<uint64>(secondHost.width) * firstGuest.width;
	const bool heightMatches = static_cast<uint64>(firstHost.height) * secondGuest.height == static_cast<uint64>(secondHost.height) * firstGuest.height;
	if (widthMatches && heightMatches)
		return {};
	if (!widthMatches && !heightMatches)
		return {LatteSurfaceScaleCompatibility::WidthAndHeightMismatch, false};
	return {widthMatches ? LatteSurfaceScaleCompatibility::HeightMismatch : LatteSurfaceScaleCompatibility::WidthMismatch, false};
}

constexpr bool LatteSurfaceHasSameScale(const LatteSurfaceExtent& firstGuest, const LatteSurfaceExtent& firstHost,
	const LatteSurfaceExtent& secondGuest, const LatteSurfaceExtent& secondHost)
{
	return LatteSurfaceCompareScale(firstGuest, firstHost, secondGuest, secondHost).compatible;
}

constexpr sint64 LatteSurfaceBoundaryFloor(sint64 boundary, uint32 guestExtent, uint32 hostExtent)
{
	if (guestExtent == 0)
		return 0;
	const sint64 numerator = boundary * static_cast<sint64>(hostExtent);
	if (numerator >= 0)
		return numerator / guestExtent;
	return -((-numerator + guestExtent - 1) / guestExtent);
}

constexpr sint64 LatteSurfaceBoundaryCeil(sint64 boundary, uint32 guestExtent, uint32 hostExtent)
{
	if (guestExtent == 0)
		return 0;
	const sint64 numerator = boundary * static_cast<sint64>(hostExtent);
	if (numerator >= 0)
		return (numerator + guestExtent - 1) / guestExtent;
	return -((-numerator) / guestExtent);
}

constexpr LatteSurfaceRect LatteSurfaceScaleRect(const LatteSurfaceRect& rect, const LatteSurfaceExtent& guestExtent,
	const LatteSurfaceExtent& hostExtent)
{
	const sint64 left = LatteSurfaceBoundaryFloor(rect.x, guestExtent.width, hostExtent.width);
	const sint64 top = LatteSurfaceBoundaryFloor(rect.y, guestExtent.height, hostExtent.height);
	const sint64 right = LatteSurfaceBoundaryCeil(static_cast<sint64>(rect.x) + rect.width, guestExtent.width, hostExtent.width);
	const sint64 bottom = LatteSurfaceBoundaryCeil(static_cast<sint64>(rect.y) + rect.height, guestExtent.height, hostExtent.height);
	return {
		static_cast<sint32>(left),
		static_cast<sint32>(top),
		static_cast<sint32>(right - left),
		static_cast<sint32>(bottom - top),
	};
}

class LatteSurfaceResolutionPolicy
{
public:
	static constexpr LatteSurfaceResolutionInfo Resolve(const LatteSurfaceResolutionInput& input)
	{
		LatteSurfaceResolutionInfo result;
		result.guestExtent = input.guestExtent;
		result.requestedHostExtent = input.guestExtent;
		result.hostExtent = input.guestExtent;
		result.cpuReadable = input.cpuReadable;
		const bool renderSurface = input.scaleClass == LatteSurfaceScaleClass::ScalableRenderFamily;
		const bool staticTexture = input.scaleClass == LatteSurfaceScaleClass::Unknown &&
			input.nativeReason == LatteSurfaceFallbackReason::StaticSampledTexture;
		result.configuredScalePercent = renderSurface ?
			LatteSurfaceNormalizeRenderScalePercent(input.configuredRenderScalePercent) :
			(staticTexture ? LatteSurfaceNormalizeStaticTextureScaleFactor(
				input.configuredStaticTextureScaleFactor) * 100 : 100);
		result.activeScalePercent = renderSurface ?
			LatteSurfaceNormalizeRenderScalePercent(input.activeRenderScalePercent) :
			(staticTexture ? LatteSurfaceNormalizeStaticTextureScaleFactor(
				input.activeStaticTextureScaleFactor) * 100 : 100);
		result.scaleGeneration = input.scaleGeneration;
		result.scaleClass = input.scaleClass;
		if (input.hasGraphicPackExtent)
		{
			result.requestedHostExtent = input.graphicPackExtent;
			result.hostExtent = input.graphicPackExtent;
			result.requestedSource = LatteSurfaceScaleSource::GraphicPackFixed;
			result.source = LatteSurfaceScaleSource::GraphicPackFixed;
			return result;
		}
		if ((renderSurface || staticTexture) && result.activeScalePercent != 100)
		{
			result.requestedSource = renderSurface ? LatteSurfaceScaleSource::RenderSurfaceScale :
				LatteSurfaceScaleSource::StaticTextureScale;
			const auto scaledExtent = LatteSurfaceScaleExtentByPercent(input.guestExtent,
				result.activeScalePercent);
			if (!scaledExtent.valid)
			{
				result.source = LatteSurfaceScaleSource::SafetyFallback;
				result.fallbackReason = LatteSurfaceFallbackReason::DimensionUnsupported;
				result.scaleClass = LatteSurfaceScaleClass::ForceNative;
				return result;
			}
			result.requestedHostExtent = scaledExtent.extent;
			result.hostExtent = result.requestedHostExtent;
			result.source = result.requestedSource;
		}
		else if (!renderSurface)
		{
			result.fallbackReason = input.nativeReason;
		}
		return result;
	}

	static constexpr LatteSurfaceResolutionInfo ApplyBackendFallback(LatteSurfaceResolutionInfo result,
		LatteSurfaceFallbackReason reason)
	{
		result.hostExtent = result.guestExtent;
		result.source = LatteSurfaceScaleSource::SafetyFallback;
		result.fallbackReason = reason;
		result.scaleClass = LatteSurfaceScaleClass::ForceNative;
		return result;
	}
};

constexpr uint32 LatteSurfaceUsageBit(LatteSurfaceUsage usage)
{
	return uint32{1} << static_cast<uint8>(usage);
}

static_assert(!LatteSurfaceHasSameScale({1280, 720, 1}, {1920, 720, 1}, {1280, 720, 1}, {1280, 720, 1}));
static_assert(!LatteSurfaceHasSameScale({1280, 720, 1}, {1280, 1080, 1}, {1280, 720, 1}, {1280, 720, 1}));
static_assert(LatteSurfaceCompareScale({1280, 720, 1}, {1920, 720, 1}, {1280, 720, 1}, {1280, 720, 1}).reason == LatteSurfaceScaleCompatibility::WidthMismatch);
static_assert(LatteSurfaceCompareScale({1280, 720, 1}, {1280, 1080, 1}, {1280, 720, 1}, {1280, 720, 1}).reason == LatteSurfaceScaleCompatibility::HeightMismatch);
static_assert(LatteSurfaceHasSameScale({1280, 720, 1}, {1920, 1080, 1}, {640, 360, 1}, {960, 540, 1}));
static_assert(LatteSurfaceMipExtent({3, 3, 1}, 2, false).width == 1);
static_assert(LatteSurfaceScaleRect({1, 1, 1, 1}, {3, 3, 1}, {5, 5, 1}).width == 3);
static_assert(LatteSurfaceResolutionPolicy::Resolve({{1280, 720, 1}, true, {1921, 1081, 1}, false, 100, 100, 1, 1, 0}).hostExtent.width == 1921);
static_assert(LatteSurfaceResolutionPolicy::Resolve({{1280, 720, 6}, true, {1920, 1080, 12}, false, 100, 100, 1, 1, 0}).hostExtent.depth == 12);
static_assert(LatteSurfaceResolutionPolicy::ApplyBackendFallback(
	LatteSurfaceResolutionPolicy::Resolve({{1280, 720, 1}, true, {2560, 1440, 1}, false, 100, 100, 1, 1, 0}),
	LatteSurfaceFallbackReason::AllocationFailed).hostExtent.width == 1280);
static_assert(LatteSurfaceResolveLinearCopyLayout(8, 4, 6, 3, 10, 4).valid);
static_assert(LatteSurfaceResolveLinearCopyLayout(8, 4, 6, 3, 10, 4).sourceRowBytes == 32);
static_assert(!LatteSurfaceResolveLinearCopyLayout(8, 4, 9, 3, 10, 4).valid);
static_assert(!LatteSurfaceResolveLinearCopyLayout(8, 4, 6, 3, 5, 4).valid);
static_assert(LatteSurfaceSelectCopyPath(true, false, false) == LatteSurfaceCopyPath::DirectRender);
static_assert(LatteSurfaceSelectCopyPath(false, false, false) == LatteSurfaceCopyPath::NativeBoundary);
static_assert(LatteSurfaceSelectCopyPath(true, true, false) == LatteSurfaceCopyPath::NativeBoundary);
static_assert(LatteSurfaceSelectCopyPath(true, false, true) == LatteSurfaceCopyPath::NativeBoundary);
static_assert(LatteSurfaceClassify({LatteSurfaceUsage::ColorAttachment}).scaleClass == LatteSurfaceScaleClass::ScalableRenderFamily);
static_assert(LatteSurfaceClassify({LatteSurfaceUsage::DepthStencilAttachment}).scaleClass == LatteSurfaceScaleClass::ScalableRenderFamily);
static_assert(LatteSurfaceClassify({LatteSurfaceUsage::Sampled}).reason == LatteSurfaceFallbackReason::StaticSampledTexture);
static_assert(LatteSurfaceClassify({LatteSurfaceUsage::ColorAttachment, true}).reason == LatteSurfaceFallbackReason::CompressedFormat);
static_assert(LatteSurfaceClassify({LatteSurfaceUsage::ColorAttachment, false, true}).reason == LatteSurfaceFallbackReason::LinearLayout);
static_assert(LatteSurfaceClassify({LatteSurfaceUsage::ColorAttachment, false, false, true}).reason == LatteSurfaceFallbackReason::MsaaUnsupported);
static_assert(LatteSurfaceResolutionPolicy::Resolve({{1280, 720, 1}, false, {}, false, 200, 200, 1, 1, 7,
	LatteSurfaceScaleClass::ScalableRenderFamily, LatteSurfaceFallbackReason::None}).hostExtent.width == 2560);
static_assert(LatteSurfaceResolutionPolicy::Resolve({{1280, 720, 1}, false, {}, false, 50, 50, 1, 1, 7,
	LatteSurfaceScaleClass::ScalableRenderFamily, LatteSurfaceFallbackReason::None}).hostExtent.width == 640);
static_assert(LatteSurfaceResolutionPolicy::Resolve({{1024, 1024, 1}, false, {}, false, 100, 100, 2, 2, 7,
	LatteSurfaceScaleClass::Unknown, LatteSurfaceFallbackReason::StaticSampledTexture}).hostExtent.width == 2048);

constexpr bool LatteSurfaceContentSerialSelfTest()
{
	LatteSurfaceContentSerials serials;
	serials.MarkGuestWrite(1);
	if (serials.Latest() != 1 || serials.IsCurrent(LatteTextureRepresentation::GuestNative))
		return false;
	serials.MarkGuestUpload(1, false);
	if (!serials.IsCurrent(LatteTextureRepresentation::GuestNative) || serials.IsCurrent(LatteTextureRepresentation::Render))
		return false;
	serials.MarkSynchronized(LatteTextureRepresentation::GuestNative, LatteTextureRepresentation::Render);
	if (!serials.IsCurrent(LatteTextureRepresentation::Render))
		return false;
	serials.MarkRenderWrite(2, false);
	if (!serials.IsCurrent(LatteTextureRepresentation::Render) || serials.IsCurrent(LatteTextureRepresentation::GuestNative))
		return false;
	serials.MarkSynchronized(LatteTextureRepresentation::Render, LatteTextureRepresentation::GuestNative);
	serials.MarkGuestReadback(LatteTextureRepresentation::GuestNative);
	return serials.guestRam == 2 && serials.guestNativeImage == 2 && serials.renderImage == 2;
}

static_assert(LatteSurfaceContentSerialSelfTest());
static_assert(LatteSurfacePlanRepresentationSync({1, 1, 0}, LatteTextureRepresentation::Render, false).requiresBackendOperation);
static_assert(!LatteSurfacePlanRepresentationSync({2, 1, 1}, LatteTextureRepresentation::Render, false).available);
