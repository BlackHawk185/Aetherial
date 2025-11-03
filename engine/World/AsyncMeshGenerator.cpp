#include "AsyncMeshGenerator.h"
#include "../Profiling/Profiler.h"
#include <iostream>

AsyncMeshGenerator* g_asyncMeshGenerator = nullptr;

AsyncMeshGenerator::AsyncMeshGenerator()
    : m_running(true), m_pendingJobs(0)
{
    // For large chunks (512³), mesh generation is memory-bound, not CPU-bound
    // Single-threaded gives better cache locality and less overhead
    // Use at most 2 threads to avoid cache thrashing
    unsigned int threadCount = 1;  // Single-threaded mesh generation
    
    const char* meshThreadsEnv = std::getenv("MESH_THREADS");
    if (meshThreadsEnv) {
        threadCount = std::max(1u, static_cast<unsigned int>(std::atoi(meshThreadsEnv)));
        threadCount = std::min(threadCount, 4u);  // Cap at 4 threads max
    }

    std::cout << "[ASYNC MESH] Starting " << threadCount << " worker thread(s)" << std::endl;

    for (unsigned int i = 0; i < threadCount; ++i) {
        m_workers.emplace_back(&AsyncMeshGenerator::workerThreadFunc, this);
    }
}

AsyncMeshGenerator::~AsyncMeshGenerator()
{
    shutdown();
}

void AsyncMeshGenerator::shutdown()
{
    m_running = false;
    m_jobCondition.notify_all();

    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    m_workers.clear();
}

void AsyncMeshGenerator::queueChunkMeshGeneration(VoxelChunk* chunk, std::function<void()> onComplete)
{
    if (!chunk) return;

    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_jobQueue.push({chunk, onComplete});
        m_pendingJobs++;
    }

    m_jobCondition.notify_one();
}

void AsyncMeshGenerator::processCompletedMeshes()
{
    PROFILE_SCOPE("AsyncMeshGenerator::processCompletedMeshes");

    std::queue<CompletedMesh> localQueue;

    {
        std::lock_guard<std::mutex> lock(m_completedMutex);
        std::swap(localQueue, m_completedQueue);
    }

    while (!localQueue.empty())
    {
        auto& completed = localQueue.front();

        // Fast atomic swap on main thread - no actual mesh generation here
        completed.chunk->setRenderMesh(completed.renderMesh);
        completed.chunk->setCollisionMesh(completed.collisionMesh);
        completed.chunk->m_modelInstances = std::move(completed.modelInstances);
        completed.chunk->meshDirty = false;

        // Call completion callback if provided
        if (completed.onComplete) {
            completed.onComplete();
        }

        localQueue.pop();
    }
}

bool AsyncMeshGenerator::hasPendingJobs() const
{
    return m_pendingJobs > 0;
}

void AsyncMeshGenerator::workerThreadFunc()
{
    while (m_running)
    {
        MeshJob job;

        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCondition.wait(lock, [this] { return !m_jobQueue.empty() || !m_running; });

            if (!m_running) break;

            if (m_jobQueue.empty()) continue;

            job = m_jobQueue.front();
            m_jobQueue.pop();
        }

        // Generate mesh on background thread (CPU-intensive work)
        auto newMesh = std::make_shared<VoxelMesh>();
        auto newCollisionMesh = std::make_shared<CollisionMesh>();
        std::unordered_map<uint8_t, std::vector<Vec3>> tempModelInstances;

        // Copy the chunk's mesh generation logic here
        auto& registry = BlockTypeRegistry::getInstance();
        for (int z = 0; z < VoxelChunk::SIZE; ++z) {
            for (int y = 0; y < VoxelChunk::SIZE; ++y) {
                for (int x = 0; x < VoxelChunk::SIZE; ++x) {
                    uint8_t blockID = job.chunk->getVoxel(x, y, z);
                    if (blockID == BlockID::AIR) continue;

                    const BlockTypeInfo* blockInfo = registry.getBlockType(blockID);
                    if (blockInfo && blockInfo->renderType == BlockRenderType::OBJ) {
                        Vec3 instancePos((float)x + 0.5f, (float)y, (float)z + 0.5f);
                        tempModelInstances[blockID].push_back(instancePos);
                    }
                }
            }
        }

        job.chunk->generateSimpleMeshInto(newMesh->quads);

        for (const auto& quad : newMesh->quads) {
            newCollisionMesh->faces.push_back({
                quad.position,
                quad.normal,
                quad.width,
                quad.height
            });
        }

        newMesh->needsUpdate = true;

        // Add to completed queue
        {
            std::lock_guard<std::mutex> lock(m_completedMutex);
            m_completedQueue.push({
                job.chunk,
                newMesh,
                newCollisionMesh,
                std::move(tempModelInstances),
                job.onComplete
            });
        }

        m_pendingJobs--;
    }
}
