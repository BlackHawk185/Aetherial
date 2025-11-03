// GameState.cpp - Core game world state management implementation
#include "GameState.h"

#include <iostream>
#include <cstdlib>
#include <ctime>
#include <future>
#include <vector>

#include "../World/VoxelChunk.h"
#include "../World/VoronoiIslandPlacer.h"
#include "../Rendering/GlobalLightingManager.h"

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

    // Configure lighting system for maximum performance
    g_globalLighting.setUpdateFrequency(20.0f);   // Increased from 10 FPS to 20 FPS for smoother lighting
    g_globalLighting.setOcclusionEnabled(false);  // Disable occlusion for maximum performance
    
    std::cout << "💡 Configured lighting: Simple face-orientation lighting at 10 FPS for performance" << std::endl;

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

    // Update physics first
    updatePhysics(deltaTime);

    // Update player
    updatePlayer(deltaTime);

    // Update island physics
    m_islandSystem.updateIslandPhysics(deltaTime);
    
    // NOTE: syncPhysicsToChunks() is called by GameClient, not here
    // Server doesn't have a renderer, so it shouldn't sync physics to rendering
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
        float regionSize = 3000.0f;          // World region size (1000x1000 units)
        
        // Island generation (density-based for infinite scaling)
        float islandDensity = 2.0f;          // Islands per 1000x1000 area (scales infinitely!)
        float minIslandRadius = 100.0f;       // Minimum island size
        float maxIslandRadius = 750.0f;      // Maximum island size
        
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
    
    // Create islands from definitions
    std::vector<uint32_t> islandIDs;
    islandIDs.reserve(islandDefs.size());
    
    for (const auto& def : islandDefs) {
        uint32_t islandID = m_islandSystem.createIsland(def.position);
        islandIDs.push_back(islandID);
        m_islandIDs.push_back(islandID);
        
        std::cout << "[WORLD] Island " << islandID 
                  << " @ (" << def.position.x << ", " << def.position.y << ", " << def.position.z << ")"
                  << " radius=" << def.radius << std::endl;
    }
    
    // **PARALLEL ISLAND GENERATION** using std::async
    std::cout << "[WORLD] Generating islands in parallel..." << std::endl;
    
    std::vector<std::future<void>> generationFutures;
    generationFutures.reserve(islandDefs.size());
    
    // Launch all island generations asynchronously
    for (size_t i = 0; i < islandDefs.size(); ++i) {
        const IslandDefinition& def = islandDefs[i];
        uint32_t islandID = islandIDs[i];
        
        generationFutures.push_back(std::async(std::launch::async, 
            [this, islandID, def]() {
                std::cout << "[WORLD] Generating island " << islandID 
                          << " (radius=" << def.radius << ")..." << std::endl;
                m_islandSystem.generateFloatingIslandOrganic(islandID, def.seed, def.radius);
            }
        ));
    }
    
    // Wait for all islands to finish generating
    for (auto& future : generationFutures)
    {
        future.get();
    }
    
    std::cout << "[WORLD] All islands generated!" << std::endl;

    // Log collision mesh generation for each island
    for (uint32_t islandID : m_islandIDs)
    {
        const FloatingIsland* island = m_islandSystem.getIsland(islandID);
        if (island)
        {
            // Count solid voxels across all chunks in this island
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
            
            std::cout << "[SERVER] Island " << islandID << " has " << totalChunks << " chunks with " << solidVoxels << " solid voxels total" << std::endl;
            
            // Log collision mesh info from first chunk (for backward compatibility)
            if (!island->chunks.empty())
            {
                const auto& firstChunk = island->chunks.begin()->second;
                auto mesh = firstChunk->getCollisionMesh();
                std::cout << "[SERVER] Generated island " << islandID << " with collision mesh ("
                          << (mesh ? mesh->faces.size() : 0) << " faces in first chunk)"
                          << std::endl;
            }
        }
    }
    
    // Calculate player spawn position above the first island
    m_playerSpawnPosition = Vec3(0.0f, 64.0f, 0.0f);  // Default fallback
    
    if (!islandDefs.empty()) {
        // Spawn above the first island
        Vec3 firstIslandCenter = islandDefs[0].position;
        m_playerSpawnPosition = Vec3(firstIslandCenter.x, firstIslandCenter.y + 64.0f, firstIslandCenter.z);
    }

    std::cout << "🎯 Player spawn: (" << m_playerSpawnPosition.x << ", " 
              << m_playerSpawnPosition.y << ", " << m_playerSpawnPosition.z << ")" << std::endl;
}

void GameState::updatePhysics(float deltaTime)
{
    // Update generic entity physics (including fluid particles)
    g_physics.update(deltaTime);
}

void GameState::updatePlayer(float deltaTime)
{
    // Player update is now managed by PlayerController in GameClient
    (void)deltaTime;
}
