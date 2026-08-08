#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureReadbackMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"

LatteTextureReadbackInfoMtl::~LatteTextureReadbackInfoMtl()
{
    if (m_commandBuffer)
        m_commandBuffer->release();
}

void LatteTextureReadbackInfoMtl::StartTransfer()
{
	cemu_assert(m_textureView);

	auto* baseTexture = (LatteTextureMtl*)m_textureView->baseTexture;

	cemu_assert_debug(m_textureView->baseTexture->dim != Latte::E_DIM::DIM_3D);
	const uint32 mip = static_cast<uint32>(m_textureView->firstMip);
	const uint32 slice = static_cast<uint32>(m_textureView->firstSlice);
	const auto extent = baseTexture->GetRepresentationExtent(m_representation, mip);

	size_t bytesPerRow = GetMtlTextureBytesPerRow(baseTexture->format, baseTexture->isDepth, extent.width);
	size_t bytesPerImage = GetMtlTextureBytesPerImage(baseTexture->format, baseTexture->isDepth, extent.height, bytesPerRow);

	auto blitCommandEncoder = m_mtlr->GetBlitCommandEncoder();

	blitCommandEncoder->copyFromTexture(baseTexture->GetTexture(m_representation), slice, mip, MTL::Origin{0, 0, 0},
		MTL::Size{extent.width, extent.height, 1}, m_mtlr->GetTextureReadbackBuffer(), m_bufferOffset, bytesPerRow, bytesPerImage);

	m_commandBuffer = m_mtlr->GetCurrentCommandBuffer()->retain();
	// TODO: uncomment?
	//m_mtlr->RequestSoonCommit();
	m_mtlr->CommitCommandBuffer();
}

bool LatteTextureReadbackInfoMtl::IsFinished()
{
    // Command buffer wasn't even comitted, let's commit immediately
    //if (m_mtlr->GetCurrentCommandBuffer() == m_commandBuffer)
    //    m_mtlr->CommitCommandBuffer();

    return CommandBufferCompleted(m_commandBuffer);
}

void LatteTextureReadbackInfoMtl::ForceFinish()
{
    m_commandBuffer->waitUntilCompleted();
}

uint8* LatteTextureReadbackInfoMtl::GetData()
{
	return (uint8*)m_mtlr->GetTextureReadbackBuffer()->contents() + m_bufferOffset;
}
