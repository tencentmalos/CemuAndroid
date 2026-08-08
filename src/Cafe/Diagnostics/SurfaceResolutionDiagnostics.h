#pragma once

#include "Cafe/HW/Latte/Core/LatteSurfaceScale.h"

#include <string>

class LatteTexture;

namespace spatial::debugbus
{
class DebugCommandRegistry;
}

namespace SurfaceResolutionDiagnostics
{
	struct PresentSourceSnapshot
	{
		bool valid{};
		LatteSurfaceExtent guestExtent{};
		LatteSurfaceExtent hostExtent{};
		uint64 familyId{};
		bool graphicPackFixed{};
	};

	void RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry);
	void BeginTitle();
	void EndTitle();

	uint64 RegisterSurface(const LatteTexture& texture);
	void UpdateSurfaceResolution(const LatteTexture& texture);
	void UnregisterSurface(uint64 surfaceId);
	void RecordUsage(LatteTexture& texture, LatteSurfaceUsage usage);
	void RecordUpload(LatteTexture& texture);
	void RecordEdge(const LatteTexture& first, const LatteTexture& second, LatteSurfaceEdgeType type, std::string reason = {});
	void RecordCopy(const LatteTexture& source, const LatteTexture& destination, bool compatible, bool resolve);
	void RecordReadback(LatteTexture& texture, bool succeeded);
	void RecordPresentSource(LatteTexture& texture, bool padView);
	void RecordRepresentationAllocated(LatteTexture& texture, LatteTextureRepresentation representation, uint64 bytes);
	void RecordRepresentationReleased(LatteTexture& texture, LatteTextureRepresentation representation, uint64 bytes);
	void RecordRepresentationSync(LatteTexture& texture, LatteTextureRepresentation source,
		LatteTextureRepresentation destination, const LatteSurfaceSubresourceRange& range,
		const LatteSurfaceOperationResult& result);
	void RecordNativeBoundaryCopy(const LatteTexture& source, const LatteTexture& destination,
		const LatteSurfaceOperationResult& result);

	PresentSourceSnapshot GetPresentSource(bool padView);
	void PublishProfilerCounters();
}
