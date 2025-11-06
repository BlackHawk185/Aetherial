// GameState.cpp - Core game world state management implementation
#include "GameState.h"

#include <iostream>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <future>
#include <vector>

#include "../World/VoxelChunk.h"
#include "../World/VoronoiIslandPlacer.h"

GameState::GameState()
{
    // Constructor - minimal initialization
}

GameState::~GameState()
{
    shutdown();
}

bool GameState::initialize(bool shouldCreateDefaultWorld)
{
    if (m_initialized)
    {
        std::cerr << "GameState already initialized!" << std::endl;
        return false;
    }

    std::cout << "🌍 Initializing GameState..." << std::endl;

    // Set static island system pointer for inter-chunk culling
    VoxelChunk::setIslandSystem(&m_islandSystem);
    
    // Initialize physics system - Re-enabled with fixed BodyID handling
    m_physicsSystem = std::make_unique<PhysicsSystem>();

    // Real-time CSM/PCF shadows - no lightmap system needed
    std::cout << "💡 Using real-time CSM shadows (no lightmap system)" << std::endl;

    // Create default world if requested
    if (shouldCreateDefaultWorld)
    {
        createDefaultWorld();
    }

    m_initialized = true;
    return true;
}

void GameState::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    std::cout << "🔄 Shutting down GameState..." << std::endl;

    // Clear island data
    m_islandIDs.clear();

    // Physics system will be shut down automatically when destroyed

    m_initialized = false;
    std::cout << "✅ GameState shutdown complete" << std::endl;
}

void GameState::updateSimulation(float deltaTime)
{
    if (!m_initialized)
    {
        return;
    }

    // Physics is now updated by GameServer with its own physics instance
    // (not called here to avoid global g_physics dependency)

    // Update player
    updatePlayer(deltaTime);

    // Update island physics
    m_islandSystem.updateIslandPhysics(deltaTime);
    
    // NOTE: syncPhysicsToChunks() is called by GameClient, not here
    // Server doesn't have a renderer, so it shouldn't sync physics to rendering
}

void GameState::updateIslandActivation(const Vec3& playerPosition)
{
    // Check if player has moved significantly
    float movementDistance = (playerPosition - m_lastPlayerPosition).length();
    if (movementDistance < 10.0f && !m_realizedIslandIndices.empty())
    {
        return;  // Skip check if player hasn't moved much
    }
    
    m_lastPlayerPosition = playerPosition;
    
    // Check each unrealized island
    for (size_t i = 0; i < m_islandDefinitions.size(); ++i)
    {
        // Skip if already realized
        if (m_realizedIslandIndices.count(i) > 0)
            continue;
        
        const IslandDefinition& def = m_islandDefinitions[i];
        float distance = (def.position - playerPosition).length();
        
        // Activate if within range
        if (distance < m_islandActivationRadius)
        {
            std::cout << "[ACTIVATION] Realizing island " << i 
                      << " at distance " << distance << " units" << std::endl;
            realizeIsland(i);
        }
    }
}

void GameState::setPrimaryPlayerPosition(const Vec3& position)
{
    // Player position is now managed by PlayerController in GameClient
    (void)position;
}

bool GameState::setVoxel(uint32_t islandID, const Vec3& localPos, uint8_t voxelType)
{
    // Delegate to island system
    m_islandSystem.setVoxelInIsland(islandID, localPos, voxelType);
    return true;  // Error handling will be added when async operations are implemented
}

uint8_t GameState::getVoxel(uint32_t islandID, const Vec3& localPos) const
{
    // Delegate to island system
    return m_islandSystem.getVoxelFromIsland(islandID, localPos);
}

Vec3 GameState::getIslandCenter(uint32_t islandID) const
{
    return m_islandSystem.getIslandCenter(islandID);
}

