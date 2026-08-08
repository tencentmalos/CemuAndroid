#pragma once

#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"

LatteBufferCacheUploadSource LatteBufferCache_setUploadSource(
	LatteBufferCacheUploadSource source);

class LatteBufferCacheUploadSourceScope
{
  public:
	explicit LatteBufferCacheUploadSourceScope(LatteBufferCacheUploadSource source)
		: m_previousSource(LatteBufferCache_setUploadSource(source))
	{
	}

	~LatteBufferCacheUploadSourceScope()
	{
		LatteBufferCache_setUploadSource(m_previousSource);
	}

	LatteBufferCacheUploadSourceScope(const LatteBufferCacheUploadSourceScope&) = delete;
	LatteBufferCacheUploadSourceScope& operator=(const LatteBufferCacheUploadSourceScope&) = delete;

  private:
	LatteBufferCacheUploadSource m_previousSource;
};

void LatteBufferCache_init(size_t bufferSize);
void LatteBufferCache_UnloadAll();

uint32 LatteBufferCache_retrieveDataInCache(MPTR physAddress, uint32 size);
void LatteBufferCache_copyStreamoutDataToCache(MPTR physAddress, uint32 size, uint32 streamoutBufferOffset);
void LatteBufferCache_invalidate(MPTR physAddress, uint32 size);

void LatteBufferCache_notifyDCFlush(MPTR address, uint32 size);
void LatteBufferCache_processDCFlushQueue();

void LatteBufferCache_processDeallocations();
void LatteBufferCache_incrementalCleanup();

void LatteBufferCache_getStats(uint32& heapSize, uint32& allocationSize, uint32& allocNum);

void LatteBufferCache_notifySwapTVScanBuffer();
