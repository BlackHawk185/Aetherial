// IslandChunkSystem.cpp - Implementation of physics-driven chunking
#include "IslandChunkSystem.h"

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <chrono>
#include <random>
#include <unordered_set>
#include <queue>

#include "VoxelChunk.h"
#include "BlockType.h"
#include "TreeGenerator.h"
#include "AsyncMeshGenerator.h"
#include "../Profiling/Profiler.h"
#include "../Rendering/InstancedQuadRenderer.h"
#include "../Rendering/ModelInstanceRenderer.h"
#include "../../libs/FastNoiseLite/FastNoiseLite.h"

IslandChunkSystem g_islandSystem;

IslandChunkSystem::IslandChunkSystem()
{
    // Initialize system
    // Seed random number generator for island velocity
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    
    // Set static pointer so VoxelChunk can access island system for inter-chunk culling
    VoxelChunk::s_islandSystem = this;
}

IslandChunkSystem::~IslandChunkSystem()
{
    // Clear static pointer before destruction
    VoxelChunk::s_islandSystem = nullptr;
    
    // Clean up all islands
    // Collect IDs first to avoid iterator invalidation
    std::vector<uint32_t> islandIDs;
    for (auto& [id, island] : m_islands)
    {
        islandIDs.push_back(id);
    }
    
    // Now safely destroy all islands
    for (uint32_t id : islandIDs)
    {
        destroyIsland(id);
    }
}

uint32_t IslandChunkSystem::createIsland(const Vec3& physicsCenter)
{
    return createIsland(physicsCenter, 0);  // 0 = auto-assign ID
}

uint32_t IslandChunkSystem::createIsland(const Vec3& physicsCenter, uint32_t forceIslandID)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    
    // Determine island ID
    uint32_t islandID;
    if (forceIslandID == 0)
    {
        // Auto-assign: use next available ID
        islandID = m_nextIslandID++;
    }
    else
    {
        // Force specific ID (for network sync)
        islandID = forceIslandID;
        // Update next ID to avoid collisions
        if (forceIslandID >= m_nextIslandID)
        {
            m_nextIslandID = forceIslandID + 1;
        }
    }
    
    // Create the island
    FloatingIsland& island = m_islands[islandID];
    island.islandID = islandID;
    island.physicsCenter = physicsCenter;
    island.needsPhysicsUpdate = true;
    island.acceleration = Vec3(0.0f, 0.0f, 0.0f);
    
    // Set initial random drift velocity for natural island movement
    // Use island position as seed for deterministic random values
    uint32_t seedX = static_cast<uint32_t>(std::abs(physicsCenter.x * 73856093.0f));
    uint32_t seedY = static_cast<uint32_t>(std::abs(physicsCenter.y * 19349663.0f));
    uint32_t seedZ = static_cast<uint32_t>(std::abs(physicsCenter.z * 83492791.0f));
    std::mt19937 rng(seedX ^ seedY ^ seedZ);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    
    island.velocity = Vec3(
        dist(rng),  // Random X drift
        dist(rng) * 0.3f,  // Reduced Y drift (mostly horizontal movement)
        dist(rng)   // Random Z drift
    );
    
    std::cout << "[ISLAND] Created island " << islandID 
              << " with drift velocity (" << island.velocity.x 
              << ", " << island.velocity.y 
              << ", " << island.velocity.z << ")" << std::endl;
    
    return islandID;
}

void IslandChunkSystem::destroyIsland(uint32_t islandID)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto it = m_islands.find(islandID);
    if (it == m_islands.end())
        return;

    m_islands.erase(it);
}

FloatingIsland* IslandChunkSystem::getIsland(uint32_t islandID)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto it = m_islands.find(islandID);
    return (it != m_islands.end()) ? &it->second : nullptr;
}

const FloatingIsland* IslandChunkSystem::getIsland(uint32_t islandID) const
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto it = m_islands.find(islandID);
    return (it != m_islands.end()) ? &it->second : nullptr;
}