void GameState::createDefaultWorld()
{
    std::cout << "🏝️ Creating procedural world with Voronoi island placement..." << std::endl;

    // ═══════════════════════════════════════════════════════════════
    // VORONOI WORLD GENERATION CONFIG - Centralized for easy tuning
    // ═══════════════════════════════════════════════════════════════
    struct WorldGenConfig {
        // World boundaries
        float regionSize = 3000.0f;          // World size/region size (1000x1000 units)
        
        // Island generation (density-based for infinite scaling)
        float islandDensity = 3.0f;          // Islands per 1000x1000 area (scales infinitely!)
        float minIslandRadius = 100.0f;       // Minimum island size
        float maxIslandRadius = 1500.0f;      // Maximum island size
        
        // Advanced Voronoi tuning
        float verticalSpread = 100.0f;       // Vertical Y-axis spread (±units)
        float heightNoiseFreq = 0.005f;      // Frequency for Y variation (lower = smoother)
        float cellThreshold = 0.1f;          // Cell center detection (lower = stricter)
        
        // Derived properties (calculated automatically by VoronoiIslandPlacer):
        // - Actual island count = islandDensity * (regionSize/1000)²
        // - Cell size ≈ 1000 / sqrt(islandDensity)
        // - For 1000x1000 region with density 8: ~8 islands, cell size ~354 units
        // - For 2000x2000 region with density 8: ~32 islands, cell size ~354 units (scales!)
    } config;
    
    uint32_t worldSeed = static_cast<uint32_t>(std::time(nullptr));  // Use time as master seed
    
    // Calculate actual island count for this region
    float areaMultiplier = (config.regionSize * config.regionSize) / (1000.0f * 1000.0f);
    int expectedIslands = static_cast<int>(config.islandDensity * areaMultiplier);
    
    std::cout << "[WORLD] World seed: " << worldSeed << std::endl;
    std::cout << "[WORLD] Region: " << config.regionSize << "x" << config.regionSize << std::endl;
    std::cout << "[WORLD] Island density: " << config.islandDensity << " per 1000² (expecting ~" 
              << expectedIslands << " islands)" << std::endl;
    
    // Generate island definitions using Voronoi cellular noise
    VoronoiIslandPlacer placer;
    placer.verticalSpreadMultiplier = config.verticalSpread;
    placer.heightNoiseFrequency = config.heightNoiseFreq;
    placer.cellCenterThreshold = config.cellThreshold;
    
    std::vector<IslandDefinition> islandDefs = placer.generateIslands(
        worldSeed,
        config.regionSize,
        config.islandDensity,
        config.minIslandRadius,
        config.maxIslandRadius
    );
    
    std::cout << "[WORLD] Voronoi placement generated " << islandDefs.size() << " islands" << std::endl;
    
    // Store island definitions for deferred generation
    m_islandDefinitions = std::move(islandDefs);
    
    // Realize the first island immediately (for player spawn)
    if (!m_islandDefinitions.empty())
    {
        std::cout << "[WORLD] Immediately realizing first island for spawn..." << std::endl;
        realizeIsland(0);  // First island always realized at startup
    }
    
    std::cout << "[WORLD] World generation complete! " << m_islandDefinitions.size() 
              << " islands defined, " << m_realizedIslandIndices.size() << " realized." << std::endl;
    std::cout << "[WORLD] Remaining islands will activate within " 
              << m_islandActivationRadius << " units of player" << std::endl;
    
    // Calculate player spawn position above the first island
    m_playerSpawnPosition = Vec3(0.0f, 64.0f, 0.0f);  // Default fallback
    
    if (!m_islandDefinitions.empty()) {
        // Spawn above the first island
        Vec3 firstIslandCenter = m_islandDefinitions[0].position;
        m_playerSpawnPosition = Vec3(firstIslandCenter.x, firstIslandCenter.y + 64.0f, firstIslandCenter.z);
        m_lastPlayerPosition = m_playerSpawnPosition;  // Initialize player position tracking
    }

    std::cout << "🎯 Player spawn: (" << m_playerSpawnPosition.x << ", " 
              << m_playerSpawnPosition.y << ", " << m_playerSpawnPosition.z << ")" << std::endl;
}

void GameState::updatePhysics(float deltaTime, PhysicsSystem* physics)
{
    if (!physics)
        return;
    
    // Update generic entity physics (including fluid particles)
    physics->update(deltaTime);
}

void GameState::updatePlayer(float deltaTime)
{
    // Player update is now managed by PlayerController in GameClient
    (void)deltaTime;
}

void GameState::realizeIsland(size_t definitionIndex)
{
    if (definitionIndex >= m_islandDefinitions.size())
    {
        std::cerr << "[ERROR] Invalid island definition index: " << definitionIndex << std::endl;
        return;
    }
    
    if (m_realizedIslandIndices.count(definitionIndex) > 0)
    {
        return;  // Already realized
    }
    
    const IslandDefinition& def = m_islandDefinitions[definitionIndex];
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Create island structure
    uint32_t islandID = m_islandSystem.createIsland(def.position);
    m_islandIDs.push_back(islandID);
    
    std::cout << "[REALIZE] Island " << islandID 
              << " @ (" << def.position.x << ", " << def.position.y << ", " << def.position.z << ")"
              << " radius=" << def.radius << std::endl;
    
    // Generate voxels with biome
    m_islandSystem.generateFloatingIslandOrganic(islandID, def.seed, def.radius, def.biome);
    
    // Enable incremental updates on all chunks
    FloatingIsland* island = m_islandSystem.getIsland(islandID);
    if (island)
    {
        for (auto& chunkPair : island->chunks)
        {
            VoxelChunk* chunk = chunkPair.second.get();
            if (chunk)
            {
                chunk->enableIncrementalUpdates();
            }
        }
        
        // Log collision mesh stats
        int solidVoxels = 0;
        int totalChunks = 0;
        
        for (const auto& [chunkCoord, chunk] : island->chunks)
        {
            if (chunk)
            {
                totalChunks++;
                for (int x = 0; x < 32; x++)
                {
                    for (int y = 0; y < 32; y++)
                    {
                        for (int z = 0; z < 32; z++)
                        {
                            if (chunk->getVoxel(x, y, z) > 0)
                                solidVoxels++;
                        }
                    }
                }
            }
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        
        std::cout << "[REALIZE] Island " << islandID << " complete: " 
                  << totalChunks << " chunks, " << solidVoxels << " voxels, "
                  << duration << "ms" << std::endl;
    }
    
    // Mark as realized
    m_realizedIslandIndices.insert(definitionIndex);
}
