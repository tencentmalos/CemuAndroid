#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "util/ChunkedHeap/ChunkedHeap.h"

#include "Cafe/HW/Latte/Renderer/Vulkan/VKRBase.h"

class LatteTextureVk : public LatteTexture
{
public:
	LatteTextureVk(class VulkanRenderer* vkRenderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
		uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth, LatteSurfaceUsage initialUsage);

	~LatteTextureVk();

	void AllocateOnHost() override;

	VKRObjectTexture* GetImageObj(LatteTextureRepresentation representation = LatteTextureRepresentation::Render) const
	{
		return representation == LatteTextureRepresentation::Render || RepresentationsAlias() ? vkObjTex : m_guestNativeObjTex;
	}
	bool HasRepresentation(LatteTextureRepresentation representation) const
	{
		return representation == LatteTextureRepresentation::Render || RepresentationsAlias() || m_guestNativeObjTex != nullptr;
	}
	LatteSurfaceOperationResult EnsureRepresentation(LatteTextureRepresentation representation);
	uint64 GetRepresentationBytes(LatteTextureRepresentation representation) const;
	
	VkFormat GetFormat() const { return vkObjTex->m_format; }
	VkImageAspectFlags GetImageAspect() const { return vkObjTex->m_imageAspect; }
	VkImageLayout GetDefaultLayout(LatteTextureRepresentation representation = LatteTextureRepresentation::Render) const
	{
		return representation == LatteTextureRepresentation::Render || RepresentationsAlias() ? m_defaultLayout : VK_IMAGE_LAYOUT_GENERAL;
	}

	VkImageLayout GetImageLayout(VkImageSubresource& subresource, LatteTextureRepresentation representation = LatteTextureRepresentation::Render)
	{
		auto& layouts = representation == LatteTextureRepresentation::Render || RepresentationsAlias() ? m_layouts : m_guestNativeLayouts;
		cemu_assert_debug(subresource.mipLevel < m_layoutsMips);
		cemu_assert_debug(subresource.arrayLayer < m_layoutsDepth);
		if (Is3DTexture())
			return layouts[subresource.mipLevel];
		return layouts[subresource.mipLevel * m_layoutsDepth + subresource.arrayLayer];
	}

	VkImageLayout GetImageLayout(VkImageSubresourceRange& subresource, LatteTextureRepresentation representation = LatteTextureRepresentation::Render)
	{
		auto& layouts = representation == LatteTextureRepresentation::Render || RepresentationsAlias() ? m_layouts : m_guestNativeLayouts;
		cemu_assert_debug(subresource.baseMipLevel < m_layoutsMips);
		cemu_assert_debug(subresource.baseArrayLayer < m_layoutsDepth);
		cemu_assert_debug(subresource.levelCount == 1);
		if (Is3DTexture())
			return layouts[subresource.baseMipLevel];
		cemu_assert_debug(subresource.layerCount > 0);
		if (subresource.layerCount > 1)
		{
			VkImageLayout imgLayout = layouts[subresource.baseMipLevel * m_layoutsDepth + subresource.baseArrayLayer];
			for (uint32 i = 1; i < subresource.layerCount; i++)
			{
				cemu_assert_debug(layouts[subresource.baseMipLevel * m_layoutsDepth + subresource.baseArrayLayer + i] == imgLayout);
			}
			return imgLayout;
		}
		return layouts[subresource.baseMipLevel * m_layoutsDepth + subresource.baseArrayLayer];
	}

	void SetImageLayout(VkImageSubresource& subresource, VkImageLayout newLayout, LatteTextureRepresentation representation = LatteTextureRepresentation::Render)
	{
		auto& layouts = representation == LatteTextureRepresentation::Render || RepresentationsAlias() ? m_layouts : m_guestNativeLayouts;
		cemu_assert_debug(subresource.mipLevel < m_layoutsMips);
		cemu_assert_debug(subresource.arrayLayer < m_layoutsDepth);
		if (Is3DTexture())
			layouts[subresource.mipLevel] = newLayout;
		else
			layouts[subresource.mipLevel * m_layoutsDepth + subresource.arrayLayer] = newLayout;
	}

	void SetImageLayout(VkImageSubresourceRange& subresource, VkImageLayout newLayout, LatteTextureRepresentation representation = LatteTextureRepresentation::Render)
	{
		auto& layouts = representation == LatteTextureRepresentation::Render || RepresentationsAlias() ? m_layouts : m_guestNativeLayouts;
		cemu_assert_debug(subresource.baseMipLevel < m_layoutsMips);
		cemu_assert_debug(subresource.baseArrayLayer < m_layoutsDepth);
		cemu_assert_debug(subresource.levelCount == 1);
		if (Is3DTexture())
			layouts[subresource.baseMipLevel] = newLayout;
		else
		{
			for(uint32 i=0; i<subresource.layerCount; i++)
				layouts[subresource.baseMipLevel * m_layoutsDepth + subresource.baseArrayLayer + i] = newLayout;
		}
	}

protected:
	LatteTextureView* CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount) override;

public:
	uint64 m_vkFlushIndex{}; // used to track read-write dependencies within the same renderpass

	uint64 m_vkFlushIndex_read{};
	uint64 m_vkFlushIndex_write{};
	uint32 m_selfDependencyCheckIndex{}; // used to track if texture is being both sampled and output to during drawcall
	VkImageAspectFlags m_selfDependencyCheckAspectMask{};


private:
	class VulkanRenderer* m_vkr;

	VKRObjectTexture* vkObjTex{};
	VKRObjectTexture* m_guestNativeObjTex{};
	VkImageLayout m_defaultLayout{ VK_IMAGE_LAYOUT_GENERAL }; // the targetted long term layout of the texture. Can be either VK_IMAGE_LAYOUT_GENERAL or VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT for potential rendertargets if supported
	std::vector<VkImageLayout> m_layouts;
	std::vector<VkImageLayout> m_guestNativeLayouts;
	uint32 m_layoutsMips;
	uint32 m_layoutsDepth;

	VKRObjectTexture* CreateRepresentationImage(const LatteSurfaceExtent& extent, bool recoverable,
		bool primary = false);
};
