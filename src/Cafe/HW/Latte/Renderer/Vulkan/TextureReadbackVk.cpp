#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanProfiler.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanTextureReadback.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"

#include "spatial/profiler/Profiler.h"

LatteTextureReadbackInfoVk::LatteTextureReadbackInfoVk(VkDevice device, LatteTextureView* textureView,
	LatteTextureRepresentation representation)
	: LatteTextureReadbackInfo(textureView), m_device(device), m_representation(representation)
{
	m_image_size = GetImageSize(textureView, representation);
}

LatteTextureReadbackInfoVk::~LatteTextureReadbackInfoVk()
{
}

uint32 LatteTextureReadbackInfoVk::GetImageSize(LatteTextureView* textureView, LatteTextureRepresentation representation)
{
	const auto* baseTexture = (LatteTextureVk*)textureView->baseTexture;
	const auto extent = baseTexture->GetRepresentationExtent(representation, static_cast<uint32>(textureView->firstMip));
	// handle format
	const auto textureFormat = baseTexture->GetFormat();
	if (textureView->format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM)
	{
		cemu_assert(textureFormat == VK_FORMAT_R8G8B8A8_UNORM);
		return extent.width * extent.height * 4;
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R8_UNORM )
	{
		cemu_assert(textureFormat == VK_FORMAT_R8_UNORM);
		return extent.width * extent.height * 1;
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_SRGB)
	{
		cemu_assert(textureFormat == VK_FORMAT_R8G8B8A8_SRGB);
		return extent.width * extent.height * 4;
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R32_G32_B32_A32_FLOAT)
	{
		cemu_assert(textureFormat == VK_FORMAT_R32G32B32A32_SFLOAT);
		return extent.width * extent.height * 16;
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R32_FLOAT)
	{
		cemu_assert(textureFormat == VK_FORMAT_R32_SFLOAT || textureFormat == VK_FORMAT_D32_SFLOAT);
		if (baseTexture->isDepth)
			return extent.width * extent.height * 4;
		else
			return extent.width * extent.height * 4;
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R16_UNORM)
	{
		cemu_assert(textureFormat == VK_FORMAT_R16_UNORM);
		if (baseTexture->isDepth)
		{
			cemu_assert_debug(false);
			return extent.width * extent.height * 2;
		}
		else
		{
			return extent.width * extent.height * 2;
		}
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT)
	{
		cemu_assert(textureFormat == VK_FORMAT_R16G16B16A16_SFLOAT);
		return extent.width * extent.height * 8;
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R8_G8_UNORM)
	{
		cemu_assert(textureFormat == VK_FORMAT_R8G8_UNORM);
		return extent.width * extent.height * 2;
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R16_G16_B16_A16_UNORM)
	{
		cemu_assert(textureFormat == VK_FORMAT_R16G16B16A16_UNORM);
		return extent.width * extent.height * 8;
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::D24_S8_UNORM)
	{
		cemu_assert(textureFormat == VK_FORMAT_D24_UNORM_S8_UINT);
		// todo - if driver does not support VK_FORMAT_D24_UNORM_S8_UINT this is represented as VK_FORMAT_D32_SFLOAT_S8_UINT which is 8 bytes
		return extent.width * extent.height * 4;
	}
	else if (textureView->format == Latte::E_GX2SURFFMT::R5_G6_B5_UNORM )
	{
		if(textureFormat == VK_FORMAT_R5G6B5_UNORM_PACK16){
			return extent.width * extent.height * 2;
		}	
		return 0;
	}
	else
	{
		cemuLog_log(LogType::Force, "Unsupported texture readback format {:04x}", (uint32)textureView->format);
		cemu_assert_debug(false);
		return 0;
	}
}


