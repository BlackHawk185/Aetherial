// GreedyMeshQueue.cpp - Multi-threaded region mesh generation
#include "GPUMeshQueue.h"
#include "../Profiling/Profiler.h"
#include <iostream>

// Global instance
std::unique_ptr<GreedyMeshQueue> g_greedyMeshQueue = nullptr;

GreedyMeshQueue::GreedyMeshQueue()
{
    // Spawn worker threads (leave 2 cores for main thread and other work)
    int numThreads = std::max(2, static_cast<int>(std::thread::hardware_concurrency()) - 2);
    
    std::cout << "[GREEDY MESH] Starting " << numThreads << " worker threads for region meshing" << std::endl;
    
    for (int i = 0; i < numThreads; ++i) {
        m_workers.emplace_back(&GreedyMeshQueue::workerThreadFunc, this);
    }
}

GreedyMeshQueue::~GreedyMeshQueue()
{
    // Signal shutdown
    m_shutdownFlag = true;
    m_jobQueueCV.notify_all();
    
    // Join all threads
    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    
    std::cout << "[GREEDY MESH] Shut down " << m_workers.size() << " worker threads" << std::endl;
}

void GreedyMeshQueue::queueFullChunkMesh(VoxelChunk* chunk)
{
    if (!chunk) return;
    
    // Queue all regions for parallel processing
    {
        std::lock_guard<std::mutex> lock(m_jobQueueMutex);
        for (int i = 0; i < ChunkConfig::TOTAL_REGIONS; ++i) {
            m_jobQueue.push({chunk, i});
        }
    }
    
    // Wake up all worker threads
    m_jobQueueCV.notify_all();
}

void GreedyMeshQueue::queueRegionMesh(VoxelChunk* chunk, int regionIndex)
{
    if (!chunk) return;
    
    {
        std::lock_guard<std::mutex> lock(m_jobQueueMutex);
        m_jobQueue.push({chunk, regionIndex});
    }
    
    m_jobQueueCV.notify_one();
}

int GreedyMeshQueue::processQueue(int maxItemsPerFrame)
{
    PROFILE_SCOPE("GreedyMeshQueue::processQueue");
    
    int itemsProcessed = 0;
    
    // Upload completed region meshes to GPU
    while (itemsProcessed < maxItemsPerFrame)
    {
        RegionMeshResult result;
        
        {
            std::lock_guard<std::mutex> lock(m_completedQueueMutex);
            if (m_completedQueue.empty()) break;
            
            result = std::move(m_completedQueue.front());
            m_completedQueue.pop();
        }
        
        uploadRegionMesh(result);
        
        itemsProcessed++;
    }
    
    return itemsProcessed;
}

void GreedyMeshQueue::workerThreadFunc()
{
    while (true)
    {
        RegionMeshRequest job;
        
        // Wait for work
        {
            std::unique_lock<std::mutex> lock(m_jobQueueMutex);
            m_jobQueueCV.wait(lock, [this] { return m_shutdownFlag || !m_jobQueue.empty(); });
            
            if (m_shutdownFlag) return;
            
            if (m_jobQueue.empty()) continue;
            
            job = std::move(m_jobQueue.front());
            m_jobQueue.pop();
        }
        
        // Generate mesh for this region (CPU only, no GPU calls)
        RegionMeshResult result;
        result.chunk = job.chunk;
        result.regionIndex = job.regionIndex;
        
        // Call the region meshing function (returns quads in local buffer - thread-safe)
        result.quads = job.chunk->generateMeshForRegion(job.regionIndex);
        
        // Only push non-empty regions to completed queue (skip empty regions entirely)
        if (!result.quads.empty())
        {
            std::lock_guard<std::mutex> lock(m_completedQueueMutex);
            m_completedQueue.push(std::move(result));
        }
    }
}

void GreedyMeshQueue::uploadRegionMesh(const RegionMeshResult& result)
{
    PROFILE_SCOPE("GreedyMeshQueue::uploadRegionMesh");
    
    if (!result.chunk || !result.chunk->isClient()) return;
    
    // Get or create render mesh
    auto mesh = result.chunk->getRenderMesh();
    if (!mesh) {
        mesh = std::make_shared<VoxelMesh>();
        result.chunk->setRenderMesh(mesh);
    }
    
    // Calculate region boundaries for removal
    int rx = result.regionIndex % ChunkConfig::REGIONS_PER_AXIS;
    int ry = (result.regionIndex / ChunkConfig::REGIONS_PER_AXIS) % ChunkConfig::REGIONS_PER_AXIS;
    int rz = result.regionIndex / (ChunkConfig::REGIONS_PER_AXIS * ChunkConfig::REGIONS_PER_AXIS);
    
    int minX = rx * ChunkConfig::REGION_SIZE;
    int minY = ry * ChunkConfig::REGION_SIZE;
    int minZ = rz * ChunkConfig::REGION_SIZE;
    
    int maxX = std::min(minX + ChunkConfig::REGION_SIZE, VoxelChunk::SIZE);
    int maxY = std::min(minY + ChunkConfig::REGION_SIZE, VoxelChunk::SIZE);
    int maxZ = std::min(minZ + ChunkConfig::REGION_SIZE, VoxelChunk::SIZE);
    
    // Remove old quads from this region (main thread only - safe)
    auto& quads = mesh->quads;
    quads.erase(
        std::remove_if(quads.begin(), quads.end(), [&](const QuadFace& quad) {
            int qx = static_cast<int>(quad.position.x);
            int qy = static_cast<int>(quad.position.y);
            int qz = static_cast<int>(quad.position.z);
            
            // Adjust for face offset to get actual voxel coords
            int face = quad.faceDir;
            if (face == 1) qy--;       // Top face
            else if (face == 3) qz--;  // Front face
            else if (face == 5) qx--;  // Right face
            
            return (qx >= minX && qx < maxX &&
                    qy >= minY && qy < maxY &&
                    qz >= minZ && qz < maxZ);
        }),
        quads.end()
    );
    
    // Append new quads from worker thread
    quads.insert(quads.end(), result.quads.begin(), result.quads.end());
    
    // Trigger callback to signal mesh is ready for GPU upload
    result.chunk->triggerMeshUpdateCallback();
}

bool GreedyMeshQueue::hasPendingWork() const
{
    std::lock_guard<std::mutex> lock1(m_jobQueueMutex);
    std::lock_guard<std::mutex> lock2(m_completedQueueMutex);
    return !m_jobQueue.empty() || !m_completedQueue.empty();
}

size_t GreedyMeshQueue::getPendingWorkCount() const
{
    std::lock_guard<std::mutex> lock1(m_jobQueueMutex);
    std::lock_guard<std::mutex> lock2(m_completedQueueMutex);
    return m_jobQueue.size() + m_completedQueue.size();
}

void GreedyMeshQueue::clear()
{
    {
        std::lock_guard<std::mutex> lock(m_jobQueueMutex);
        while (!m_jobQueue.empty()) {
            m_jobQueue.pop();
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(m_completedQueueMutex);
        while (!m_completedQueue.empty()) {
            m_completedQueue.pop();
        }
    }
}
