#include "Cafe/Diagnostics/SurfaceResolutionDiagnostics.h"
#include "Cafe/Diagnostics/ReflectionDebugDump.h"

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceScaleState.h"
#include "config/CemuConfig.h"

#include "spatial/debugbus/DebugCommandRegistry.h"
#include "spatial/profiler/Profiler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr size_t kUsageCount = static_cast<size_t>(LatteSurfaceUsage::Count);

	struct FirstUse
	{
		uint64 sequence{};
		uint32 frame{};
		uint32 draw{};
	};

	struct SurfaceRecord
	{
		uint64 id{};
		uint64 parent{};
		bool active{true};
		MPTR physAddress{};
		MPTR physMipAddress{};
		LatteSurfaceExtent guestExtent{};
		LatteSurfaceExtent requestedHostExtent{};
		LatteSurfaceExtent hostExtent{};
		LatteSurfaceScaleClass scaleClass{LatteSurfaceScaleClass::Unknown};
		LatteSurfaceScaleSource scaleSource{LatteSurfaceScaleSource::Native};
		LatteSurfaceFallbackReason fallbackReason{LatteSurfaceFallbackReason::None};
		uint32 pitch{};
		uint32 format{};
		uint32 hostFormat{};
		uint32 dim{};
		uint32 tileMode{};
		uint32 mipLevels{};
		bool depth{};
		bool compressed{};
		bool cpuReadable{};
		bool graphicPackFixed{};
		uint32 usageMask{};
		std::array<FirstUse, kUsageCount> firstUse{};
		uint64 estimatedGuestBytes{};
		uint64 estimatedHostBytes{};
		uint64 uploadBytes{};
		uint64 copySourceCount{};
		uint64 copyDestinationCount{};
		uint64 readbackCount{};
		uint64 readbackFailureCount{};
		uint64 nativeCompanionBytes{};
		uint64 companionCreateCount{};
		uint64 companionReleaseCount{};
		uint64 upscaleCount{};
		uint64 downscaleCount{};
		uint64 representationSyncFailureCount{};
	};

	struct EdgeKey
	{
		uint64 first{};
		uint64 second{};
		LatteSurfaceEdgeType type{};

		bool operator==(const EdgeKey&) const = default;
	};

	struct EdgeKeyHash
	{
		size_t operator()(const EdgeKey& key) const
		{
			const size_t h1 = std::hash<uint64>{}(key.first);
			const size_t h2 = std::hash<uint64>{}(key.second);
			return h1 ^ std::rotl(h2, 17) ^ (static_cast<size_t>(key.type) << 1);
		}
	};

	struct EdgeRecord
	{
		EdgeKey key{};
		std::string reason;
		uint64 count{1};
	};

	struct FailureKey
	{
		bool nativeBoundary{};
		LatteSurfaceFallbackReason reason{LatteSurfaceFallbackReason::None};
		LatteTextureRepresentation sourceRepresentation{LatteTextureRepresentation::Render};
		LatteTextureRepresentation destinationRepresentation{LatteTextureRepresentation::Render};
		uint32 sourceFormat{};
		uint32 destinationFormat{};
		uint32 sourceHostFormat{};
		uint32 destinationHostFormat{};
		bool sourceDepth{};
		bool destinationDepth{};

		bool operator==(const FailureKey&) const = default;
	};

	struct FailureKeyHash
	{
		size_t operator()(const FailureKey& key) const
		{
			size_t hash = std::hash<uint32>{}(key.sourceFormat);
			hash ^= std::rotl(std::hash<uint32>{}(key.destinationFormat), 5);
			hash ^= std::rotl(std::hash<uint32>{}(key.sourceHostFormat), 11);
			hash ^= std::rotl(std::hash<uint32>{}(key.destinationHostFormat), 17);
			hash ^= static_cast<size_t>(key.nativeBoundary) << 1;
			hash ^= static_cast<size_t>(key.reason) << 3;
			hash ^= static_cast<size_t>(key.sourceRepresentation) << 8;
			hash ^= static_cast<size_t>(key.destinationRepresentation) << 10;
			hash ^= static_cast<size_t>(key.sourceDepth) << 12;
			hash ^= static_cast<size_t>(key.destinationDepth) << 13;
			return hash;
		}
	};

	struct FailureRecord
	{
		FailureKey key{};
		uint64 count{};
		uint64 firstSourceSurfaceId{};
		uint64 firstDestinationSurfaceId{};
	};

	// DebugBus payloads are deliberately flat: foundation reflection owns field names and the
	// generic serializer owns text formatting. Adding a diagnostic field therefore only requires
	// changing its payload and reflection declaration, not every command-specific string builder.
	struct StatusPayload
	{
		std::string configured_factor{"1"};
		std::string active_factor{"1"};
		uint64 configured_render_scale_percent{100};
		uint64 active_render_scale_percent{100};
		uint64 configured_static_texture_scale_factor{1};
		uint64 active_static_texture_scale_factor{1};
		bool render_pending_restart{};
		bool static_texture_pending_restart{};
		bool pending_restart{};
		uint64 generation{};
		uint64 title_generation{};
		uint64 surfaces_active{};
		uint64 surfaces_total{};
		uint64 families_total{};
		uint64 families_scaled{};
		uint64 families_render_scaled{};
		uint64 families_static_texture_scaled{};
		uint64 families_native{};
		uint64 families_fallback{};
		uint64 graphic_pack_fixed_surfaces{};
		uint64 native_bytes_estimated{};
		uint64 scaled_bytes{};
		uint64 render_scaled_bytes{};
		uint64 static_texture_scaled_bytes{};
		uint64 native_companion_bytes{};
		uint64 native_companion_peak_bytes{};
		uint64 companion_creates{};
		uint64 companion_releases{};
		uint64 representation_upscales{};
		uint64 representation_downscales{};
		uint64 representation_sync_failures{};
		uint64 native_boundary_copies{};
		uint64 native_boundary_copy_failures{};
		uint64 copy_scale_conflicts{};
		uint64 alias_conflicts{};
		uint64 readbacks_total{};
		uint64 resized_readbacks{};
		uint64 readback_failures{};
		std::string tv_guest_extent;
		std::string tv_host_extent;
		std::string pad_guest_extent;
		std::string pad_host_extent;
	};
	REFLECT_CLASS_PROPERTIES(StatusPayload, configured_factor, active_factor,
		configured_render_scale_percent, active_render_scale_percent,
		configured_static_texture_scale_factor, active_static_texture_scale_factor,
		render_pending_restart, static_texture_pending_restart, pending_restart,
		generation, title_generation, surfaces_active, surfaces_total, families_total, families_scaled,
		families_render_scaled, families_static_texture_scaled, families_native, families_fallback,
		graphic_pack_fixed_surfaces, native_bytes_estimated, scaled_bytes, render_scaled_bytes,
		static_texture_scaled_bytes, native_companion_bytes, native_companion_peak_bytes, companion_creates,
		companion_releases, representation_upscales, representation_downscales,
		representation_sync_failures, native_boundary_copies, native_boundary_copy_failures,
		copy_scale_conflicts, alias_conflicts, readbacks_total,
		resized_readbacks, readback_failures, tv_guest_extent, tv_host_extent, pad_guest_extent,
		pad_host_extent)

	struct FamilyListPayload
	{
		uint64 families_total{};
		uint64 families_returned{};
	};
	REFLECT_CLASS_PROPERTIES(FamilyListPayload, families_total, families_returned)

	struct FamilyPayload
	{
		uint64 family_id{};
		uint64 surface_count{};
		std::string surface_ids;
		std::string guest_extent;
		std::string requested_host_extent;
		std::string host_extent;
		std::string usage;
		std::string scale_class;
		std::string scale_source;
		std::string fallback_reason;
		bool present_source{};
		bool graphic_pack_fixed{};
		uint64 estimated_host_bytes{};
		uint64 upload_bytes{};
		uint64 readback_count{};
		uint64 compressed_surface_count{};
		uint64 edge_count{};
		uint64 conflict_count{};
		std::string edge_types;
	};
	REFLECT_CLASS_PROPERTIES(FamilyPayload, family_id, surface_count, surface_ids, guest_extent,
		requested_host_extent, host_extent, usage, scale_class, scale_source, fallback_reason,
		present_source, graphic_pack_fixed, estimated_host_bytes, upload_bytes,
		readback_count, compressed_surface_count, edge_count, conflict_count, edge_types)

	struct SurfaceListPayload
	{
		uint64 query{};
		uint64 matches{};
	};
	REFLECT_CLASS_PROPERTIES(SurfaceListPayload, query, matches)

	struct SurfacePayload
	{
		uint64 surface_id{};
		uint64 family_id{};
		bool active{};
		std::string phys_address;
		std::string phys_mip_address;
		std::string guest_extent;
		std::string requested_host_extent;
		std::string host_extent;
		uint64 pitch{};
		std::string format;
		std::string host_format;
		uint64 dimension{};
		uint64 tile_mode{};
		uint64 mip_levels{};
		uint64 sample_count{1};
		uint64 slice_or_layer_count{};
		bool depth_attachment{};
		bool compressed{};
		bool cpu_readable{};
		bool graphic_pack_fixed{};
		std::string usage;
		uint64 estimated_guest_bytes{};
		uint64 estimated_host_bytes{};
		uint64 upload_bytes{};
		std::string scale_source;
		std::string scale_class;
		std::string fallback_reason{"None"};
		uint64 copy_source_count{};
		uint64 copy_destination_count{};
		uint64 readback_count{};
		uint64 readback_failure_count{};
		uint64 native_companion_bytes{};
		uint64 companion_create_count{};
		uint64 companion_release_count{};
		uint64 upscale_count{};
		uint64 downscale_count{};
		uint64 representation_sync_failure_count{};
	};
	REFLECT_CLASS_PROPERTIES(SurfacePayload, surface_id, family_id, active, phys_address,
		phys_mip_address, guest_extent, requested_host_extent, host_extent, pitch, format, host_format, dimension, tile_mode,
		mip_levels, sample_count, slice_or_layer_count, depth_attachment, compressed, cpu_readable,
		graphic_pack_fixed, usage, estimated_guest_bytes, estimated_host_bytes, upload_bytes,
		scale_source, scale_class, fallback_reason, copy_source_count, copy_destination_count, readback_count,
		readback_failure_count, native_companion_bytes, companion_create_count,
		companion_release_count, upscale_count, downscale_count, representation_sync_failure_count)

	struct FirstUsePayload
	{
		std::string usage;
		uint64 sequence{};
		uint64 frame{};
		uint64 draw{};
	};
	REFLECT_CLASS_PROPERTIES(FirstUsePayload, usage, sequence, frame, draw)

	struct EdgePayload
	{
		std::string edge_type;
		uint64 first_surface_id{};
		uint64 second_surface_id{};
		uint64 count{};
		std::string reason;
	};
	REFLECT_CLASS_PROPERTIES(EdgePayload, edge_type, first_surface_id, second_surface_id, count, reason)

	struct TransitionListPayload
	{
		uint64 title_generation{};
		uint64 transition_count{};
		uint64 sampled_to_render_target_count{};
		uint64 render_target_to_sampled_count{};
		uint64 transitions_returned{};
	};
	REFLECT_CLASS_PROPERTIES(TransitionListPayload, title_generation, transition_count,
		sampled_to_render_target_count, render_target_to_sampled_count, transitions_returned)

	struct TransitionPayload
	{
		uint64 surface_id{};
		uint64 family_id{};
		bool active{};
		std::string transition;
		std::string render_target_usage;
		std::string guest_extent;
		std::string host_extent;
		std::string format;
		bool graphic_pack_fixed{};
		uint64 sampled_sequence{};
		uint64 sampled_frame{};
		uint64 sampled_draw{};
		uint64 render_target_sequence{};
		uint64 render_target_frame{};
		uint64 render_target_draw{};
	};
	REFLECT_CLASS_PROPERTIES(TransitionPayload, surface_id, family_id, active, transition,
		render_target_usage, guest_extent, host_extent, format, graphic_pack_fixed, sampled_sequence,
		sampled_frame, sampled_draw, render_target_sequence, render_target_frame, render_target_draw)

	struct GraphicPackConflictListPayload
	{
		uint64 title_generation{};
		uint64 graphic_pack_fixed_surfaces{};
		uint64 graphic_pack_conflict_count{};
		uint64 conflicts_returned{};
	};
	REFLECT_CLASS_PROPERTIES(GraphicPackConflictListPayload, title_generation,
		graphic_pack_fixed_surfaces, graphic_pack_conflict_count, conflicts_returned)

	struct GraphicPackConflictPayload
	{
		std::string edge_type;
		std::string reason;
		uint64 count{};
		uint64 first_surface_id{};
		bool first_graphic_pack_fixed{};
		std::string first_guest_extent;
		std::string first_host_extent;
		uint64 second_surface_id{};
		bool second_graphic_pack_fixed{};
		std::string second_guest_extent;
		std::string second_host_extent;
	};
	REFLECT_CLASS_PROPERTIES(GraphicPackConflictPayload, edge_type, reason, count, first_surface_id,
		first_graphic_pack_fixed, first_guest_extent, first_host_extent, second_surface_id,
		second_graphic_pack_fixed, second_guest_extent, second_host_extent)

	struct FailureListPayload
	{
		uint64 failure_kinds{};
		uint64 failures_total{};
		uint64 failures_returned{};
	};
	REFLECT_CLASS_PROPERTIES(FailureListPayload, failure_kinds, failures_total, failures_returned)

	struct FailurePayload
	{
		std::string operation;
		std::string reason;
		uint64 count{};
		uint64 first_source_surface_id{};
		uint64 first_destination_surface_id{};
		std::string source_representation;
		std::string destination_representation;
		std::string source_format;
		std::string destination_format;
		std::string source_host_format;
		std::string destination_host_format;
		bool source_depth{};
		bool destination_depth{};
	};
	REFLECT_CLASS_PROPERTIES(FailurePayload, operation, reason, count, first_source_surface_id,
		first_destination_surface_id, source_representation, destination_representation,
		source_format, destination_format, source_host_format, destination_host_format,
		source_depth, destination_depth)

	std::mutex s_mutex;
	std::unordered_map<uint64, SurfaceRecord> s_surfaces;
	std::unordered_map<EdgeKey, size_t, EdgeKeyHash> s_edgeIndices;
	std::vector<EdgeRecord> s_edges;
	std::unordered_map<FailureKey, FailureRecord, FailureKeyHash> s_failureRecords;
	uint64 s_nextSurfaceId{1};
	uint64 s_nextUseSequence{1};
	uint64 s_copyScaleConflicts{};
	uint64 s_aliasConflicts{};
	uint64 s_readbacks{};
	uint64 s_resizedReadbacks{};
	uint64 s_readbackFailures{};
	uint64 s_nativeCompanionBytes{};
	uint64 s_nativeCompanionPeakBytes{};
	uint64 s_companionCreates{};
	uint64 s_companionReleases{};
	uint64 s_representationUpscales{};
	uint64 s_representationDownscales{};
	uint64 s_representationSyncFailures{};
	uint64 s_nativeBoundaryCopies{};
	uint64 s_nativeBoundaryCopyFailures{};
	uint64 s_titleGeneration{};
	SurfaceResolutionDiagnostics::PresentSourceSnapshot s_tvSource;
	SurfaceResolutionDiagnostics::PresentSourceSnapshot s_padSource;

	void ResetTitleStateLocked()
	{
		s_surfaces.clear();
		s_edgeIndices.clear();
		s_edges.clear();
		s_failureRecords.clear();
		s_nextUseSequence = 1;
		s_copyScaleConflicts = 0;
		s_aliasConflicts = 0;
		s_readbacks = 0;
		s_resizedReadbacks = 0;
		s_readbackFailures = 0;
		s_nativeCompanionBytes = 0;
		s_nativeCompanionPeakBytes = 0;
		s_companionCreates = 0;
		s_companionReleases = 0;
		s_representationUpscales = 0;
		s_representationDownscales = 0;
		s_representationSyncFailures = 0;
		s_nativeBoundaryCopies = 0;
		s_nativeBoundaryCopyFailures = 0;
		s_tvSource = {};
		s_padSource = {};
	}

	struct DiagnosticsSnapshot
	{
		std::unordered_map<uint64, SurfaceRecord> surfaces;
		std::vector<EdgeRecord> edges;
		std::vector<FailureRecord> failures;
		uint64 copyScaleConflicts{};
		uint64 aliasConflicts{};
		uint64 readbacks{};
		uint64 resizedReadbacks{};
		uint64 readbackFailures{};
		uint64 nativeCompanionBytes{};
		uint64 nativeCompanionPeakBytes{};
		uint64 companionCreates{};
		uint64 companionReleases{};
		uint64 representationUpscales{};
		uint64 representationDownscales{};
		uint64 representationSyncFailures{};
		uint64 nativeBoundaryCopies{};
		uint64 nativeBoundaryCopyFailures{};
		uint64 titleGeneration{};
		SurfaceResolutionDiagnostics::PresentSourceSnapshot tvSource;
		SurfaceResolutionDiagnostics::PresentSourceSnapshot padSource;
	};

	DiagnosticsSnapshot TakeSnapshot()
	{
		std::scoped_lock lock{s_mutex};
		return DiagnosticsSnapshot{
			.surfaces = s_surfaces,
			.edges = s_edges,
			.failures = [&]() {
				std::vector<FailureRecord> failures;
				failures.reserve(s_failureRecords.size());
				for (const auto& [key, failure] : s_failureRecords)
					failures.emplace_back(failure);
				return failures;
			}(),
			.copyScaleConflicts = s_copyScaleConflicts,
			.aliasConflicts = s_aliasConflicts,
			.readbacks = s_readbacks,
			.resizedReadbacks = s_resizedReadbacks,
			.readbackFailures = s_readbackFailures,
			.nativeCompanionBytes = s_nativeCompanionBytes,
			.nativeCompanionPeakBytes = s_nativeCompanionPeakBytes,
			.companionCreates = s_companionCreates,
			.companionReleases = s_companionReleases,
			.representationUpscales = s_representationUpscales,
			.representationDownscales = s_representationDownscales,
			.representationSyncFailures = s_representationSyncFailures,
			.nativeBoundaryCopies = s_nativeBoundaryCopies,
			.nativeBoundaryCopyFailures = s_nativeBoundaryCopyFailures,
			.titleGeneration = s_titleGeneration,
			.tvSource = s_tvSource,
			.padSource = s_padSource,
		};
	}

	uint64 FindRoot(const DiagnosticsSnapshot& snapshot, uint64 id)
	{
		while (true)
		{
			const auto it = snapshot.surfaces.find(id);
			if (it == snapshot.surfaces.end() || it->second.parent == id)
				return id;
			id = it->second.parent;
		}
	}

	const char* UsageName(LatteSurfaceUsage usage)
	{
		switch (usage)
		{
		case LatteSurfaceUsage::Unknown: return "Unknown";
		case LatteSurfaceUsage::Sampled: return "Sampled";
		case LatteSurfaceUsage::ColorAttachment: return "ColorAttachment";
		case LatteSurfaceUsage::DepthStencilAttachment: return "DepthStencilAttachment";
		case LatteSurfaceUsage::CopySource: return "CopySource";
		case LatteSurfaceUsage::CopyDestination: return "CopyDestination";
		case LatteSurfaceUsage::ResolveSource: return "ResolveSource";
		case LatteSurfaceUsage::ResolveDestination: return "ResolveDestination";
		case LatteSurfaceUsage::PresentSource: return "PresentSource";
		case LatteSurfaceUsage::GuestUpload: return "GuestUpload";
		case LatteSurfaceUsage::CpuReadback: return "CpuReadback";
		case LatteSurfaceUsage::LinearStaging: return "LinearStaging";
		case LatteSurfaceUsage::Count: break;
		}
		return "Invalid";
	}

	const char* ScaleClassName(LatteSurfaceScaleClass scaleClass)
	{
		switch (scaleClass)
		{
		case LatteSurfaceScaleClass::Unknown: return "Unknown";
		case LatteSurfaceScaleClass::ScalableRenderFamily: return "ScalableRenderFamily";
		case LatteSurfaceScaleClass::Conditional: return "Conditional";
		case LatteSurfaceScaleClass::ForceNative: return "ForceNative";
		}
		return "Invalid";
	}

	const char* ScaleSourceName(LatteSurfaceScaleSource source)
	{
		switch (source)
		{
		case LatteSurfaceScaleSource::Native: return "Native";
		case LatteSurfaceScaleSource::RenderSurfaceScale: return "RenderSurfaceScale";
		case LatteSurfaceScaleSource::StaticTextureScale: return "StaticTextureScale";
		case LatteSurfaceScaleSource::TitlePolicy: return "TitlePolicy";
		case LatteSurfaceScaleSource::GraphicPackFixed: return "GraphicPackFixed";
		case LatteSurfaceScaleSource::SafetyFallback: return "SafetyFallback";
		}
		return "Invalid";
	}

	const char* FallbackReasonName(LatteSurfaceFallbackReason reason)
	{
		switch (reason)
		{
		case LatteSurfaceFallbackReason::None: return "None";
		case LatteSurfaceFallbackReason::UnknownUsage: return "UnknownUsage";
		case LatteSurfaceFallbackReason::StaticSampledTexture: return "StaticSampledTexture";
		case LatteSurfaceFallbackReason::CompressedFormat: return "CompressedFormat";
		case LatteSurfaceFallbackReason::CpuReadable: return "CpuReadable";
		case LatteSurfaceFallbackReason::LinearLayout: return "LinearLayout";
		case LatteSurfaceFallbackReason::VideoSurface: return "VideoSurface";
		case LatteSurfaceFallbackReason::AliasConflict: return "AliasConflict";
		case LatteSurfaceFallbackReason::FormatReinterpret: return "FormatReinterpret";
		case LatteSurfaceFallbackReason::CopyScaleConflict: return "CopyScaleConflict";
		case LatteSurfaceFallbackReason::GraphicPackConflict: return "GraphicPackConflict";
		case LatteSurfaceFallbackReason::MsaaUnsupported: return "MsaaUnsupported";
		case LatteSurfaceFallbackReason::DimensionUnsupported: return "DimensionUnsupported";
		case LatteSurfaceFallbackReason::FormatUnsupported: return "FormatUnsupported";
		case LatteSurfaceFallbackReason::MemoryBudgetExceeded: return "MemoryBudgetExceeded";
		case LatteSurfaceFallbackReason::AllocationFailed: return "AllocationFailed";
		case LatteSurfaceFallbackReason::BackendOperationUnsupported: return "BackendOperationUnsupported";
		}
		return "Invalid";
	}

	const char* RepresentationName(LatteTextureRepresentation representation)
	{
		switch (representation)
		{
		case LatteTextureRepresentation::Render: return "Render";
		case LatteTextureRepresentation::GuestNative: return "GuestNative";
		}
		return "Invalid";
	}

	const char* EdgeName(LatteSurfaceEdgeType type)
	{
		switch (type)
		{
		case LatteSurfaceEdgeType::AttachmentPair: return "AttachmentPair";
		case LatteSurfaceEdgeType::CompatibleCopy: return "CompatibleCopy";
		case LatteSurfaceEdgeType::Resolve: return "Resolve";
		case LatteSurfaceEdgeType::CompatibleAlias: return "CompatibleAlias";
		case LatteSurfaceEdgeType::Conflict: return "Conflict";
		}
		return "Invalid";
	}

	std::string ExtentString(const LatteSurfaceExtent& extent, bool includeDepth = true)
	{
		std::string value = std::to_string(extent.width) + 'x' + std::to_string(extent.height);
		if (includeDepth)
			value += 'x' + std::to_string(extent.depth);
		return value;
	}

	std::string HexString(uint64 value)
	{
		std::array<char, 2 + std::numeric_limits<uint64>::digits + 1> buffer{};
		buffer[0] = '0';
		buffer[1] = 'x';
		const auto result = std::to_chars(buffer.data() + 2, buffer.data() + buffer.size(), value, 16);
		return std::string{buffer.data(), result.ptr};
	}

	std::string ScalePercentString(uint32 percent)
	{
		if (percent % 100 == 0)
			return std::to_string(percent / 100);
		return "0." + std::to_string(percent / 10);
	}

	uint32 GetHostFormat(const LatteTexture& texture)
	{
		return texture.overwriteInfo.hasFormatOverwrite ?
			static_cast<uint32>(texture.overwriteInfo.format) : static_cast<uint32>(texture.format);
	}

	void RecordFailureLocked(bool nativeBoundary, const LatteTexture& source, const LatteTexture& destination,
		LatteTextureRepresentation sourceRepresentation, LatteTextureRepresentation destinationRepresentation,
		LatteSurfaceFallbackReason reason)
	{
		FailureKey key{
			.nativeBoundary = nativeBoundary,
			.reason = reason,
			.sourceRepresentation = sourceRepresentation,
			.destinationRepresentation = destinationRepresentation,
			.sourceFormat = static_cast<uint32>(source.format),
			.destinationFormat = static_cast<uint32>(destination.format),
			.sourceHostFormat = GetHostFormat(source),
			.destinationHostFormat = GetHostFormat(destination),
			.sourceDepth = source.isDepth,
			.destinationDepth = destination.isDepth,
		};
		auto it = s_failureRecords.try_emplace(key, FailureRecord{
			.key = key,
			.firstSourceSurfaceId = source.diagnosticSurfaceId,
			.firstDestinationSurfaceId = destination.diagnosticSurfaceId,
		}).first;
		it->second.count++;
	}

	template<typename Payload>
	void AppendPayload(std::ostringstream& out, std::string_view begin, const Payload& payload, std::string_view end = {})
	{
		out << begin << '\n' << ReflectionDebugDump::PackScalarObject(payload);
		if (!end.empty())
			out << end << '\n';
	}

	uint64 EstimateBytes(uint32 width, uint32 height, uint32 depth, uint32 mipLevels, Latte::E_GX2SURFFMT format, Latte::E_DIM dim)
	{
		uint64 units{};
		const bool compressed = Latte::IsCompressedFormat(format);
		for (uint32 mip = 0; mip < std::max(mipLevels, 1u); ++mip)
		{
			const uint32 mipDepth = dim == Latte::E_DIM::DIM_3D ? std::max(depth >> mip, 1u) : std::max(depth, 1u);
			const uint32 mipWidth = std::max(width >> mip, 1u);
			const uint32 mipHeight = std::max(height >> mip, 1u);
			if (compressed)
				units += static_cast<uint64>((mipWidth + 3) / 4) * ((mipHeight + 3) / 4) * mipDepth;
			else
				units += static_cast<uint64>(mipWidth) * mipHeight * mipDepth;
		}
		return (units * Latte::GetFormatBits(format) + 7) / 8;
	}

	uint64 FindRootLocked(uint64 id)
	{
		auto it = s_surfaces.find(id);
		if (it == s_surfaces.end())
			return id;
		if (it->second.parent == id)
			return id;
		it->second.parent = FindRootLocked(it->second.parent);
		return it->second.parent;
	}

	void UnionLocked(uint64 first, uint64 second)
	{
		const uint64 firstRoot = FindRootLocked(first);
		const uint64 secondRoot = FindRootLocked(second);
		if (firstRoot == secondRoot)
			return;
		const uint64 root = std::min(firstRoot, secondRoot);
		const uint64 child = std::max(firstRoot, secondRoot);
		s_surfaces.at(child).parent = root;
	}

	std::string UsageMaskString(uint32 mask)
	{
		std::string result;
		for (size_t index = 0; index < kUsageCount; ++index)
		{
			const auto usage = static_cast<LatteSurfaceUsage>(index);
			if ((mask & LatteSurfaceUsageBit(usage)) == 0)
				continue;
			if (!result.empty())
				result += ',';
			result += UsageName(usage);
		}
		return result.empty() ? "None" : result;
	}

	std::optional<uint64> ParseValue(std::string_view text)
	{
		try
		{
			return std::stoull(std::string{text}, nullptr, 0);
		}
		catch (...)
		{
			return std::nullopt;
		}
	}

	bool ScaleRatiosMatch(const SurfaceRecord& first, const SurfaceRecord& second)
	{
		if (first.guestExtent.width == 0 || first.guestExtent.height == 0 ||
			second.guestExtent.width == 0 || second.guestExtent.height == 0)
		{
			return false;
		}
		return static_cast<uint64>(first.hostExtent.width) * second.guestExtent.width ==
				static_cast<uint64>(second.hostExtent.width) * first.guestExtent.width &&
			static_cast<uint64>(first.hostExtent.height) * second.guestExtent.height ==
				static_cast<uint64>(second.hostExtent.height) * first.guestExtent.height;
	}

	const FirstUse* EarliestRenderTargetUse(const SurfaceRecord& surface, LatteSurfaceUsage& usage)
	{
		const auto& color = surface.firstUse[static_cast<size_t>(LatteSurfaceUsage::ColorAttachment)];
		const auto& depth = surface.firstUse[static_cast<size_t>(LatteSurfaceUsage::DepthStencilAttachment)];
		if (color.sequence == 0 && depth.sequence == 0)
			return nullptr;
		if (depth.sequence == 0 || (color.sequence != 0 && color.sequence < depth.sequence))
		{
			usage = LatteSurfaceUsage::ColorAttachment;
			return &color;
		}
		usage = LatteSurfaceUsage::DepthStencilAttachment;
		return &depth;
	}

	std::string BuildStatus()
	{
		const auto snapshot = TakeSnapshot();
		const auto runtime = LatteSurfaceScaleState::GetSnapshot(
			GetConfig().render_surface_scale_percent.GetValue(),
			GetConfig().static_texture_scale_factor.GetValue());
		struct FamilyState
		{
			bool renderScaled{};
			bool staticTextureScaled{};
			bool fallback{};
		};
		std::unordered_map<uint64, FamilyState> families;
		StatusPayload payload;
		payload.configured_factor = ScalePercentString(runtime.configuredRenderScalePercent);
		payload.active_factor = ScalePercentString(runtime.activeRenderScalePercent);
		payload.configured_render_scale_percent = runtime.configuredRenderScalePercent;
		payload.active_render_scale_percent = runtime.activeRenderScalePercent;
		payload.configured_static_texture_scale_factor = runtime.configuredStaticTextureScaleFactor;
		payload.active_static_texture_scale_factor = runtime.activeStaticTextureScaleFactor;
		payload.render_pending_restart = runtime.renderPendingRestart;
		payload.static_texture_pending_restart = runtime.staticTexturePendingRestart;
		payload.pending_restart = runtime.pendingRestart;
		payload.generation = runtime.generation;
		payload.title_generation = snapshot.titleGeneration;
		payload.surfaces_total = snapshot.surfaces.size();
		for (const auto& [id, surface] : snapshot.surfaces)
		{
			if (!surface.active)
				continue;
			payload.surfaces_active++;
			auto& family = families[FindRoot(snapshot, id)];
			family.renderScaled |= surface.scaleSource == LatteSurfaceScaleSource::RenderSurfaceScale;
			family.staticTextureScaled |= surface.scaleSource == LatteSurfaceScaleSource::StaticTextureScale;
			family.fallback |= surface.scaleSource == LatteSurfaceScaleSource::SafetyFallback;
			payload.native_bytes_estimated += surface.estimatedGuestBytes;
			if (surface.scaleSource == LatteSurfaceScaleSource::RenderSurfaceScale ||
				surface.scaleSource == LatteSurfaceScaleSource::StaticTextureScale)
				payload.scaled_bytes += surface.estimatedHostBytes;
			if (surface.scaleSource == LatteSurfaceScaleSource::RenderSurfaceScale)
				payload.render_scaled_bytes += surface.estimatedHostBytes;
			else if (surface.scaleSource == LatteSurfaceScaleSource::StaticTextureScale)
				payload.static_texture_scaled_bytes += surface.estimatedHostBytes;
			payload.graphic_pack_fixed_surfaces += surface.graphicPackFixed;
		}
		payload.families_total = families.size();
		for (const auto& [id, family] : families)
		{
			if (family.fallback)
				payload.families_fallback++;
			else if (family.renderScaled || family.staticTextureScaled)
			{
				payload.families_scaled++;
				payload.families_render_scaled += family.renderScaled;
				payload.families_static_texture_scaled += family.staticTextureScaled;
			}
			else
				payload.families_native++;
		}
		payload.native_companion_bytes = snapshot.nativeCompanionBytes;
		payload.native_companion_peak_bytes = snapshot.nativeCompanionPeakBytes;
		payload.companion_creates = snapshot.companionCreates;
		payload.companion_releases = snapshot.companionReleases;
		payload.representation_upscales = snapshot.representationUpscales;
		payload.representation_downscales = snapshot.representationDownscales;
		payload.representation_sync_failures = snapshot.representationSyncFailures;
		payload.native_boundary_copies = snapshot.nativeBoundaryCopies;
		payload.native_boundary_copy_failures = snapshot.nativeBoundaryCopyFailures;
		payload.copy_scale_conflicts = snapshot.copyScaleConflicts;
		payload.alias_conflicts = snapshot.aliasConflicts;
		payload.readbacks_total = snapshot.readbacks;
		payload.resized_readbacks = snapshot.resizedReadbacks;
		payload.readback_failures = snapshot.readbackFailures;
		if (snapshot.tvSource.valid)
		{
			payload.tv_guest_extent = ExtentString(snapshot.tvSource.guestExtent);
			payload.tv_host_extent = ExtentString(snapshot.tvSource.hostExtent);
		}
		if (snapshot.padSource.valid)
		{
			payload.pad_guest_extent = ExtentString(snapshot.padSource.guestExtent);
			payload.pad_host_extent = ExtentString(snapshot.padSource.hostExtent);
		}
		return "surface_scale_status:\n" + ReflectionDebugDump::PackScalarObject(payload);
	}

	std::string BuildFamilies(size_t limit)
	{
		const auto snapshot = TakeSnapshot();
		struct Family
		{
			uint64 id{};
			uint32 usageMask{};
			uint64 bytes{};
			uint64 maxArea{};
			bool present{};
			bool renderTarget{};
			bool pack{};
			bool renderScaled{};
			bool staticTextureScaled{};
			bool fallback{};
			std::vector<const SurfaceRecord*> surfaces;
		};
		std::unordered_map<uint64, Family> byId;
		for (const auto& [id, surface] : snapshot.surfaces)
		{
			if (!surface.active)
				continue;
			const uint64 root = FindRoot(snapshot, id);
			auto& family = byId[root];
			family.id = root;
			family.usageMask |= surface.usageMask;
			family.bytes += surface.estimatedHostBytes;
			family.maxArea = std::max(family.maxArea, static_cast<uint64>(surface.hostExtent.width) * surface.hostExtent.height);
			family.present |= (surface.usageMask & LatteSurfaceUsageBit(LatteSurfaceUsage::PresentSource)) != 0;
			family.renderTarget |= (surface.usageMask & (LatteSurfaceUsageBit(LatteSurfaceUsage::ColorAttachment) | LatteSurfaceUsageBit(LatteSurfaceUsage::DepthStencilAttachment))) != 0;
			family.pack |= surface.graphicPackFixed;
			family.renderScaled |= surface.scaleSource == LatteSurfaceScaleSource::RenderSurfaceScale;
			family.staticTextureScaled |= surface.scaleSource == LatteSurfaceScaleSource::StaticTextureScale;
			family.fallback |= surface.scaleSource == LatteSurfaceScaleSource::SafetyFallback;
			family.surfaces.emplace_back(&surface);
		}
		std::vector<Family> families;
		families.reserve(byId.size());
		for (auto& [id, family] : byId)
			families.emplace_back(std::move(family));
		std::ranges::sort(families, [](const Family& lhs, const Family& rhs) {
			return std::tuple{lhs.present, lhs.renderTarget, lhs.surfaces.size(), lhs.maxArea, lhs.bytes, lhs.id} >
				std::tuple{rhs.present, rhs.renderTarget, rhs.surfaces.size(), rhs.maxArea, rhs.bytes, rhs.id};
		});

		std::ostringstream out;
		const FamilyListPayload listPayload{
			.families_total = families.size(),
			.families_returned = std::min(limit, families.size()),
		};
		AppendPayload(out, "surface_scale_families:", listPayload);
		for (size_t index = 0; index < std::min(limit, families.size()); ++index)
		{
			const auto& family = families[index];
			uint64 edgeCount{};
			uint64 conflictCount{};
			uint32 edgeTypeMask{};
			for (const auto& edge : snapshot.edges)
			{
				if (FindRoot(snapshot, edge.key.first) != family.id && FindRoot(snapshot, edge.key.second) != family.id)
					continue;
				edgeCount++;
				conflictCount += edge.key.type == LatteSurfaceEdgeType::Conflict;
				edgeTypeMask |= uint32{1} << static_cast<uint8>(edge.key.type);
			}
			const auto representativeIt = std::ranges::max_element(family.surfaces, {}, [](const SurfaceRecord* surface) {
				const bool present = (surface->usageMask & LatteSurfaceUsageBit(LatteSurfaceUsage::PresentSource)) != 0;
				return std::pair{present, static_cast<uint64>(surface->hostExtent.width) * surface->hostExtent.height};
			});
			const auto& representative = **representativeIt;
			const auto fallbackIt = std::ranges::find_if(family.surfaces, [](const SurfaceRecord* surface) {
				return surface->scaleSource == LatteSurfaceScaleSource::SafetyFallback;
			});
			const auto* fallbackSurface = fallbackIt == family.surfaces.end() ? nullptr : *fallbackIt;
			uint64 uploadBytes{};
			uint64 readbackCount{};
			uint64 compressedCount{};
			std::ostringstream surfaceIds;
			bool firstSurfaceId = true;
			for (const auto* surface : family.surfaces)
			{
				if (!firstSurfaceId)
					surfaceIds << ',';
				surfaceIds << surface->id;
				firstSurfaceId = false;
				uploadBytes += surface->uploadBytes;
				readbackCount += surface->readbackCount;
				compressedCount += surface->compressed;
			}
			std::string edgeTypes;
			for (uint32 typeIndex = 0; typeIndex <= static_cast<uint32>(LatteSurfaceEdgeType::Conflict); ++typeIndex)
			{
				if ((edgeTypeMask & (uint32{1} << typeIndex)) == 0)
					continue;
				if (!edgeTypes.empty())
					edgeTypes += ',';
				edgeTypes += EdgeName(static_cast<LatteSurfaceEdgeType>(typeIndex));
			}
			FamilyPayload payload{
				.family_id = family.id,
				.surface_count = family.surfaces.size(),
				.surface_ids = surfaceIds.str(),
				.guest_extent = ExtentString(representative.guestExtent),
				.requested_host_extent = ExtentString(representative.requestedHostExtent),
				.host_extent = ExtentString(representative.hostExtent),
				.usage = UsageMaskString(family.usageMask),
				.scale_class = ScaleClassName(representative.scaleClass),
				.scale_source = family.fallback ? "SafetyFallback" :
					(family.renderScaled && family.staticTextureScaled ? "MixedScale" :
						(family.renderScaled ? "RenderSurfaceScale" :
							(family.staticTextureScaled ? "StaticTextureScale" :
								ScaleSourceName(representative.scaleSource)))),
				.fallback_reason = fallbackSurface ? FallbackReasonName(fallbackSurface->fallbackReason) : "None",
				.present_source = family.present,
				.graphic_pack_fixed = family.pack,
				.estimated_host_bytes = family.bytes,
				.upload_bytes = uploadBytes,
				.readback_count = readbackCount,
				.compressed_surface_count = compressedCount,
				.edge_count = edgeCount,
				.conflict_count = conflictCount,
				.edge_types = edgeTypes.empty() ? "None" : std::move(edgeTypes),
			};
			AppendPayload(out, "family_begin", payload, "family_end");
		}
		return out.str();
	}

	std::string BuildSurface(uint64 value)
	{
		const auto snapshot = TakeSnapshot();
		std::vector<const SurfaceRecord*> matches;
		for (const auto& [id, surface] : snapshot.surfaces)
		{
			if (id == value || surface.physAddress == value || FindRoot(snapshot, id) == value)
				matches.emplace_back(&surface);
		}
		std::ranges::sort(matches, {}, &SurfaceRecord::id);
		std::ostringstream out;
		AppendPayload(out, "surface_scale_surface:", SurfaceListPayload{value, matches.size()});
		for (const auto* surface : matches)
		{
			const SurfacePayload payload{
				.surface_id = surface->id,
				.family_id = FindRoot(snapshot, surface->id),
				.active = surface->active,
				.phys_address = HexString(surface->physAddress),
				.phys_mip_address = HexString(surface->physMipAddress),
				.guest_extent = ExtentString(surface->guestExtent),
				.requested_host_extent = ExtentString(surface->requestedHostExtent),
				.host_extent = ExtentString(surface->hostExtent),
				.pitch = surface->pitch,
				.format = HexString(surface->format),
				.host_format = HexString(surface->hostFormat),
				.dimension = surface->dim,
				.tile_mode = surface->tileMode,
				.mip_levels = surface->mipLevels,
				.slice_or_layer_count = surface->guestExtent.depth,
				.depth_attachment = surface->depth,
				.compressed = surface->compressed,
				.cpu_readable = surface->cpuReadable,
				.graphic_pack_fixed = surface->graphicPackFixed,
				.usage = UsageMaskString(surface->usageMask),
				.estimated_guest_bytes = surface->estimatedGuestBytes,
				.estimated_host_bytes = surface->estimatedHostBytes,
				.upload_bytes = surface->uploadBytes,
				.scale_source = ScaleSourceName(surface->scaleSource),
				.scale_class = ScaleClassName(surface->scaleClass),
				.fallback_reason = FallbackReasonName(surface->fallbackReason),
				.copy_source_count = surface->copySourceCount,
				.copy_destination_count = surface->copyDestinationCount,
				.readback_count = surface->readbackCount,
				.readback_failure_count = surface->readbackFailureCount,
				.native_companion_bytes = surface->nativeCompanionBytes,
				.companion_create_count = surface->companionCreateCount,
				.companion_release_count = surface->companionReleaseCount,
				.upscale_count = surface->upscaleCount,
				.downscale_count = surface->downscaleCount,
				.representation_sync_failure_count = surface->representationSyncFailureCount,
			};
			AppendPayload(out, "surface_begin", payload);
			for (size_t usageIndex = 0; usageIndex < kUsageCount; ++usageIndex)
			{
				const auto& firstUse = surface->firstUse[usageIndex];
				if (firstUse.sequence == 0)
					continue;
				AppendPayload(out, "first_use_begin", FirstUsePayload{
					.usage = UsageName(static_cast<LatteSurfaceUsage>(usageIndex)),
					.sequence = firstUse.sequence,
					.frame = firstUse.frame,
					.draw = firstUse.draw,
				}, "first_use_end");
			}
			out << "surface_end\n";
		}
		out << "edges_begin\n";
		for (const auto& edge : snapshot.edges)
		{
			if (std::ranges::none_of(matches, [&](const SurfaceRecord* surface) { return surface->id == edge.key.first || surface->id == edge.key.second; }))
				continue;
			AppendPayload(out, "edge_begin", EdgePayload{
				.edge_type = EdgeName(edge.key.type),
				.first_surface_id = edge.key.first,
				.second_surface_id = edge.key.second,
				.count = edge.count,
				.reason = edge.reason.empty() ? "none" : edge.reason,
			}, "edge_end");
		}
		out << "edges_end\n";
		return out.str();
	}

	std::string BuildTransitions(size_t limit)
	{
		const auto snapshot = TakeSnapshot();
		struct Transition
		{
			const SurfaceRecord* surface{};
			const FirstUse* sampled{};
			const FirstUse* renderTarget{};
			LatteSurfaceUsage renderTargetUsage{};
			bool sampledFirst{};
		};
		std::vector<Transition> transitions;
		uint64 sampledToRenderTargetCount{};
		uint64 renderTargetToSampledCount{};
		for (const auto& [id, surface] : snapshot.surfaces)
		{
			const auto& sampled = surface.firstUse[static_cast<size_t>(LatteSurfaceUsage::Sampled)];
			LatteSurfaceUsage renderTargetUsage{};
			const FirstUse* renderTarget = EarliestRenderTargetUse(surface, renderTargetUsage);
			if (sampled.sequence == 0 || renderTarget == nullptr)
				continue;
			const bool sampledFirst = sampled.sequence < renderTarget->sequence;
			sampledToRenderTargetCount += sampledFirst;
			renderTargetToSampledCount += !sampledFirst;
			transitions.emplace_back(Transition{&surface, &sampled, renderTarget, renderTargetUsage, sampledFirst});
		}
		std::ranges::sort(transitions, [](const Transition& lhs, const Transition& rhs) {
			return std::tuple{lhs.sampledFirst, lhs.surface->active,
				std::min(lhs.sampled->sequence, lhs.renderTarget->sequence), lhs.surface->id} >
				std::tuple{rhs.sampledFirst, rhs.surface->active,
					std::min(rhs.sampled->sequence, rhs.renderTarget->sequence), rhs.surface->id};
		});

		std::ostringstream out;
		AppendPayload(out, "surface_scale_transitions:", TransitionListPayload{
			.title_generation = snapshot.titleGeneration,
			.transition_count = transitions.size(),
			.sampled_to_render_target_count = sampledToRenderTargetCount,
			.render_target_to_sampled_count = renderTargetToSampledCount,
			.transitions_returned = std::min(limit, transitions.size()),
		});
		for (size_t index = 0; index < std::min(limit, transitions.size()); ++index)
		{
			const auto& transition = transitions[index];
			const auto& surface = *transition.surface;
			AppendPayload(out, "transition_begin", TransitionPayload{
				.surface_id = surface.id,
				.family_id = FindRoot(snapshot, surface.id),
				.active = surface.active,
				.transition = transition.sampledFirst ? "SampledToRenderTarget" : "RenderTargetToSampled",
				.render_target_usage = UsageName(transition.renderTargetUsage),
				.guest_extent = ExtentString(surface.guestExtent),
				.host_extent = ExtentString(surface.hostExtent),
				.format = HexString(surface.format),
				.graphic_pack_fixed = surface.graphicPackFixed,
				.sampled_sequence = transition.sampled->sequence,
				.sampled_frame = transition.sampled->frame,
				.sampled_draw = transition.sampled->draw,
				.render_target_sequence = transition.renderTarget->sequence,
				.render_target_frame = transition.renderTarget->frame,
				.render_target_draw = transition.renderTarget->draw,
			}, "transition_end");
		}
		return out.str();
	}

	std::string BuildGraphicPackConflicts(size_t limit)
	{
		const auto snapshot = TakeSnapshot();
		struct Conflict
		{
			const EdgeRecord* edge{};
			const SurfaceRecord* first{};
			const SurfaceRecord* second{};
			std::string_view reason;
		};
		std::vector<Conflict> conflicts;
		uint64 fixedSurfaceCount{};
		for (const auto& [id, surface] : snapshot.surfaces)
			fixedSurfaceCount += surface.active && surface.graphicPackFixed;
		for (const auto& edge : snapshot.edges)
		{
			const auto firstIt = snapshot.surfaces.find(edge.key.first);
			const auto secondIt = snapshot.surfaces.find(edge.key.second);
			if (firstIt == snapshot.surfaces.end() || secondIt == snapshot.surfaces.end())
				continue;
			const auto& first = firstIt->second;
			const auto& second = secondIt->second;
			if ((!first.active && !second.active) || (!first.graphicPackFixed && !second.graphicPackFixed))
				continue;
			if (edge.key.type == LatteSurfaceEdgeType::Conflict)
				conflicts.emplace_back(Conflict{&edge, &first, &second, edge.reason.empty() ? std::string_view{"conflict"} : edge.reason});
			else if (!ScaleRatiosMatch(first, second))
				conflicts.emplace_back(Conflict{&edge, &first, &second, "graphic_pack_scale_mismatch"});
		}
		std::ranges::sort(conflicts, [](const Conflict& lhs, const Conflict& rhs) {
			return std::tuple{lhs.first->active || lhs.second->active, lhs.edge->count, lhs.edge->key.first, lhs.edge->key.second} >
				std::tuple{rhs.first->active || rhs.second->active, rhs.edge->count, rhs.edge->key.first, rhs.edge->key.second};
		});

		std::ostringstream out;
		AppendPayload(out, "surface_scale_graphic_pack_conflicts:", GraphicPackConflictListPayload{
			.title_generation = snapshot.titleGeneration,
			.graphic_pack_fixed_surfaces = fixedSurfaceCount,
			.graphic_pack_conflict_count = conflicts.size(),
			.conflicts_returned = std::min(limit, conflicts.size()),
		});
		for (size_t index = 0; index < std::min(limit, conflicts.size()); ++index)
		{
			const auto& conflict = conflicts[index];
			AppendPayload(out, "conflict_begin", GraphicPackConflictPayload{
				.edge_type = EdgeName(conflict.edge->key.type),
				.reason = std::string{conflict.reason},
				.count = conflict.edge->count,
				.first_surface_id = conflict.first->id,
				.first_graphic_pack_fixed = conflict.first->graphicPackFixed,
				.first_guest_extent = ExtentString(conflict.first->guestExtent, false),
				.first_host_extent = ExtentString(conflict.first->hostExtent, false),
				.second_surface_id = conflict.second->id,
				.second_graphic_pack_fixed = conflict.second->graphicPackFixed,
				.second_guest_extent = ExtentString(conflict.second->guestExtent, false),
				.second_host_extent = ExtentString(conflict.second->hostExtent, false),
			}, "conflict_end");
		}
		return out.str();
	}

	std::string BuildFailures(size_t limit)
	{
		auto snapshot = TakeSnapshot();
		std::ranges::sort(snapshot.failures, [](const FailureRecord& lhs, const FailureRecord& rhs) {
			if (lhs.count != rhs.count)
				return lhs.count > rhs.count;
			return static_cast<uint32>(lhs.key.reason) < static_cast<uint32>(rhs.key.reason);
		});
		uint64 total{};
		for (const auto& failure : snapshot.failures)
			total += failure.count;
		std::ostringstream out;
		AppendPayload(out, "surface_scale_failures:", FailureListPayload{
			.failure_kinds = snapshot.failures.size(),
			.failures_total = total,
			.failures_returned = std::min(limit, snapshot.failures.size()),
		});
		for (size_t index = 0; index < std::min(limit, snapshot.failures.size()); ++index)
		{
			const auto& failure = snapshot.failures[index];
			AppendPayload(out, "failure_begin", FailurePayload{
				.operation = failure.key.nativeBoundary ? "NativeBoundaryCopy" : "RepresentationSync",
				.reason = FallbackReasonName(failure.key.reason),
				.count = failure.count,
				.first_source_surface_id = failure.firstSourceSurfaceId,
				.first_destination_surface_id = failure.firstDestinationSurfaceId,
				.source_representation = RepresentationName(failure.key.sourceRepresentation),
				.destination_representation = RepresentationName(failure.key.destinationRepresentation),
				.source_format = HexString(failure.key.sourceFormat),
				.destination_format = HexString(failure.key.destinationFormat),
				.source_host_format = HexString(failure.key.sourceHostFormat),
				.destination_host_format = HexString(failure.key.destinationHostFormat),
				.source_depth = failure.key.sourceDepth,
				.destination_depth = failure.key.destinationDepth,
			}, "failure_end");
		}
		return out.str();
	}
}