Vec3 IslandChunkSystem::getIslandCenter(uint32_t islandID) const
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto it = m_islands.find(islandID);
    if (it != m_islands.end())
    {
        return it->second.physicsCenter;
    }
    return Vec3(0.0f, 0.0f, 0.0f);  // Return zero vector if island not found
}

Vec3 IslandChunkSystem::getIslandVelocity(uint32_t islandID) const
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto it = m_islands.find(islandID);
    if (it != m_islands.end())
    {
        return it->second.velocity;
    }
    return Vec3(0.0f, 0.0f, 0.0f);  // Return zero velocity if island not found
}

void IslandChunkSystem::addChunkToIsland(uint32_t islandID, const Vec3& chunkCoord)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto itIsl = m_islands.find(islandID);
    FloatingIsland* island = (itIsl != m_islands.end()) ? &itIsl->second : nullptr;
    if (!island)
        return;

    // Check if chunk already exists
    if (island->chunks.find(chunkCoord) != island->chunks.end())
        return;

    // Create new chunk and set island context
    auto newChunk = std::make_unique<VoxelChunk>();
    newChunk->setIslandContext(islandID, chunkCoord);
    newChunk->setIsClient(m_isClient);  // Inherit client flag from island system
    island->chunks[chunkCoord] = std::move(newChunk);
}

void IslandChunkSystem::removeChunkFromIsland(uint32_t islandID, const Vec3& chunkCoord)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto itIsl = m_islands.find(islandID);
    FloatingIsland* island = (itIsl != m_islands.end()) ? &itIsl->second : nullptr;
    if (!island)
        return;

    auto it = island->chunks.find(chunkCoord);
    if (it != island->chunks.end())
    {
        island->chunks.erase(it);
    }
}

VoxelChunk* IslandChunkSystem::getChunkFromIsland(uint32_t islandID, const Vec3& chunkCoord)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto itIsl = m_islands.find(islandID);
    FloatingIsland* island = (itIsl != m_islands.end()) ? &itIsl->second : nullptr;
    if (!island)
        return nullptr;

    auto it = island->chunks.find(chunkCoord);
    if (it != island->chunks.end())
    {
        return it->second.get();
    }

    return nullptr;
}

