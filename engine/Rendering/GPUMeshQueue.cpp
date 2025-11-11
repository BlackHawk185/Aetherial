// GPUMeshQueue.cpp - Main-thread mesh generation queue implementation
#include "GPUMeshQueue.h"
#include "../Profiling/Profiler.h"
#include <iostream>

// Global instance
std::unique_ptr<GPUMeshQueue> g_gpuMeshQueue = nullptr;

GPUMeshQueue::GPUMeshQueue()
{
    std::cout << "[GPU MESH QUEUE] Initialized main-thread mesh queue" << std::endl;
}

GPUMeshQueue::~GPUMeshQueue()
{
    clear();
}

void GPUMeshQueue::queueFullChunkMesh(VoxelChunk* chunk, std::function<void()> onComplete)
{
    if (!chunk) return;
    
    m_workQueue.push({
        MeshWorkType::FullChunk,
        chunk,
        -1,  // No region index for full chunk
        onComplete
    });
}

void GPUMeshQueue::queueRegionMesh(VoxelChunk* chunk, int regionIndex, std::function<void()> onComplete)
{
    if (!chunk) return;
    
    m_workQueue.push({
        MeshWorkType::SingleRegion,
        chunk,
        regionIndex,
        onComplete
    });
}

int GPUMeshQueue::processQueue(int maxItemsPerFrame)
{
    PROFILE_SCOPE("GPUMeshQueue::processQueue");
    
    int itemsProcessed = 0;
    
    while (!m_workQueue.empty() && itemsProcessed < maxItemsPerFrame)
    {
        MeshWorkItem item = m_workQueue.front();
        m_workQueue.pop();
        
        processWorkItem(item);
        
        // Call completion callback if provided
        if (item.onComplete) {
            item.onComplete();
        }
        
        itemsProcessed++;
    }
    
    return itemsProcessed;
}

void GPUMeshQueue::processWorkItem(const MeshWorkItem& item)
{
    PROFILE_SCOPE("GPUMeshQueue::processWorkItem");
    
    if (!item.chunk) return;
    
    switch (item.type)
    {
        case MeshWorkType::FullChunk:
        {
            // Generate mesh for entire chunk
            item.chunk->generateMesh(false);  // false = no lighting (real-time lighting used)
            break;
        }
        
        case MeshWorkType::SingleRegion:
        {
            // Generate mesh for specific region only (optimized partial update)
            item.chunk->generateMeshForRegion(item.regionIndex);
            break;
        }
    }
    
    // Trigger mesh update callback AFTER mesh generation (prevents 1-frame lag)
    item.chunk->triggerMeshUpdateCallback();
}

void GPUMeshQueue::clear()
{
    while (!m_workQueue.empty()) {
        m_workQueue.pop();
    }
}