void SurfaceResolutionDiagnostics::RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry)
{
	registry.Register("surface_scale_status", "Show internal-resolution surface summary", [](const std::vector<std::string>& args) {
		if (!args.empty())
			return std::string{"usage: surface_scale_status\n"};
		return BuildStatus();
	});
	registry.Register("surface_scale_families", "List observed surface families", [](const std::vector<std::string>& args) {
		if (args.size() > 1)
			return std::string{"usage: surface_scale_families [limit]\n"};
		size_t limit = 32;
		if (!args.empty())
		{
			auto parsed = ParseValue(args.front());
			if (!parsed || *parsed == 0)
				return std::string{"surface_scale_families invalid limit\n"};
			limit = std::min<uint64>(*parsed, 512);
		}
		return BuildFamilies(limit);
	});
	registry.Register("surface_scale_surface", "Show a surface or family by ID/address", [](const std::vector<std::string>& args) {
		if (args.size() != 1)
			return std::string{"usage: surface_scale_surface <family-or-address>\n"};
		auto value = ParseValue(args.front());
		return value ? BuildSurface(*value) : std::string{"surface_scale_surface invalid value\n"};
	});
	registry.Register("surface_scale_transitions", "List sampled/render-target first-use transitions", [](const std::vector<std::string>& args) {
		if (args.size() > 1)
			return std::string{"usage: surface_scale_transitions [limit]\n"};
		size_t limit = 32;
		if (!args.empty())
		{
			auto parsed = ParseValue(args.front());
			if (!parsed || *parsed == 0)
				return std::string{"surface_scale_transitions invalid limit\n"};
			limit = std::min<uint64>(*parsed, 512);
		}
		return BuildTransitions(limit);
	});
	registry.Register("surface_scale_graphic_pack_conflicts", "List Graphic Pack fixed surfaces and conflicts", [](const std::vector<std::string>& args) {
		if (args.size() > 1)
			return std::string{"usage: surface_scale_graphic_pack_conflicts [limit]\n"};
		size_t limit = 32;
		if (!args.empty())
		{
			auto parsed = ParseValue(args.front());
			if (!parsed || *parsed == 0)
				return std::string{"surface_scale_graphic_pack_conflicts invalid limit\n"};
			limit = std::min<uint64>(*parsed, 512);
		}
		return BuildGraphicPackConflicts(limit);
	});
	registry.Register("surface_scale_failures", "List internal-resolution operation failures", [](const std::vector<std::string>& args) {
		if (args.size() > 1)
			return std::string{"usage: surface_scale_failures [limit]\n"};
		size_t limit = 32;
		if (!args.empty())
		{
			auto parsed = ParseValue(args.front());
			if (!parsed || *parsed == 0)
				return std::string{"surface_scale_failures invalid limit\n"};
			limit = std::min<uint64>(*parsed, 512);
		}
		return BuildFailures(limit);
	});
	registry.Register("surface_scale_reset_stats", "Reset surface operation counters", [](const std::vector<std::string>& args) {
		if (!args.empty())
			return std::string{"usage: surface_scale_reset_stats\n"};
		std::scoped_lock lock{s_mutex};
		s_copyScaleConflicts = 0;
		s_aliasConflicts = 0;
		s_readbacks = 0;
		s_resizedReadbacks = 0;
		s_readbackFailures = 0;
		s_nativeCompanionPeakBytes = s_nativeCompanionBytes;
		s_companionCreates = 0;
		s_companionReleases = 0;
		s_representationUpscales = 0;
		s_representationDownscales = 0;
		s_representationSyncFailures = 0;
		s_nativeBoundaryCopies = 0;
		s_nativeBoundaryCopyFailures = 0;
		s_failureRecords.clear();
		for (auto& [id, surface] : s_surfaces)
		{
			surface.copySourceCount = 0;
			surface.copyDestinationCount = 0;
			surface.readbackCount = 0;
			surface.readbackFailureCount = 0;
			surface.companionCreateCount = 0;
			surface.companionReleaseCount = 0;
			surface.upscaleCount = 0;
			surface.downscaleCount = 0;
			surface.representationSyncFailureCount = 0;
		}
		return std::string{"surface_scale_reset_stats succeeded\n"};
	});
}