void IslandChunkSystem::generateFloatingIslandOrganic(uint32_t islandID, uint32_t seed, float radius, BiomeType biome)
{
    PROFILE_SCOPE("IslandChunkSystem::generateFloatingIslandOrganic");
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    FloatingIsland* island = getIsland(islandID);
    if (!island)
        return;

    // Get biome palette for block selection
    BiomeSystem biomeSystem;
    BiomePalette palette = biomeSystem.getPalette(biome);
    
    std::cout << "[BIOME] Island " << islandID << " - " << biomeSystem.getBiomeName(biome) << std::endl;

    // Start with a center chunk at origin to ensure we have at least one chunk
    addChunkToIsland(islandID, Vec3(0, 0, 0));
    
    // **NOISE CONFIGURATION**
    float densityThreshold = 0.35f;
    float baseHeightRatio = 0.075f;  // Height as a factor of radius (reduced for flatter islands)
    float noise3DFrequency = 0.02f;
    float noise2DFrequency = 0.015f;
    int fractalOctaves = 2;
    float fractalGain = 0.4f;
    
    // **ENVIRONMENT OVERRIDES** for noise parameters
    const char* freq3DEnv = std::getenv("NOISE_FREQ_3D");
    if (freq3DEnv) noise3DFrequency = std::stof(freq3DEnv);
    
    const char* freq2DEnv = std::getenv("NOISE_FREQ_2D");
    if (freq2DEnv) noise2DFrequency = std::stof(freq2DEnv);
    
    const char* thresholdEnv = std::getenv("NOISE_THRESHOLD");
    if (thresholdEnv) densityThreshold = std::stof(thresholdEnv);
    
    // Setup single noise generator (optimized: one noise call instead of two)
    FastNoiseLite noise3D;
    noise3D.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    noise3D.SetSeed(seed);
    noise3D.SetFrequency(noise3DFrequency);
    noise3D.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise3D.SetFractalOctaves(fractalOctaves);
    noise3D.SetFractalLacunarity(2.0f);
    noise3D.SetFractalGain(fractalGain);
    
    auto voxelGenStart = std::chrono::high_resolution_clock::now();
    
    // **BFS CONNECTIVITY-AWARE GENERATION**
    // Only place voxels reachable from center - guarantees connectivity
    int islandHeight = static_cast<int>(radius * baseHeightRatio);
    float radiusSquared = (radius * 1.4f) * (radius * 1.4f);
    float radiusDivisor = 1.0f / (radius * 1.2f);
    
    long long voxelsGenerated = 0;
    long long voxelsSampled = 0;
    
    // BFS from center
    std::queue<Vec3> frontier;
    std::unordered_set<int64_t> visited;
    
    auto encodePos = [](const Vec3& p) -> int64_t {
        return (static_cast<int64_t>(p.x + 32768) << 32) | 
               (static_cast<int64_t>(p.y + 32768) << 16) | 
               static_cast<int64_t>(p.z + 32768);
    };
    
    Vec3 startPos(0, 0, 0);
    frontier.push(startPos);
    visited.insert(encodePos(startPos));
    setBlockIDWithAutoChunk(islandID, startPos, palette.deepBlock);  // Use biome deep block
    voxelsGenerated++;
    
    while (!frontier.empty())
    {
        Vec3 current = frontier.front();
        frontier.pop();
        
        // Check all 6 neighbors
        static const Vec3 neighbors[6] = {
            Vec3(1, 0, 0), Vec3(-1, 0, 0),
            Vec3(0, 1, 0), Vec3(0, -1, 0),
            Vec3(0, 0, 1), Vec3(0, 0, -1)
        };
        
        for (const Vec3& delta : neighbors)
        {
            Vec3 neighbor = current + delta;
            int64_t neighborKey = encodePos(neighbor);
            
            if (visited.count(neighborKey)) continue;
            visited.insert(neighborKey);
            voxelsSampled++;
            
            float dx = static_cast<float>(neighbor.x);
            float dy = static_cast<float>(neighbor.y);
            float dz = static_cast<float>(neighbor.z);
            
            // Sphere culling
            float distanceSquared = dx * dx + dy * dy + dz * dz;
            if (distanceSquared > radiusSquared) continue;
            
            // Vertical density
            float islandHeightRange = islandHeight * 2.0f;
            float normalizedY = (dy + islandHeight) / islandHeightRange;
            float centerOffset = normalizedY - 0.5f;
            float verticalDensity = 1.0f - (centerOffset * centerOffset * 4.0f);
            verticalDensity = std::max(0.0f, verticalDensity);
            
            if (verticalDensity < 0.01f) continue;
            
            // Radial falloff
            float distanceFromCenter = std::sqrt(distanceSquared);
            float islandBase = 1.0f - (distanceFromCenter * radiusDivisor);
            islandBase = std::max(0.0f, islandBase);
            islandBase = islandBase * islandBase;
            
            if (islandBase < 0.01f) continue;
            
            // 3D noise
            float noise = noise3D.GetNoise(dx, dy * 0.7f, dz);
            noise = (noise + 1.0f) * 0.5f;
            
            float finalDensity = islandBase * verticalDensity * noise;
            
            if (finalDensity > densityThreshold)
            {
                // Initially place all blocks as deep blocks
                // We'll determine the correct surface/subsurface blocks in a second pass
                setBlockIDWithAutoChunk(islandID, neighbor, palette.deepBlock);
                frontier.push(neighbor);
                voxelsGenerated++;
            }
        }
    }
    
    auto voxelGenEnd = std::chrono::high_resolution_clock::now();
    auto voxelGenDuration = std::chrono::duration_cast<std::chrono::milliseconds>(voxelGenEnd - voxelGenStart).count();
    
    std::cout << "🔨 Voxel Generation (BFS): " << voxelGenDuration << "ms (" << voxelsGenerated << " voxels, " 
              << island->chunks.size() << " chunks)" << std::endl;
    std::cout << "   └─ Positions Sampled: " << voxelsSampled << " (connectivity-aware)" << std::endl;
    
    // **SURFACE DETECTION PASS**
    // Now that all voxels are placed, determine which should be surface/subsurface blocks
    auto surfacePassStart = std::chrono::high_resolution_clock::now();
    int surfaceBlocksPlaced = 0;
    int subsurfaceBlocksPlaced = 0;
    
    static const Vec3 checkNeighbors[6] = {
        Vec3(1, 0, 0), Vec3(-1, 0, 0),
        Vec3(0, 1, 0), Vec3(0, -1, 0),
        Vec3(0, 0, 1), Vec3(0, 0, -1)
    };
    
    // Iterate through all chunks and check each solid block
    for (auto& [chunkCoord, chunk] : island->chunks)
    {
        if (!chunk) continue;
        
        for (int lz = 0; lz < VoxelChunk::SIZE; ++lz)
        {
            for (int ly = 0; ly < VoxelChunk::SIZE; ++ly)
            {
                for (int lx = 0; lx < VoxelChunk::SIZE; ++lx)
                {
                    uint8_t currentBlock = chunk->getVoxel(lx, ly, lz);
                    if (currentBlock == BlockID::AIR) continue;
                    if (currentBlock != palette.deepBlock) continue;  // Only process deep blocks
                    
                    // Calculate world position
                    Vec3 worldPos = chunkCoord * VoxelChunk::SIZE + Vec3(lx, ly, lz);
                    
                    // Check if any neighbor is air (exposed surface)
                    bool isExposed = false;
                    for (const Vec3& delta : checkNeighbors)
                    {
                        Vec3 neighborPos = worldPos + delta;
                        uint8_t neighborBlock = getBlockIDInIsland(islandID, neighborPos);
                        if (neighborBlock == BlockID::AIR)
                        {
                            isExposed = true;
                            break;
                        }
                    }
                    
                    if (isExposed)
                    {
                        // This is a surface block - replace with surface block type
                        chunk->setVoxel(lx, ly, lz, palette.surfaceBlock);
                        surfaceBlocksPlaced++;
                    }
                    else
                    {
                        // Check if any neighbor is a surface block (1 layer deep)
                        bool nearSurface = false;
                        for (const Vec3& delta : checkNeighbors)
                        {
                            Vec3 neighborPos = worldPos + delta;
                            uint8_t neighborBlock = getBlockIDInIsland(islandID, neighborPos);
                            if (neighborBlock == palette.surfaceBlock)
                            {
                                nearSurface = true;
                                break;
                            }
                        }
                        
                        if (nearSurface)
                        {
                            // This is a subsurface block (1 layer below surface)
                            chunk->setVoxel(lx, ly, lz, palette.subsurfaceBlock);
                            subsurfaceBlocksPlaced++;
                        }
                        // else: remains deep block
                    }
                }
            }
        }
    }
    
    auto surfacePassEnd = std::chrono::high_resolution_clock::now();
    auto surfacePassDuration = std::chrono::duration_cast<std::chrono::milliseconds>(surfacePassEnd - surfacePassStart).count();
    
    std::cout << "🎨 Surface Detection: " << surfacePassDuration << "ms (" 
              << surfaceBlocksPlaced << " surface, " 
              << subsurfaceBlocksPlaced << " subsurface)" << std::endl;
    
    // **VEGETATION DECORATION PASS**
    // Place grass GLB models and voxel trees based on biome vegetation density
    auto decorationStart = std::chrono::high_resolution_clock::now();
    
    int grassPlaced = 0;
    int treesPlaced = 0;
    
    // Use biome-specific vegetation density - SPARSE trees for Terralith-like feel
    float grassChance = palette.vegetationDensity * 30.0f;  // Convert to percentage (0-30%)
    float treeChance = palette.vegetationDensity * 1.5f;    // Much sparser trees (0-1.5%)
    
    // Direct chunk iteration - we already have the island pointer, no locks needed
    for (auto& [chunkCoord, chunk] : island->chunks) {
        if (!chunk) continue;
        
        // Scan each voxel in this chunk
        for (int z = 0; z < VoxelChunk::SIZE; ++z) {
            for (int x = 0; x < VoxelChunk::SIZE; ++x) {
                for (int y = VoxelChunk::SIZE - 1; y >= 0; --y) {  // Scan top-down
                    uint8_t blockID = chunk->getVoxel(x, y, z);
                    if (blockID == BlockID::AIR) continue;
                    
                    // Found solid block - check if surface is exposed (only place on surface blocks)
                    if (blockID != palette.surfaceBlock) {
                        break;  // Not a surface block, skip this column
                    }
                    
                    if (y + 1 < VoxelChunk::SIZE) {
                        uint8_t blockAbove = chunk->getVoxel(x, y + 1, z);
                        if (blockAbove == BlockID::AIR) {
                            float roll = (std::rand() % 100);
                            
                            // Calculate world position for tree placement
                            Vec3 worldPos = chunkCoord * VoxelChunk::SIZE + Vec3(x, y + 1, z);
                            
                            // Place voxel tree (rarer)
                            if (roll < treeChance) {
                                // Use world position as seed for variety
                                uint32_t treeSeed = static_cast<uint32_t>(
                                    static_cast<int>(worldPos.x) * 73856093 ^ 
                                    static_cast<int>(worldPos.y) * 19349663 ^ 
                                    static_cast<int>(worldPos.z) * 83492791
                                );
                                TreeGenerator::generateTree(this, islandID, worldPos, treeSeed, palette.vegetationDensity);
                                treesPlaced++;
                            }
                            // Place grass GLB (more common)
                            else if (roll < grassChance) {
                                chunk->setVoxel(x, y + 1, z, BlockID::DECOR_GRASS);
                                grassPlaced++;
                            }
                        }
                    } else {
                        // Edge of chunk - check neighboring chunk above (rare case)
                        Vec3 aboveChunkCoord = chunkCoord + Vec3(0, 1, 0);
                        auto itAbove = island->chunks.find(aboveChunkCoord);
                        if (itAbove != island->chunks.end() && itAbove->second) {
                            uint8_t blockAbove = itAbove->second->getVoxel(x, 0, z);
                            if (blockAbove == BlockID::AIR) {
                                float roll = (std::rand() % 100);
                                Vec3 worldPos = chunkCoord * VoxelChunk::SIZE + Vec3(x, y + 1, z);
                                
                                if (roll < treeChance) {
                                    uint32_t treeSeed = static_cast<uint32_t>(
                                        static_cast<int>(worldPos.x) * 73856093 ^ 
                                        static_cast<int>(worldPos.y) * 19349663 ^ 
                                        static_cast<int>(worldPos.z) * 83492791
                                    );
                                    TreeGenerator::generateTree(this, islandID, worldPos, treeSeed, palette.vegetationDensity);
                                    treesPlaced++;
                                }
                                else if (roll < grassChance) {
                                    itAbove->second->setVoxel(x, 0, z, BlockID::DECOR_GRASS);
                                    grassPlaced++;
                                }
                            }
                        }
                    }
                    
                    break;  // Found topmost solid block in this column, move to next
                }
            }
        }
    }
    
    auto decorationEnd = std::chrono::high_resolution_clock::now();
    auto decorationDuration = std::chrono::duration_cast<std::chrono::milliseconds>(decorationEnd - decorationStart).count();
    std::cout << "🌿 Vegetation: " << decorationDuration << "ms (" 
              << grassPlaced << " grass, " << treesPlaced << " trees)" << std::endl;
    
    // Chunks will be registered with renderer when client receives them via network
    // (Don't register here - renderer may not exist yet, and this violates separation of concerns)
    
    // Queue chunks for async mesh generation (if available)
    auto meshGenStart = std::chrono::high_resolution_clock::now();
    int chunksQueued = 0;
    
    if (g_asyncMeshGenerator)
    {
        for (auto& [chunkCoord, chunk] : island->chunks)
        {
            if (chunk)
            {
                g_asyncMeshGenerator->queueChunkMeshGeneration(chunk.get());
                chunksQueued++;
            }
        }
        
        auto meshGenEnd = std::chrono::high_resolution_clock::now();
        auto meshGenDuration = std::chrono::duration_cast<std::chrono::milliseconds>(meshGenEnd - meshGenStart).count();
        std::cout << "🔄 Async Mesh Queue: " << meshGenDuration << "ms (" << chunksQueued << " chunks queued)" << std::endl;
    }
    else
    {
        std::cout << "   └─ Async mesh generator not available - meshes will generate on first render" << std::endl;
    }
    
    auto totalEnd = std::chrono::high_resolution_clock::now();
    auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - startTime).count();
    
    std::cout << "✅ Island Generation Complete: " << totalDuration << "ms total" << std::endl;
    std::cout << "   └─ Breakdown: Voxels=" << voxelGenDuration << "ms (" 
              << (voxelGenDuration * 100 / std::max(1LL, totalDuration)) << "%)" << std::endl;
}

