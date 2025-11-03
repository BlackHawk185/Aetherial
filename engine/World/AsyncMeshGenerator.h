#pragma once

#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <functional>
#include "VoxelChunk.h"

// Async mesh generation system to prevent main thread stalling
// when receiving chunks from network
class AsyncMeshGenerator
{
public:
    AsyncMeshGenerator();
    ~AsyncMeshGenerator();

    // Queue a chunk for mesh generation on background thread
    void queueChunkMeshGeneration(VoxelChunk* chunk, std::function<void()> onComplete = nullptr);

    // Process completed meshes on main thread (fast - just swaps data)
    void processCompletedMeshes();

    // Check if there are pending jobs
    bool hasPendingJobs() const;

    // Shutdown the worker threads
    void shutdown();

private:
    struct MeshJob
    {
        VoxelChunk* chunk;
        std::function<void()> onComplete;
    };

    struct CompletedMesh
    {
        VoxelChunk* chunk;
        std::shared_ptr<VoxelMesh> renderMesh;
        std::shared_ptr<CollisionMesh> collisionMesh;
        std::unordered_map<uint8_t, std::vector<Vec3>> modelInstances;
        std::function<void()> onComplete;
    };

    void workerThreadFunc();

    std::vector<std::thread> m_workers;
    std::queue<MeshJob> m_jobQueue;
    std::queue<CompletedMesh> m_completedQueue;
    
    std::mutex m_jobMutex;
    std::mutex m_completedMutex;
    std::condition_variable m_jobCondition;
    
    std::atomic<bool> m_running;
    std::atomic<int> m_pendingJobs;
};

// Global async mesh generator
extern AsyncMeshGenerator* g_asyncMeshGenerator;