void SurfaceResolutionDiagnostics::BeginTitle()
{
	std::scoped_lock lock{s_mutex};
	ResetTitleStateLocked();
	s_titleGeneration++;
}

void SurfaceResolutionDiagnostics::EndTitle()
{
	std::scoped_lock lock{s_mutex};
	ResetTitleStateLocked();
}

uint64 SurfaceResolutionDiagnostics::RegisterSurface(const LatteTexture& texture)
{
	std::scoped_lock lock{s_mutex};
	SurfaceRecord record;
	record.id = s_nextSurfaceId++;
	record.parent = record.id;
	record.physAddress = texture.physAddress;
	record.physMipAddress = texture.physMipAddress;
	const auto& resolutionInfo = texture.GetResolutionInfo();
	record.guestExtent = resolutionInfo.guestExtent;
	record.requestedHostExtent = resolutionInfo.requestedHostExtent;
	record.hostExtent = resolutionInfo.hostExtent;
	record.scaleClass = resolutionInfo.scaleClass;
	record.scaleSource = resolutionInfo.source;
	record.fallbackReason = resolutionInfo.fallbackReason;
	record.pitch = texture.pitch;
	record.format = static_cast<uint32>(texture.format);
	record.hostFormat = texture.overwriteInfo.hasFormatOverwrite ? texture.overwriteInfo.format : record.format;
	record.dim = static_cast<uint32>(texture.dim);
	record.tileMode = static_cast<uint32>(texture.tileMode);
	record.mipLevels = texture.mipLevels;
	record.depth = texture.isDepth;
	record.compressed = texture.IsCompressedFormat();
	record.cpuReadable = texture.enableReadback;
	record.graphicPackFixed = resolutionInfo.source == LatteSurfaceScaleSource::GraphicPackFixed;
	record.estimatedGuestBytes = EstimateBytes(record.guestExtent.width, record.guestExtent.height, record.guestExtent.depth, record.mipLevels, texture.format, texture.dim);
	record.estimatedHostBytes = EstimateBytes(record.hostExtent.width, record.hostExtent.height, record.hostExtent.depth, record.mipLevels, static_cast<Latte::E_GX2SURFFMT>(record.hostFormat), texture.dim);
	s_surfaces.emplace(record.id, record);
	return record.id;
}

