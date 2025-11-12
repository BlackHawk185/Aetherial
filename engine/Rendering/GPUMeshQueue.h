// GPUMeshQueue.h - Multi-threaded chunk mesh generation queue
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
#include <unordered_set>
#include "../World/VoxelChunk.h"

// Chunk mesh generation result
struct ChunkMeshResult
{
    VoxelChunk* chunk;
    std::vector<QuadFace> quads;
};

class GreedyMeshQueue
{
public:
    GreedyMeshQueue();
    ~GreedyMeshQueue();
    
    // Queue chunk for meshing
    void queueChunkMesh(VoxelChunk* chunk);
    
    // Process completed meshes and upload to GPU (call from main thread)
    // Returns number of chunks uploaded
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
    
    std::unordered_set<VoxelChunk*> m_jobQueue;
    mutable std::mutex m_jobQueueMutex;
    std::condition_variable m_jobQueueCV;
    
    std::queue<ChunkMeshResult> m_completedQueue;
    mutable std::mutex m_completedQueueMutex;
    
    // Worker thread function
    void workerThreadFunc();
    
    // Upload completed chunk mesh to GPU (main thread only)
    void uploadChunkMesh(const ChunkMeshResult& result);
};

// Global instance
extern std::unique_ptr<GreedyMeshQueue> g_greedyMeshQueue;
