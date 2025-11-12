// GreedyMeshQueue.cpp - Multi-threaded region mesh generation
#include "GPUMeshQueue.h"
#include "../Profiling/Profiler.h"
#include <iostream>
#include <unordered_set>

// Global instance
std::unique_ptr<GreedyMeshQueue> g_greedyMeshQueue = nullptr;

GreedyMeshQueue::GreedyMeshQueue()
{
    // Spawn worker threads (leave 2 cores for main thread and other work)
    int numThreads = std::max(2, static_cast<int>(std::thread::hardware_concurrency()) - 2);
    
    std::cout << "[GREEDY MESH] Starting " << numThreads << " worker threads for chunk meshing" << std::endl;
    
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

void GreedyMeshQueue::queueChunkMesh(VoxelChunk* chunk)
{
    if (!chunk) {
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(m_jobQueueMutex);
        m_jobQueue.insert(chunk);
    }
    
    m_jobQueueCV.notify_one();
}

int GreedyMeshQueue::processQueue(int maxItemsPerFrame)
{
    PROFILE_SCOPE("GreedyMeshQueue::processQueue");
    
    int itemsProcessed = 0;
    
    // Process completed chunk meshes and upload to GPU
    while (itemsProcessed < maxItemsPerFrame)
    {
        ChunkMeshResult result;
        
        {
            std::lock_guard<std::mutex> lock(m_completedQueueMutex);
            if (m_completedQueue.empty()) break;
            
            result = std::move(m_completedQueue.front());
            m_completedQueue.pop();
        }
        
        uploadChunkMesh(result);
        
        itemsProcessed++;
    }
    
    return itemsProcessed;
}

void GreedyMeshQueue::workerThreadFunc()
{
    while (true)
    {
        VoxelChunk* chunk = nullptr;
        
        // Wait for work
        {
            std::unique_lock<std::mutex> lock(m_jobQueueMutex);
            m_jobQueueCV.wait(lock, [this] { return m_shutdownFlag || !m_jobQueue.empty(); });
            
            if (m_shutdownFlag) return;
            
            if (m_jobQueue.empty()) continue;
            
            // Pop from set (grab any element)
            auto it = m_jobQueue.begin();
            chunk = *it;
            m_jobQueue.erase(it);
        }
        
        // Generate greedy mesh for entire chunk (runs on worker thread)
        ChunkMeshResult result;
        result.chunk = chunk;
        
        // Generate quads and store in chunk's renderMesh
        auto quads = chunk->generateFullChunkMesh();
        auto mesh = chunk->getRenderMesh();
        if (mesh) {
            mesh->quads = std::move(quads);
            mesh->needsGPUUpload = true;
            
            // Build voxelFaceToQuadIndex map for ALL voxels in ALL quads
            mesh->voxelFaceToQuadIndex.clear();
            for (size_t quadIdx = 0; quadIdx < mesh->quads.size(); ++quadIdx) {
                const QuadFace& quad = mesh->quads[quadIdx];
                int width = static_cast<int>(quad.width);
                int height = static_cast<int>(quad.height);
                int face = quad.faceDir;
                
                // Determine base voxel coordinates from quad center
                int baseX, baseY, baseZ;
                if (face == 0) { // -Y
                    baseX = static_cast<int>(quad.position.x - width * 0.5f);
                    baseY = static_cast<int>(quad.position.y);
                    baseZ = static_cast<int>(quad.position.z - height * 0.5f);
                } else if (face == 1) { // +Y
                    baseX = static_cast<int>(quad.position.x - width * 0.5f);
                    baseY = static_cast<int>(quad.position.y - 1);
                    baseZ = static_cast<int>(quad.position.z - height * 0.5f);
                } else if (face == 2) { // -Z
                    baseX = static_cast<int>(quad.position.x - width * 0.5f);
                    baseY = static_cast<int>(quad.position.y - height * 0.5f);
                    baseZ = static_cast<int>(quad.position.z);
                } else if (face == 3) { // +Z
                    baseX = static_cast<int>(quad.position.x - width * 0.5f);
                    baseY = static_cast<int>(quad.position.y - height * 0.5f);
                    baseZ = static_cast<int>(quad.position.z - 1);
                } else if (face == 4) { // -X
                    baseX = static_cast<int>(quad.position.x);
                    baseY = static_cast<int>(quad.position.y - height * 0.5f);
                    baseZ = static_cast<int>(quad.position.z - width * 0.5f);
                } else { // +X (face == 5)
                    baseX = static_cast<int>(quad.position.x - 1);
                    baseY = static_cast<int>(quad.position.y - height * 0.5f);
                    baseZ = static_cast<int>(quad.position.z - width * 0.5f);
                }
                
                // Map every voxel covered by this quad
                if (face == 0 || face == 1) { // Y faces: width=X, height=Z
                    for (int dz = 0; dz < height; ++dz) {
                        for (int dx = 0; dx < width; ++dx) {
                            int vx = baseX + dx;
                            int vy = baseY;
                            int vz = baseZ + dz;
                            if (vx >= 0 && vx < 256 && vy >= 0 && vy < 256 && vz >= 0 && vz < 256) {
                                int voxelIdx = vx + vy * 256 + vz * 256 * 256;
                                uint32_t key = voxelIdx * 6 + face;
                                mesh->voxelFaceToQuadIndex[key] = static_cast<uint16_t>(quadIdx);
                            }
                        }
                    }
                } else if (face == 2 || face == 3) { // Z faces: width=X, height=Y
                    for (int dy = 0; dy < height; ++dy) {
                        for (int dx = 0; dx < width; ++dx) {
                            int vx = baseX + dx;
                            int vy = baseY + dy;
                            int vz = baseZ;
                            if (vx >= 0 && vx < 256 && vy >= 0 && vy < 256 && vz >= 0 && vz < 256) {
                                int voxelIdx = vx + vy * 256 + vz * 256 * 256;
                                uint32_t key = voxelIdx * 6 + face;
                                mesh->voxelFaceToQuadIndex[key] = static_cast<uint16_t>(quadIdx);
                            }
                        }
                    }
                } else { // X faces: width=Z, height=Y
                    for (int dy = 0; dy < height; ++dy) {
                        for (int dz = 0; dz < width; ++dz) {
                            int vx = baseX;
                            int vy = baseY + dy;
                            int vz = baseZ + dz;
                            if (vx >= 0 && vx < 256 && vy >= 0 && vy < 256 && vz >= 0 && vz < 256) {
                                int voxelIdx = vx + vy * 256 + vz * 256 * 256;
                                uint32_t key = voxelIdx * 6 + face;
                                mesh->voxelFaceToQuadIndex[key] = static_cast<uint16_t>(quadIdx);
                            }
                        }
                    }
                }
            }
        }
        
        // Push to completed queue for main thread GPU upload
        {
            std::lock_guard<std::mutex> lock(m_completedQueueMutex);
            m_completedQueue.push(std::move(result));
        }
    }
}

void GreedyMeshQueue::uploadChunkMesh(const ChunkMeshResult& result)
{
    PROFILE_SCOPE("GreedyMeshQueue::uploadChunkMesh");
    
    if (!result.chunk || !result.chunk->isClient()) {
        return;
    }
    
    // Trigger chunk to upload its greedy mesh to quad renderer
    result.chunk->uploadMeshToGPU();
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
        m_jobQueue.clear();
    }
    
    {
        std::lock_guard<std::mutex> lock(m_completedQueueMutex);
        while (!m_completedQueue.empty()) {
            m_completedQueue.pop();
        }
    }
}