void SurfaceResolutionDiagnostics::UpdateSurfaceResolution(const LatteTexture& texture)
{
	std::scoped_lock lock{s_mutex};
	auto it = s_surfaces.find(texture.diagnosticSurfaceId);
	if (it == s_surfaces.end())
		return;
	auto& record = it->second;
	const auto& resolutionInfo = texture.GetResolutionInfo();
	record.requestedHostExtent = resolutionInfo.requestedHostExtent;
	record.hostExtent = resolutionInfo.hostExtent;
	record.scaleClass = resolutionInfo.scaleClass;
	record.scaleSource = resolutionInfo.source;
	record.fallbackReason = resolutionInfo.fallbackReason;
	record.graphicPackFixed = resolutionInfo.source == LatteSurfaceScaleSource::GraphicPackFixed;
	record.estimatedHostBytes = EstimateBytes(record.hostExtent.width, record.hostExtent.height,
		record.hostExtent.depth, record.mipLevels, static_cast<Latte::E_GX2SURFFMT>(record.hostFormat),
		texture.dim);
}

void SurfaceResolutionDiagnostics::UnregisterSurface(uint64 surfaceId)
{
	std::scoped_lock lock{s_mutex};
	if (auto it = s_surfaces.find(surfaceId); it != s_surfaces.end())
		it->second.active = false;
}

