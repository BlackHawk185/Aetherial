// SimulationState.cpp - Pure game simulation implementation
#include "pch.h"
#include "SimulationState.h"
#include "../World/BlockType.h"
#include "../World/VoronoiIslandPlacer.h"
#include "../World/VoxelChunk.h"
#include <iostream>

SimulationState::SimulationState()
{
}

SimulationState::~SimulationState()
{
    shutdown();
}

bool SimulationState::initialize(bool createDefaultWorld)
{
    if (m_initialized)
    {
        std::cerr << "SimulationState already initialized!" << std::endl;
        return false;
    }

    std::cout << "🌍 Initializing SimulationState..." << std::endl;

    // Set static island system pointer for inter-chunk queries
    VoxelChunk::setIslandSystem(&m_islandSystem);
    
    // Initialize physics system
    m_physicsSystem = std::make_unique<PhysicsSystem>();
    m_physicsSystem->setIslandSystem(&m_islandSystem);

    // Create default world if requested (parameter name shadows method name, so use 'this->')
    if (createDefaultWorld)
    {
        this->createDefaultWorld();
    }

    m_initialized = true;
    return true;
}

void SimulationState::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    std::cout << "🔄 Shutting down SimulationState..." << std::endl;

    // Clear island data
    m_islandIDs.clear();

    // Physics system will be shut down automatically when destroyed

    m_initialized = false;
}

void SimulationState::updateSimulation(float deltaTime)
{
    if (!m_initialized)
    {
        return;
    }

    // Update player
    updatePlayer(deltaTime);

    // Update island physics
    m_islandSystem.updateIslandPhysics(deltaTime);
}

void SimulationState::updatePhysics(float deltaTime, PhysicsSystem* physics)
{
    if (!m_initialized || !physics)
    {
        return;
    }
    
    // Update using provided physics system
    physics->update(deltaTime);
}

void SimulationState::updateIslandActivation(const Vec3& playerPosition)
{
    if (!m_initialized)
    {
        return;
    }
    
    // Update primary player position for tracking
    m_primaryPlayerPosition = playerPosition;
    
    // Island activation is handled by the island system automatically based on chunk access
    // No need to explicitly activate - chunks are activated when accessed
}

void SimulationState::updatePlayer(float deltaTime)
{
    // Base implementation does nothing - override in Server/ClientWorld
    (void)deltaTime;
}

uint8_t SimulationState::getVoxel(uint32_t islandID, const Vec3& localPos) const
{
    return m_islandSystem.getVoxelFromIsland(islandID, localPos);
}

Vec3 SimulationState::getIslandCenter(uint32_t islandID) const
{
    return m_islandSystem.getIslandCenter(islandID);
}

void SimulationState::createDefaultWorld()
{
    std::cout << "🏝️ Creating procedural world with Voronoi island placement..." << std::endl;

    // World generation config
    struct WorldGenConfig {
        uint32_t worldSeed = 12345;
        float regionSize = 3000.0f;
        float islandDensity = 3.0f;
        float minIslandRadius = 100.0f;
        float maxIslandRadius = 200.0f;
        float minVerticalOffset = 50.0f;
        float maxVerticalOffset = 200.0f;
        float islandSpacing = 350.0f;
    } config;

    // Calculate island count based on density
    float area = config.regionSize * config.regionSize;
    float referenceArea = 1000.0f * 1000.0f;
    int islandCount = static_cast<int>((area / referenceArea) * config.islandDensity);
    std::cout << "📊 Generating " << islandCount << " islands in " << config.regionSize 
              << "x" << config.regionSize << " region (density=" << config.islandDensity << ")" << std::endl;

    // Generate Voronoi island placement
    VoronoiIslandPlacer placer;
    std::vector<IslandDefinition> islandDefs = placer.generateIslands(
        config.worldSeed,
        config.regionSize,
        config.islandDensity,
        config.minIslandRadius,
        config.maxIslandRadius
    );

    std::cout << "✅ Generated " << islandDefs.size() << " island definitions" << std::endl;

    // Create islands from definitions
    for (size_t i = 0; i < islandDefs.size(); i++)
    {
        const auto& def = islandDefs[i];
        uint32_t islandID = m_islandSystem.createIsland(def.position);
        m_islandIDs.push_back(islandID);

        std::cout << "[REALIZE] Island " << islandID 
                  << " @ (" << def.position.x << ", " << def.position.y << ", " << def.position.z << ")"
                  << " radius=" << def.radius << std::endl;

        // Generate voxel terrain for this island
        m_islandSystem.generateFloatingIslandOrganic(islandID, def.seed, def.radius, def.biome);
    }

    // Set spawn position to first island
    if (!m_islandIDs.empty())
    {
        Vec3 firstIslandCenter = m_islandSystem.getIslandCenter(m_islandIDs[0]);
        m_playerSpawnPosition = firstIslandCenter + Vec3(0, 100, 0);
    }

    std::cout << "🎮 Player spawn position: (" << m_playerSpawnPosition.x << ", " 
              << m_playerSpawnPosition.y << ", " << m_playerSpawnPosition.z << ")" << std::endl;
}
