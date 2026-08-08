#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureViewVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/Diagnostics/SurfaceResolutionDiagnostics.h"

LatteTextureVk::LatteTextureVk(class VulkanRenderer* vkRenderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle,
	Latte::E_HWTILEMODE tileMode, bool isDepth, LatteSurfaceUsage initialUsage)
	: LatteTexture(dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth, initialUsage), m_vkr(vkRenderer)
{
	vkObjTex = new VKRObjectTexture();

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	
	const auto hostExtent = GetHostExtent();
	sint32 effectiveBaseWidth = static_cast<sint32>(hostExtent.width);
	sint32 effectiveBaseHeight = static_cast<sint32>(hostExtent.height);
	sint32 effectiveBaseDepth = static_cast<sint32>(hostExtent.depth);
	effectiveBaseDepth = std::max(1, effectiveBaseDepth);

	imageInfo.extent.width = effectiveBaseWidth;
	imageInfo.extent.height = effectiveBaseHeight;
	imageInfo.mipLevels = mipLevels;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

	if (dim == Latte::E_DIM::DIM_3D)
	{
		imageInfo.extent.depth = effectiveBaseDepth;
		imageInfo.arrayLayers = 1;
		imageInfo.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
	}
	else
	{
		imageInfo.extent.depth = 1;
		imageInfo.arrayLayers = effectiveBaseDepth;
		if (dim != Latte::E_DIM::DIM_1D && (effectiveBaseDepth % 6) == 0 && effectiveBaseWidth == effectiveBaseHeight)
			imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	}
	
	VulkanRenderer::FormatInfoVK texFormatInfo;
	vkRenderer->GetTextureFormatInfoVK(format, isDepth, dim, effectiveBaseWidth, effectiveBaseHeight, &texFormatInfo);
	cemu_assert_debug(hasStencil == ((texFormatInfo.vkImageAspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0));
	imageInfo.format = texFormatInfo.vkImageFormat;
	vkObjTex->m_imageAspect = texFormatInfo.vkImageAspect;
	
	if (isDepth == false && texFormatInfo.isCompressed)
	{
		imageInfo.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
	}
	if (isDepth == false)
		imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

	if (isDepth)
	{
		imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	}
	else
	{
		if(Latte::IsCompressedFormat(format) == false && texFormatInfo.vkImageFormat != VK_FORMAT_R4G4_UNORM_PACK8) // Vulkan's R4G4 cant be used as a color attachment
			imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}

	if (m_vkr->UseAttachmentFeedbackLoop() && (imageInfo.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0)
	{
		imageInfo.usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
		m_defaultLayout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
	}

	if (dim == Latte::E_DIM::DIM_2D)
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
	else if (dim == Latte::E_DIM::DIM_1D)
		imageInfo.imageType = VK_IMAGE_TYPE_1D;
	else if (dim == Latte::E_DIM::DIM_3D)
		imageInfo.imageType = VK_IMAGE_TYPE_3D;
	else if (dim == Latte::E_DIM::DIM_2D_ARRAY)
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
	else if (dim == Latte::E_DIM::DIM_CUBEMAP)
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
	else if (dim == Latte::E_DIM::DIM_2D_MSAA)
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
	else
	{
		cemu_assert_unimplemented();
	}

	if (vkCreateImage(m_vkr->GetLogicalDevice(), &imageInfo, nullptr, &vkObjTex->m_image) != VK_SUCCESS)
		m_vkr->UnrecoverableError("Failed to create texture image");
	
	if (m_vkr->IsDebugMarkersEnabled())
	{
		VkDebugUtilsObjectNameInfoEXT objName{};
		objName.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
		objName.objectType = VK_OBJECT_TYPE_IMAGE;
		objName.pNext = nullptr;
		objName.objectHandle = (uint64_t)vkObjTex->m_image;
		auto objNameStr = fmt::format("tex_{:08x}_fmt{:04x}_tm{:x}", physAddress, (uint32)format, (uint32)tileMode);
		objName.pObjectName = objNameStr.c_str();
		vkSetDebugUtilsObjectNameEXT(m_vkr->GetLogicalDevice(), &objName);
	}

	vkObjTex->m_flags = imageInfo.flags;
	vkObjTex->m_format = imageInfo.format;

	// init layout array
	m_layoutsMips = std::max(mipLevels, 1u); // todo - use effective mip count
	m_layoutsDepth = std::max(depth, 1u);
	if (Is3DTexture())
		m_layouts.resize(m_layoutsMips, VK_IMAGE_LAYOUT_UNDEFINED); // one per mip
	else
		m_layouts.resize(m_layoutsMips * m_layoutsDepth, VK_IMAGE_LAYOUT_UNDEFINED); // one per layer per mip
}

LatteTextureVk::~LatteTextureVk()
{
	cemu_assert_debug(views.empty());

	m_vkr->surfaceCopy_notifyTextureRelease(this);

	VulkanRenderer::GetInstance()->ReleaseDestructibleObject(vkObjTex);
	vkObjTex = nullptr;
	if (m_guestNativeObjTex)
	{
		SurfaceResolutionDiagnostics::RecordRepresentationReleased(*this, LatteTextureRepresentation::GuestNative,
			GetRepresentationBytes(LatteTextureRepresentation::GuestNative));
		VulkanRenderer::GetInstance()->ReleaseDestructibleObject(m_guestNativeObjTex);
		m_guestNativeObjTex = nullptr;
	}
}

VKRObjectTexture* LatteTextureVk::CreateRepresentationImage(const LatteSurfaceExtent& extent, bool recoverable,
	bool primary)
{
	auto* object = new VKRObjectTexture();
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.extent.width = std::max<uint32>(1, extent.width);
	imageInfo.extent.height = std::max<uint32>(1, extent.height);
	imageInfo.mipLevels = std::max(mipLevels, 1);
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

	const uint32 representationDepth = std::max<uint32>(1, extent.depth);
	if (dim == Latte::E_DIM::DIM_3D)
	{
		imageInfo.extent.depth = representationDepth;
		imageInfo.arrayLayers = 1;
		imageInfo.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
	}
	else
	{
		imageInfo.extent.depth = 1;
		imageInfo.arrayLayers = representationDepth;
		if (dim != Latte::E_DIM::DIM_1D && (representationDepth % 6) == 0 && imageInfo.extent.width == imageInfo.extent.height)
			imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	}

	VulkanRenderer::FormatInfoVK formatInfo;
	m_vkr->GetTextureFormatInfoVK(format, isDepth, dim, imageInfo.extent.width, imageInfo.extent.height, &formatInfo);
	imageInfo.format = formatInfo.vkImageFormat;
	object->m_imageAspect = formatInfo.vkImageAspect;
	if (!isDepth && formatInfo.isCompressed)
		imageInfo.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
	if (!isDepth)
		imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	if (isDepth)
		imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	else if (!Latte::IsCompressedFormat(format) && formatInfo.vkImageFormat != VK_FORMAT_R4G4_UNORM_PACK8)
		imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (primary && m_vkr->UseAttachmentFeedbackLoop() &&
		(imageInfo.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0)
	{
		imageInfo.usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
		m_defaultLayout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
	}

	if (dim == Latte::E_DIM::DIM_1D)
		imageInfo.imageType = VK_IMAGE_TYPE_1D;
	else if (dim == Latte::E_DIM::DIM_3D)
		imageInfo.imageType = VK_IMAGE_TYPE_3D;
	else
		imageInfo.imageType = VK_IMAGE_TYPE_2D;

	if (vkCreateImage(m_vkr->GetLogicalDevice(), &imageInfo, nullptr, &object->m_image) != VK_SUCCESS)
	{
		delete object;
		if (!recoverable)
			m_vkr->UnrecoverableError("Failed to create texture representation image");
		return nullptr;
	}
	object->m_flags = imageInfo.flags;
	object->m_format = imageInfo.format;
	object->m_allocation = m_vkr->GetMemoryManager()->imageMemoryAllocate(object->m_image, recoverable);
	if (!object->m_allocation)
	{
		delete object;
		return nullptr;
	}
	return object;
}

LatteSurfaceOperationResult LatteTextureVk::EnsureRepresentation(LatteTextureRepresentation representation)
{
	if (representation == LatteTextureRepresentation::Render || RepresentationsAlias())
		return LatteSurfaceOperationResult::Success(GetRepresentationBytes(LatteTextureRepresentation::Render));
	if (m_guestNativeObjTex)
		return LatteSurfaceOperationResult::Success(GetRepresentationBytes(representation));
	m_guestNativeObjTex = CreateRepresentationImage(GetGuestExtent(), true);
	if (!m_guestNativeObjTex)
		return LatteSurfaceOperationResult::Failure(LatteSurfaceFallbackReason::AllocationFailed);
	if (Is3DTexture())
		m_guestNativeLayouts.assign(m_layoutsMips, VK_IMAGE_LAYOUT_UNDEFINED);
	else
		m_guestNativeLayouts.assign(m_layoutsMips * m_layoutsDepth, VK_IMAGE_LAYOUT_UNDEFINED);
	return LatteSurfaceOperationResult::Success(GetRepresentationBytes(representation));
}

uint64 LatteTextureVk::GetRepresentationBytes(LatteTextureRepresentation representation) const
{
	auto* object = GetImageObj(representation);
	if (!object || !object->m_allocation)
		return 0;
	return object->m_allocation->getAllocationSize();
}

LatteTextureView* LatteTextureVk::CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
{
	cemu_assert_debug(mipCount > 0);
	cemu_assert_debug(sliceCount > 0);
	cemu_assert_debug((firstMip + mipCount) <= this->mipLevels);
	cemu_assert_debug((firstSlice + sliceCount) <= this->depth);
	return new LatteTextureViewVk(m_vkr->GetLogicalDevice(), this, dim, format, firstMip, mipCount, firstSlice, sliceCount);
}

void LatteTextureVk::AllocateOnHost()
{
	const bool recoverable = HasHostResolutionOverride();
	auto allocationInfo = VulkanRenderer::GetInstance()->GetMemoryManager()->imageMemoryAllocate(
		GetImageObj()->m_image, recoverable);
	if (allocationInfo)
	{
		vkObjTex->m_allocation = allocationInfo;
		return;
	}
	if (!recoverable)
		m_vkr->UnrecoverableError("Failed to allocate native texture image");
	delete vkObjTex;
	vkObjTex = nullptr;
	ApplyResolutionFallback(LatteSurfaceFallbackReason::AllocationFailed);
	vkObjTex = CreateRepresentationImage(GetGuestExtent(), true, true);
	if (!vkObjTex)
		m_vkr->UnrecoverableError("Failed to allocate native texture image after scaled fallback");
}