void SurfaceResolutionDiagnostics::RecordUsage(LatteTexture& texture, LatteSurfaceUsage usage)
{
	const uint32 bit = LatteSurfaceUsageBit(usage);
	if ((texture.surfaceUsageMask & bit) != 0)
		return;
	SPATIAL_PROFILER_AUTO_SCOPE_NAME("SurfaceScale.ResolvePolicy");
	texture.surfaceUsageMask |= bit;
	std::scoped_lock lock{s_mutex};
	auto it = s_surfaces.find(texture.diagnosticSurfaceId);
	if (it == s_surfaces.end())
		return;
	auto& surface = it->second;
	surface.usageMask |= bit;
	auto& firstUse = surface.firstUse[static_cast<size_t>(usage)];
	firstUse.sequence = s_nextUseSequence++;
	firstUse.frame = LatteGPUState.frameCounter;
	firstUse.draw = LatteGPUState.drawCallCounter;
	if (usage == LatteSurfaceUsage::CpuReadback || usage == LatteSurfaceUsage::LinearStaging)
		surface.cpuReadable = true;
}

void SurfaceResolutionDiagnostics::RecordUpload(LatteTexture& texture)
{
	RecordUsage(texture, LatteSurfaceUsage::GuestUpload);
	std::scoped_lock lock{s_mutex};
	if (auto it = s_surfaces.find(texture.diagnosticSurfaceId); it != s_surfaces.end())
		it->second.uploadBytes += it->second.estimatedGuestBytes;
}