uint8_t IslandChunkSystem::getVoxelFromIsland(uint32_t islandID, const Vec3& islandRelativePosition) const
{
    // Hold lock across the entire access to prevent races
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto itIsl = m_islands.find(islandID);
    if (itIsl == m_islands.end())
        return 0;

    const FloatingIsland& island = itIsl->second;

    // Convert island-relative position to chunk coordinate and local voxel position
    Vec3 chunkCoord = FloatingIsland::islandPosToChunkCoord(islandRelativePosition);
    Vec3 localPos = FloatingIsland::islandPosToLocalPos(islandRelativePosition);

    // Find the chunk
    auto it = island.chunks.find(chunkCoord);
    if (it == island.chunks.end())
        return 0; // Chunk doesn't exist

    const VoxelChunk* chunk = it->second.get();
    if (!chunk)
        return 0;

    // Get voxel from chunk using local coordinates
    int x = static_cast<int>(localPos.x);
    int y = static_cast<int>(localPos.y);
    int z = static_cast<int>(localPos.z);

    // Check bounds (should be 0-15 for 16x16x16 chunks)
    if (x < 0 || x >= VoxelChunk::SIZE || y < 0 || y >= VoxelChunk::SIZE || z < 0 || z >= VoxelChunk::SIZE)
        return 0;

    return chunk->getVoxel(x, y, z);
}

