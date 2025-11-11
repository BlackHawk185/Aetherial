// GPUMeshQueue.h - Main-thread mesh generation queue for OpenGL
// Replaces multi-threaded AsyncMeshGenerator with single-threaded queue
// that respects OpenGL's threading restrictions
#pragma once

#include <queue>
#include <memory>
#include <functional>
#include "../World/VoxelChunk.h"

// Types of mesh work
enum class MeshWorkType
{
    FullChunk,      // Generate mesh for entire chunk (initial load)
    SingleRegion,   // Generate mesh for one region (block place/break)
};

// Mesh generation work item
struct MeshWorkItem
{
    MeshWorkType type;
    VoxelChunk* chunk;
    int regionIndex;  // Only used for SingleRegion work
    std::function<void()> onComplete;  // Optional callback after GPU upload
};

class GPUMeshQueue
{
public:
    GPUMeshQueue();
    ~GPUMeshQueue();
    
    // Queue full chunk mesh generation (for newly loaded chunks)
    void queueFullChunkMesh(VoxelChunk* chunk, std::function<void()> onComplete = nullptr);
    
    // Queue single region mesh generation (for block edits)
    void queueRegionMesh(VoxelChunk* chunk, int regionIndex, std::function<void()> onComplete = nullptr);
    
    // Process N work items per frame on main thread (call from game loop)
    // Returns number of items processed
    int processQueue(int maxItemsPerFrame = 4);
    
    // Check if there are pending work items
    bool hasPendingWork() const { return !m_workQueue.empty(); }
    
    // Get number of pending work items
    size_t getPendingWorkCount() const { return m_workQueue.size(); }
    
    // Clear all pending work (useful for cleanup)
    void clear();

private:
    std::queue<MeshWorkItem> m_workQueue;
    
    // Process a single work item
    void processWorkItem(const MeshWorkItem& item);
};

// Global instance
extern std::unique_ptr<GPUMeshQueue> g_gpuMeshQueue;