void SurfaceResolutionDiagnostics::RecordEdge(const LatteTexture& first, const LatteTexture& second, LatteSurfaceEdgeType type, std::string reason)
{
	if (first.diagnosticSurfaceId == 0 || second.diagnosticSurfaceId == 0 || first.diagnosticSurfaceId == second.diagnosticSurfaceId)
		return;
	SPATIAL_PROFILER_AUTO_SCOPE_NAME("SurfaceScale.BuildFamily");
	std::scoped_lock lock{s_mutex};
	EdgeKey key{std::min(first.diagnosticSurfaceId, second.diagnosticSurfaceId), std::max(first.diagnosticSurfaceId, second.diagnosticSurfaceId), type};
	auto edgeIt = s_edgeIndices.find(key);
	if (edgeIt == s_edgeIndices.end())
	{
		s_edgeIndices.emplace(key, s_edges.size());
		s_edges.emplace_back(EdgeRecord{key, std::move(reason), 1});
		if (type != LatteSurfaceEdgeType::Conflict)
			UnionLocked(key.first, key.second);
		else if (s_edges.back().reason != "effective_scale_mismatch")
			s_aliasConflicts++;
	}
	else
	{
		s_edges[edgeIt->second].count++;
	}
}

void SurfaceResolutionDiagnostics::RecordCopy(const LatteTexture& source, const LatteTexture& destination, bool compatible, bool resolve)
{
	SPATIAL_PROFILER_AUTO_SCOPE_NAME("SurfaceScale.CopyBoundary");
	RecordEdge(source, destination, compatible ? (resolve ? LatteSurfaceEdgeType::Resolve : LatteSurfaceEdgeType::CompatibleCopy) : LatteSurfaceEdgeType::Conflict,
		compatible ? std::string{} : std::string{"effective_scale_mismatch"});
	std::scoped_lock lock{s_mutex};
	if (auto it = s_surfaces.find(source.diagnosticSurfaceId); it != s_surfaces.end())
		it->second.copySourceCount++;
	if (auto it = s_surfaces.find(destination.diagnosticSurfaceId); it != s_surfaces.end())
		it->second.copyDestinationCount++;
	if (!compatible)
		s_copyScaleConflicts++;
}