void IslandChunkSystem::setVoxelInIsland(uint32_t islandID, const Vec3& islandRelativePosition, uint8_t voxelType)
{
    // Acquire chunk pointer under lock, then perform heavy work without holding the map mutex
    VoxelChunk* chunk = nullptr;
    Vec3 localPos;
    Vec3 chunkCoord;
    Vec3 islandCenter;
    bool isNewChunk = false;
    {
        std::lock_guard<std::mutex> lock(m_islandsMutex);
        auto itIsl = m_islands.find(islandID);
        if (itIsl == m_islands.end())
            return;
        FloatingIsland& island = itIsl->second;
        chunkCoord = FloatingIsland::islandPosToChunkCoord(islandRelativePosition);
        localPos = FloatingIsland::islandPosToLocalPos(islandRelativePosition);
        islandCenter = island.physicsCenter;
        std::unique_ptr<VoxelChunk>& chunkPtr = island.chunks[chunkCoord];
        if (!chunkPtr)
        {
            chunkPtr = std::make_unique<VoxelChunk>();
            chunkPtr->setIslandContext(islandID, chunkCoord);
            chunkPtr->setIsClient(m_isClient);  // Inherit client flag from island system
            isNewChunk = true;
        }
        chunk = chunkPtr.get();
    }

    // Set voxel outside of islands mutex to avoid deadlocks
    int x = static_cast<int>(localPos.x);
    int y = static_cast<int>(localPos.y);
    int z = static_cast<int>(localPos.z);
    if (x < 0 || x >= VoxelChunk::SIZE || y < 0 || y >= VoxelChunk::SIZE || z < 0 || z >= VoxelChunk::SIZE)
        return;
    
    chunk->setVoxel(x, y, z, voxelType);
    // Note: mesh regeneration is handled by the caller (GameClient or GameServer)
    // to allow batch updates and neighbor chunk updates
}

