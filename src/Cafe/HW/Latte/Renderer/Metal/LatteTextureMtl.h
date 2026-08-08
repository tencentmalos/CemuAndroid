#pragma once

#include <Metal/Metal.hpp>

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "HW/Latte/ISA/LatteReg.h"
#include "util/ChunkedHeap/ChunkedHeap.h"

class LatteTextureMtl : public LatteTexture
{
public:
	LatteTextureMtl(class MetalRenderer* mtlRenderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
		uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth, LatteSurfaceUsage initialUsage);
	~LatteTextureMtl();

	MTL::Texture* GetTexture(LatteTextureRepresentation representation = LatteTextureRepresentation::Render) const {
	    return representation == LatteTextureRepresentation::Render || RepresentationsAlias() ? m_texture : m_guestNativeTexture;
	}
	bool HasRepresentation(LatteTextureRepresentation representation) const
	{
		return representation == LatteTextureRepresentation::Render || RepresentationsAlias() || m_guestNativeTexture != nullptr;
	}
	LatteSurfaceOperationResult EnsureRepresentation(LatteTextureRepresentation representation);
	uint64 GetRepresentationBytes(LatteTextureRepresentation representation) const;

	void AllocateOnHost() override;

protected:
	LatteTextureView* CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount) override;

private:
	class MetalRenderer* m_mtlr;

	MTL::Texture* m_texture;
	MTL::Texture* m_guestNativeTexture{};

	MTL::Texture* CreateRepresentationTexture(const LatteSurfaceExtent& extent);
};