void SurfaceResolutionDiagnostics::RecordReadback(LatteTexture& texture, bool succeeded)
{
	SPATIAL_PROFILER_AUTO_SCOPE_NAME("SurfaceScale.ReadbackBoundary");
	RecordUsage(texture, LatteSurfaceUsage::CpuReadback);
	std::scoped_lock lock{s_mutex};
	s_readbacks++;
	s_resizedReadbacks += texture.HasHostResolutionOverride();
	s_readbackFailures += !succeeded;
	if (auto it = s_surfaces.find(texture.diagnosticSurfaceId); it != s_surfaces.end())
	{
		it->second.readbackCount++;
		it->second.readbackFailureCount += !succeeded;
	}
}

void SurfaceResolutionDiagnostics::RecordRepresentationAllocated(LatteTexture& texture,
	LatteTextureRepresentation representation, uint64 bytes)
{
	if (representation != LatteTextureRepresentation::GuestNative || texture.RepresentationsAlias() || bytes == 0)
		return;
	std::scoped_lock lock{s_mutex};
	s_nativeCompanionBytes += bytes;
	s_nativeCompanionPeakBytes = std::max(s_nativeCompanionPeakBytes, s_nativeCompanionBytes);
	s_companionCreates++;
	if (auto it = s_surfaces.find(texture.diagnosticSurfaceId); it != s_surfaces.end())
	{
		it->second.nativeCompanionBytes = bytes;
		it->second.companionCreateCount++;
	}
}