void IslandChunkSystem::setVoxelDataOnly(uint32_t islandID, const Vec3& islandRelativePosition, uint8_t voxelType)
{
    // SERVER-ONLY: Modify voxel data WITHOUT triggering any mesh operations
    // This method directly modifies the voxel array and never calls chunk->setVoxel()
    VoxelChunk* chunk = nullptr;
    Vec3 localPos;
    Vec3 chunkCoord;
    {
        std::lock_guard<std::mutex> lock(m_islandsMutex);
        auto itIsl = m_islands.find(islandID);
        if (itIsl == m_islands.end())
            return;
        FloatingIsland& island = itIsl->second;
        chunkCoord = FloatingIsland::islandPosToChunkCoord(islandRelativePosition);
        localPos = FloatingIsland::islandPosToLocalPos(islandRelativePosition);
        std::unique_ptr<VoxelChunk>& chunkPtr = island.chunks[chunkCoord];
        if (!chunkPtr)
        {
            chunkPtr = std::make_unique<VoxelChunk>();
            chunkPtr->setIslandContext(islandID, chunkCoord);
            chunkPtr->setIsClient(false);  // Server chunks are never client chunks
        }
        chunk = chunkPtr.get();
    }

    // Directly modify voxel data without calling chunk->setVoxel()
    int x = static_cast<int>(localPos.x);
    int y = static_cast<int>(localPos.y);
    int z = static_cast<int>(localPos.z);
    if (x < 0 || x >= VoxelChunk::SIZE || y < 0 || y >= VoxelChunk::SIZE || z < 0 || z >= VoxelChunk::SIZE)
        return;
    
    // Use the direct data modification method (no mesh operations)
    chunk->setVoxelDataDirect(x, y, z, voxelType);
}

