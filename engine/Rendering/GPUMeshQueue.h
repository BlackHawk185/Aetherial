// GPUMeshQueue.h - Multi-threaded region mesh generation queue
// Worker threads generate mesh data, main thread uploads to GPU
#pragma once

#include <queue>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include "../World/VoxelChunk.h"

// Region mesh generation request
struct RegionMeshRequest
{
    VoxelChunk* chunk;
    int regionIndex;
};

// Region mesh generation result
struct RegionMeshResult
{
    VoxelChunk* chunk;
    int regionIndex;
    std::vector<QuadFace> quads;
};

class GreedyMeshQueue
{
public:
    GreedyMeshQueue();
    ~GreedyMeshQueue();
    
    // Queue full chunk mesh generation (queues all regions)
    void queueFullChunkMesh(VoxelChunk* chunk);
    
    // Queue single region mesh generation (for block edits)
    void queueRegionMesh(VoxelChunk* chunk, int regionIndex);
    
    // Process completed meshes and upload to GPU (call from main thread)
    // Returns number of regions uploaded
    int processQueue(int maxItemsPerFrame = 16);
    
    // Check if there are pending work items
    bool hasPendingWork() const;
    
    // Get number of pending work items
    size_t getPendingWorkCount() const;
    
    // Clear all pending work (useful for cleanup)
    void clear();

private:
    // Worker thread pool
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_shutdownFlag{false};
    
    // Job queues with synchronization
    std::queue<RegionMeshRequest> m_jobQueue;
    mutable std::mutex m_jobQueueMutex;
    std::condition_variable m_jobQueueCV;
    
    std::queue<RegionMeshResult> m_completedQueue;
    mutable std::mutex m_completedQueueMutex;
    
    // Worker thread function
    void workerThreadFunc();
    
    // Upload completed region mesh to GPU (main thread only)
    void uploadRegionMesh(const RegionMeshResult& result);
};

// Global instance
extern std::unique_ptr<GreedyMeshQueue> g_greedyMeshQueue;
