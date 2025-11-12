// GreedyMeshQueue.cpp - Multi-threaded region mesh generation
#include "GPUMeshQueue.h"
#include "InstancedQuadRenderer.h"
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
        std::cout << "[MESH QUEUE] WARNING: Attempted to queue null chunk!" << std::endl;
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(m_jobQueueMutex);
        auto result = m_jobQueue.insert(chunk);
        if (result.second) {
            std::cout << "[MESH QUEUE] Chunk " << chunk << " queued for meshing. Queue size: " 
                      << m_jobQueue.size() << std::endl;
        } else {
            std::cout << "[MESH QUEUE] Chunk " << chunk << " already in queue (deduplicated)" << std::endl;
        }
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
            std::cout << "[MESH QUEUE] Processing completed mesh for chunk " << result.chunk 
                      << ". Remaining in queue: " << m_completedQueue.size() << std::endl;
        }
        
        uploadChunkMesh(result);
        
        itemsProcessed++;
    }
    
    if (itemsProcessed > 0) {
        std::cout << "[MESH QUEUE] Processed " << itemsProcessed << " mesh(es) this frame" << std::endl;
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
        
        std::cout << "[MESH WORKER] Starting mesh generation for chunk " << chunk << std::endl;
        
        // Generate mesh for entire chunk
        ChunkMeshResult result;
        result.chunk = chunk;
        result.quads = chunk->generateFullChunkMesh();
        
        std::cout << "[MESH WORKER] Completed mesh generation for chunk " << chunk 
                  << " - Generated " << result.quads.size() << " quads" << std::endl;
        
        // Push to completed queue
        {
            std::lock_guard<std::mutex> lock(m_completedQueueMutex);
            m_completedQueue.push(std::move(result));
            std::cout << "[MESH WORKER] Pushed to completed queue. Completed queue size: " 
                      << m_completedQueue.size() << std::endl;
        }
    }
}