void LatteTextureReadbackInfoVk::StartTransfer()
{
	cemu_assert(m_textureView);

	auto* baseTexture = (LatteTextureVk*)m_textureView->baseTexture;
	baseTexture->GetImageObj(m_representation)->flagForCurrentCommandBuffer();

	cemu_assert_debug(m_textureView->baseTexture->dim != Latte::E_DIM::DIM_3D);
	const uint32 mip = static_cast<uint32>(m_textureView->firstMip);
	const uint32 slice = static_cast<uint32>(m_textureView->firstSlice);
	const auto extent = baseTexture->GetRepresentationExtent(m_representation, mip);

	VkBufferImageCopy region{};
	region.bufferOffset = m_buffer_offset;
	region.bufferRowLength = extent.width;
	region.bufferImageHeight = extent.height;

	region.imageSubresource.aspectMask = baseTexture->GetImageAspect();
	region.imageSubresource.baseArrayLayer = slice;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.mipLevel = mip;

	region.imageOffset = {0,0,0};
	region.imageExtent = {extent.width, extent.height, 1};

	const auto renderer = VulkanRenderer::GetInstance();
	CEMU_VULKAN_GPU_PROFILE_SCOPE("vulkan.readback_copy", renderer->getCurrentCommandBuffer());
	renderer->draw_endRenderPass(LatteVulkanRenderPassEndReason::Readback);

	VkImageSubresourceRange imageRange{};
	imageRange.aspectMask = region.imageSubresource.aspectMask;
	imageRange.baseMipLevel = region.imageSubresource.mipLevel;
	imageRange.levelCount = 1;
	imageRange.baseArrayLayer = region.imageSubresource.baseArrayLayer;
	imageRange.layerCount = region.imageSubresource.layerCount;
	renderer->barrier_image<VulkanRenderer::ANY_TRANSFER | VulkanRenderer::IMAGE_WRITE, VulkanRenderer::TRANSFER_READ>(
		baseTexture->GetImageObj(m_representation)->m_image, imageRange, baseTexture->GetImageLayout(imageRange, m_representation),
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	baseTexture->SetImageLayout(imageRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_representation);

	renderer->barrier_sequentializeTransfer();

	vkCmdCopyImageToBuffer(renderer->getCurrentCommandBuffer(), baseTexture->GetImageObj(m_representation)->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_buffer, 1, &region);

	renderer->barrier_sequentializeTransfer();

	renderer->barrier_image<VulkanRenderer::TRANSFER_READ, VulkanRenderer::ANY_TRANSFER | VulkanRenderer::IMAGE_WRITE>(
		baseTexture->GetImageObj(m_representation)->m_image, imageRange, baseTexture->GetImageLayout(imageRange, m_representation),
		baseTexture->GetDefaultLayout(m_representation)); // make sure transfer is finished before image is modified
	baseTexture->SetImageLayout(imageRange, baseTexture->GetDefaultLayout(m_representation), m_representation);
	renderer->barrier_bufferRange<VulkanRenderer::TRANSFER_WRITE, VulkanRenderer::HOST_READ>(m_buffer, m_buffer_offset, m_image_size); // make sure transfer is finished before result is read

	m_associatedCommandBufferId = renderer->GetCurrentCommandBufferId();
	m_textureView = nullptr;

	// to decrease latency of readbacks make sure that the current command buffer is submitted soon
	renderer->RequestSubmitSoon(LatteVulkanSubmitReason::Readback);
	renderer->RequestSubmitOnIdle(LatteVulkanSubmitReason::Readback);
}

bool LatteTextureReadbackInfoVk::IsFinished()
{
	const auto renderer = VulkanRenderer::GetInstance();
	return renderer->HasCommandBufferFinished(m_associatedCommandBufferId);
}

void LatteTextureReadbackInfoVk::ForceFinish()
{
	SPATIAL_PROFILER_AUTO_SCOPE_NAME("vulkan.completion.wait.readback_visibility");
	const auto renderer = VulkanRenderer::GetInstance();
	renderer->WaitCommandBufferFinished(m_associatedCommandBufferId, LatteVulkanSubmitReason::Readback);
}