void IslandChunkSystem::setVoxelWithAutoChunk(uint32_t islandID, const Vec3& islandRelativePos, uint8_t voxelType)
{
    // Acquire chunk pointer under lock, then write outside the lock
    VoxelChunk* chunk = nullptr;
    int localX = 0, localY = 0, localZ = 0;
    {
        std::lock_guard<std::mutex> lock(m_islandsMutex);
        auto itIsl = m_islands.find(islandID);
        if (itIsl == m_islands.end())
            return;
        FloatingIsland& island = itIsl->second;

        int chunkX = static_cast<int>(std::floor(islandRelativePos.x / VoxelChunk::SIZE));
        int chunkY = static_cast<int>(std::floor(islandRelativePos.y / VoxelChunk::SIZE));
        int chunkZ = static_cast<int>(std::floor(islandRelativePos.z / VoxelChunk::SIZE));
        Vec3 chunkCoord(chunkX, chunkY, chunkZ);

        // Calculate local coordinates correctly for negative positions
        // Use the same floor-based calculation to keep coordinates consistent
        localX = static_cast<int>(std::floor(islandRelativePos.x)) - (chunkX * VoxelChunk::SIZE);
        localY = static_cast<int>(std::floor(islandRelativePos.y)) - (chunkY * VoxelChunk::SIZE);
        localZ = static_cast<int>(std::floor(islandRelativePos.z)) - (chunkZ * VoxelChunk::SIZE);

        std::unique_ptr<VoxelChunk>& chunkPtr = island.chunks[chunkCoord];
        if (!chunkPtr) {
            chunkPtr = std::make_unique<VoxelChunk>();
            chunkPtr->setIslandContext(islandID, chunkCoord);
            chunkPtr->setIsClient(m_isClient);  // Inherit client flag from island system
        }
        chunk = chunkPtr.get();
    }

    if (!chunk) return;
    chunk->setVoxel(localX, localY, localZ, voxelType);
    // Mesh generation can be deferred/batched; leave as-is to avoid stutters
}