void GreedyMeshQueue::uploadChunkMesh(const ChunkMeshResult& result)
{
    PROFILE_SCOPE("GreedyMeshQueue::uploadChunkMesh");
    
    if (!result.chunk) {
        std::cout << "[MESH UPLOAD] ERROR: Null chunk in result!" << std::endl;
        return;
    }
    
    if (!result.chunk->isClient()) {
        std::cout << "[MESH UPLOAD] Skipping server-side chunk " << result.chunk << std::endl;
        return;
    }
    
    std::cout << "[MESH UPLOAD] Uploading mesh for chunk " << result.chunk 
              << " with " << result.quads.size() << " quads" << std::endl;
    
    // Get or create render mesh
    auto mesh = result.chunk->getRenderMesh();
    if (!mesh) {
        mesh = std::make_shared<VoxelMesh>();
        result.chunk->setRenderMesh(mesh);
        std::cout << "[MESH UPLOAD] Created new VoxelMesh for chunk" << std::endl;
    }
    
    // Store quads directly
    mesh->quads = result.quads;
    mesh->needsGPUUpload = true;
    
    // Populate voxel-to-quad tracking by scanning all quads
    mesh->voxelFaceToQuadIndex.clear();
    mesh->isExploded.assign(VoxelChunk::VOLUME, false);
    
    for (size_t quadIdx = 0; quadIdx < mesh->quads.size(); ++quadIdx)
    {
        const QuadFace& quad = mesh->quads[quadIdx];
        int width = static_cast<int>(quad.width);
        int height = static_cast<int>(quad.height);
        int face = quad.faceDir;
        
        // Calculate base corner position from center (reverse of addQuad logic)
        int baseX, baseY, baseZ;
        
        switch (face)
        {
            case 0: // -Y (bottom): width=X, height=Z
                baseX = static_cast<int>(quad.position.x - width * 0.5f);
                baseY = static_cast<int>(quad.position.y);
                baseZ = static_cast<int>(quad.position.z - height * 0.5f);
                break;
            case 1: // +Y (top): width=X, height=Z
                baseX = static_cast<int>(quad.position.x - width * 0.5f);
                baseY = static_cast<int>(quad.position.y - 1);
                baseZ = static_cast<int>(quad.position.z - height * 0.5f);
                break;
            case 2: // -Z (back): width=X, height=Y
                baseX = static_cast<int>(quad.position.x - width * 0.5f);
                baseY = static_cast<int>(quad.position.y - height * 0.5f);
                baseZ = static_cast<int>(quad.position.z);
                break;
            case 3: // +Z (front): width=X, height=Y
                baseX = static_cast<int>(quad.position.x - width * 0.5f);
                baseY = static_cast<int>(quad.position.y - height * 0.5f);
                baseZ = static_cast<int>(quad.position.z - 1);
                break;
            case 4: // -X (left): width=Z, height=Y
                baseX = static_cast<int>(quad.position.x);
                baseY = static_cast<int>(quad.position.y - height * 0.5f);
                baseZ = static_cast<int>(quad.position.z - width * 0.5f);
                break;
            case 5: // +X (right): width=Z, height=Y
                baseX = static_cast<int>(quad.position.x - 1);
                baseY = static_cast<int>(quad.position.y - height * 0.5f);
                baseZ = static_cast<int>(quad.position.z - width * 0.5f);
                break;
            default:
                continue; // Invalid face direction
        }
        
        // Map all voxels covered by this quad based on face direction
        if (face == 0 || face == 1) // Y faces: width=X, height=Z
        {
            for (int dz = 0; dz < height; ++dz)
            {
                for (int dx = 0; dx < width; ++dx)
                {
                    int vx = baseX + dx;
                    int vy = baseY;
                    int vz = baseZ + dz;
                    
                    if (vx >= 0 && vx < VoxelChunk::SIZE && 
                        vy >= 0 && vy < VoxelChunk::SIZE && 
                        vz >= 0 && vz < VoxelChunk::SIZE)
                    {
                        int voxelIdx = vx + vy * VoxelChunk::SIZE + vz * VoxelChunk::SIZE * VoxelChunk::SIZE;
                        uint32_t key = voxelIdx * 6 + face;
                        mesh->voxelFaceToQuadIndex[key] = static_cast<uint16_t>(quadIdx);
                    }
                }
            }
        }
        else if (face == 2 || face == 3) // Z faces: width=X, height=Y
        {
            for (int dy = 0; dy < height; ++dy)
            {
                for (int dx = 0; dx < width; ++dx)
                {
                    int vx = baseX + dx;
                    int vy = baseY + dy;
                    int vz = baseZ;
                    
                    if (vx >= 0 && vx < VoxelChunk::SIZE && 
                        vy >= 0 && vy < VoxelChunk::SIZE && 
                        vz >= 0 && vz < VoxelChunk::SIZE)
                    {
                        int voxelIdx = vx + vy * VoxelChunk::SIZE + vz * VoxelChunk::SIZE * VoxelChunk::SIZE;
                        uint32_t key = voxelIdx * 6 + face;
                        mesh->voxelFaceToQuadIndex[key] = static_cast<uint16_t>(quadIdx);
                    }
                }
            }
        }
        else // X faces: width=Z, height=Y
        {
            for (int dy = 0; dy < height; ++dy)
            {
                for (int dz = 0; dz < width; ++dz)
                {
                    int vx = baseX;
                    int vy = baseY + dy;
                    int vz = baseZ + dz;
                    
                    if (vx >= 0 && vx < VoxelChunk::SIZE && 
                        vy >= 0 && vy < VoxelChunk::SIZE && 
                        vz >= 0 && vz < VoxelChunk::SIZE)
                    {
                        int voxelIdx = vx + vy * VoxelChunk::SIZE + vz * VoxelChunk::SIZE * VoxelChunk::SIZE;
                        uint32_t key = voxelIdx * 6 + face;
                        mesh->voxelFaceToQuadIndex[key] = static_cast<uint16_t>(quadIdx);
                    }
                }
            }
        }
    }
    
    std::cout << "[MESH UPLOAD] Stored " << mesh->quads.size() << " quads in mesh, populated tracking" << std::endl;
    
    // Upload to GPU
    extern std::unique_ptr<InstancedQuadRenderer> g_instancedQuadRenderer;
    if (g_instancedQuadRenderer) {
        g_instancedQuadRenderer->uploadChunkMesh(result.chunk);
        std::cout << "[MESH UPLOAD] Triggered GPU upload via renderer" << std::endl;
    } else {
        std::cout << "[MESH UPLOAD] ERROR: No renderer available for GPU upload!" << std::endl;
    }
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