void SurfaceResolutionDiagnostics::RecordRepresentationReleased(LatteTexture& texture,
	LatteTextureRepresentation representation, uint64 bytes)
{
	if (representation != LatteTextureRepresentation::GuestNative || texture.RepresentationsAlias() || bytes == 0)
		return;
	std::scoped_lock lock{s_mutex};
	s_nativeCompanionBytes = bytes > s_nativeCompanionBytes ? 0 : s_nativeCompanionBytes - bytes;
	s_companionReleases++;
	if (auto it = s_surfaces.find(texture.diagnosticSurfaceId); it != s_surfaces.end())
	{
		it->second.nativeCompanionBytes = 0;
		it->second.companionReleaseCount++;
	}
}

void SurfaceResolutionDiagnostics::RecordRepresentationSync(LatteTexture& texture,
	LatteTextureRepresentation source, LatteTextureRepresentation destination,
	const LatteSurfaceSubresourceRange&, const LatteSurfaceOperationResult& result)
{
	std::scoped_lock lock{s_mutex};
	const bool upscale = source == LatteTextureRepresentation::GuestNative && destination == LatteTextureRepresentation::Render;
	if (result.succeeded)
	{
		if (upscale)
			s_representationUpscales++;
		else
			s_representationDownscales++;
	}
	else
	{
		s_representationSyncFailures++;
		RecordFailureLocked(false, texture, texture, source, destination, result.reason);
	}
	if (auto it = s_surfaces.find(texture.diagnosticSurfaceId); it != s_surfaces.end())
	{
		if (!result.succeeded)
			it->second.representationSyncFailureCount++;
		else if (upscale)
			it->second.upscaleCount++;
		else
			it->second.downscaleCount++;
	}
}

void SurfaceResolutionDiagnostics::RecordNativeBoundaryCopy(const LatteTexture& source, const LatteTexture& destination,
	const LatteSurfaceOperationResult& result)
{
	std::scoped_lock lock{s_mutex};
	s_nativeBoundaryCopies++;
	s_nativeBoundaryCopyFailures += !result.succeeded;
	if (!result.succeeded)
	{
		RecordFailureLocked(true, source, destination, LatteTextureRepresentation::GuestNative,
			LatteTextureRepresentation::GuestNative, result.reason);
	}
}

void SurfaceResolutionDiagnostics::RecordPresentSource(LatteTexture& texture, bool padView)
{
	RecordUsage(texture, LatteSurfaceUsage::PresentSource);
	std::scoped_lock lock{s_mutex};
	auto it = s_surfaces.find(texture.diagnosticSurfaceId);
	if (it == s_surfaces.end())
		return;
	PresentSourceSnapshot snapshot;
	snapshot.valid = true;
	snapshot.guestExtent = it->second.guestExtent;
	snapshot.hostExtent = it->second.hostExtent;
	snapshot.familyId = FindRootLocked(texture.diagnosticSurfaceId);
	snapshot.graphicPackFixed = it->second.graphicPackFixed;
	(padView ? s_padSource : s_tvSource) = snapshot;
}

SurfaceResolutionDiagnostics::PresentSourceSnapshot SurfaceResolutionDiagnostics::GetPresentSource(bool padView)
{
	std::scoped_lock lock{s_mutex};
	return padView ? s_padSource : s_tvSource;
}

void SurfaceResolutionDiagnostics::PublishProfilerCounters()
{
	const auto runtime = LatteSurfaceScaleState::GetSnapshot(
		GetConfig().render_surface_scale_percent.GetValue(),
		GetConfig().static_texture_scale_factor.GetValue());
	uint64 familiesNative{};
	uint64 familiesScaled{};
	uint64 familiesFallback{};
	uint64 activeSurfaces{};
	uint64 nativeBytes{};
	uint64 scaledBytes{};
	uint64 conflicts{};
	uint64 nativeCompanionBytes{};
	uint64 representationUpscales{};
	uint64 representationDownscales{};
	uint64 representationSyncFailures{};
	PresentSourceSnapshot tvSource;
	PresentSourceSnapshot padSource;
	{
		std::scoped_lock lock{s_mutex};
		struct FamilyState
		{
			bool scaled{};
			bool fallback{};
		};
		std::unordered_map<uint64, FamilyState> families;
		for (auto& [id, surface] : s_surfaces)
		{
			if (!surface.active)
				continue;
			activeSurfaces++;
			nativeBytes += surface.estimatedGuestBytes;
			if (surface.scaleSource == LatteSurfaceScaleSource::RenderSurfaceScale ||
				surface.scaleSource == LatteSurfaceScaleSource::StaticTextureScale)
				scaledBytes += surface.estimatedHostBytes;
			auto& family = families[FindRootLocked(id)];
			family.scaled |= surface.scaleSource == LatteSurfaceScaleSource::RenderSurfaceScale ||
				surface.scaleSource == LatteSurfaceScaleSource::StaticTextureScale;
			family.fallback |= surface.scaleSource == LatteSurfaceScaleSource::SafetyFallback;
		}
		for (const auto& [id, family] : families)
		{
			if (family.fallback)
				familiesFallback++;
			else if (family.scaled)
				familiesScaled++;
			else
				familiesNative++;
		}
		conflicts = s_copyScaleConflicts + s_aliasConflicts;
		nativeCompanionBytes = s_nativeCompanionBytes;
		representationUpscales = s_representationUpscales;
		representationDownscales = s_representationDownscales;
		representationSyncFailures = s_representationSyncFailures;
		tvSource = s_tvSource;
		padSource = s_padSource;
	}
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.configured_render_percent",
		runtime.configuredRenderScalePercent, "Cemu Surface Scale", "percent");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.active_render_percent",
		runtime.activeRenderScalePercent, "Cemu Surface Scale", "percent");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.configured_static_texture_factor",
		runtime.configuredStaticTextureScaleFactor, "Cemu Surface Scale", "factor");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.active_static_texture_factor",
		runtime.activeStaticTextureScaleFactor, "Cemu Surface Scale", "factor");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.pending_restart", runtime.pendingRestart, "Cemu Surface Scale", "state");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.families_native", familiesNative, "Cemu Surface Scale", "families");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.families_scaled", familiesScaled, "Cemu Surface Scale", "families");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.families_fallback", familiesFallback, "Cemu Surface Scale", "families");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.surfaces_active", activeSurfaces, "Cemu Surface Scale", "surfaces");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.native_bytes_estimated", nativeBytes, "Cemu Surface Scale", "bytes");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.scaled_bytes", scaledBytes, "Cemu Surface Scale", "bytes");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.conflicts", conflicts, "Cemu Surface Scale", "conflicts");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.native_companion_bytes", nativeCompanionBytes, "Cemu Surface Scale", "bytes");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.representation_upscales", representationUpscales, "Cemu Surface Scale", "operations");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.representation_downscales", representationDownscales, "Cemu Surface Scale", "operations");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.representation_sync_failures", representationSyncFailures, "Cemu Surface Scale", "failures");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.tv_guest_width", tvSource.guestExtent.width, "Cemu Surface Scale", "pixels");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.tv_guest_height", tvSource.guestExtent.height, "Cemu Surface Scale", "pixels");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.tv_host_width", tvSource.hostExtent.width, "Cemu Surface Scale", "pixels");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.tv_host_height", tvSource.hostExtent.height, "Cemu Surface Scale", "pixels");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.pad_guest_width", padSource.guestExtent.width, "Cemu Surface Scale", "pixels");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.pad_guest_height", padSource.guestExtent.height, "Cemu Surface Scale", "pixels");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.pad_host_width", padSource.hostExtent.width, "Cemu Surface Scale", "pixels");
	SPATIAL_PROFILER_COUNTER_SET("cemu.surface_scale.pad_host_height", padSource.hostExtent.height, "Cemu Surface Scale", "pixels");
}