// ID-based block methods (clean and efficient)
void IslandChunkSystem::setBlockIDWithAutoChunk(uint32_t islandID, const Vec3& islandRelativePos, uint8_t blockID)
{
    setVoxelWithAutoChunk(islandID, islandRelativePos, blockID);
}

uint8_t IslandChunkSystem::getBlockIDInIsland(uint32_t islandID, const Vec3& islandRelativePosition) const
{
    return getVoxelFromIsland(islandID, islandRelativePosition);
}

void IslandChunkSystem::getAllChunks(std::vector<VoxelChunk*>& outChunks)
{
    outChunks.clear();
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    for (auto& [id, island] : m_islands)
    {
        // Add all chunks from this island
        for (auto& [chunkCoord, chunk] : island.chunks)
        {
            if (chunk)
            {
                outChunks.push_back(chunk.get());
            }
        }
    }
}

void IslandChunkSystem::getVisibleChunks(const Vec3& viewPosition,
                                         std::vector<VoxelChunk*>& outChunks)
{
    // Frustum culling will be added when we implement proper camera frustum
    getAllChunks(outChunks);
}

void IslandChunkSystem::updateIslandPhysics(float deltaTime)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    for (auto& [id, island] : m_islands)
    {
        // Track if island actually moved/rotated this frame
        bool moved = false;
        
        // OPTIMIZATION: Only update if island is actually moving
        float velocityMagnitudeSq = island.velocity.x * island.velocity.x +
                                    island.velocity.y * island.velocity.y +
                                    island.velocity.z * island.velocity.z;
        
        if (velocityMagnitudeSq > 0.0001f) // Threshold: ~0.01 units/sec
        {
            island.physicsCenter.x += island.velocity.x * deltaTime;
            island.physicsCenter.y += island.velocity.y * deltaTime;
            island.physicsCenter.z += island.velocity.z * deltaTime;
            moved = true;
        }
        
        // OPTIMIZATION: Only update rotation if island is actually rotating
        float angularMagnitudeSq = island.angularVelocity.x * island.angularVelocity.x +
                                   island.angularVelocity.y * island.angularVelocity.y +
                                   island.angularVelocity.z * island.angularVelocity.z;
        
        if (angularMagnitudeSq > 0.0001f) // Threshold: ~0.01 rad/sec
        {
            island.rotation.x += island.angularVelocity.x * deltaTime;
            island.rotation.y += island.angularVelocity.y * deltaTime;
            island.rotation.z += island.angularVelocity.z * deltaTime;
            moved = true;
        }
        
        // Only mark for GPU update if island actually moved
        if (moved)
        {
            island.needsPhysicsUpdate = true;
        }
    }
}

void IslandChunkSystem::updatePlayerChunks(const Vec3& playerPosition)
{
    // Infinite world generation will be implemented in a future version
    // For now, we manually create islands in GameState
}

void IslandChunkSystem::generateChunksAroundPoint(const Vec3& center)
{
    // Chunk generation around points will be used for infinite world expansion
    // Currently handled manually through createIsland()
}


