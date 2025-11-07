// ServerWorld.cpp - Server-side world implementation
#include "pch.h"
#include "ServerWorld.h"
#include "../World/BlockType.h"
#include <iostream>

// External global systems
extern FluidSystem g_fluidSystem;
extern ECSWorld g_ecs;

ServerWorld::ServerWorld()
{
}

ServerWorld::~ServerWorld()
{
    shutdown();
}

bool ServerWorld::initialize(bool createDefaultWorld)
{
    if (m_initialized)
    {
        std::cerr << "ServerWorld already initialized!" << std::endl;
        return false;
    }

    std::cout << "[SERVER] Initializing ServerWorld..." << std::endl;

    // Initialize base simulation
    if (!m_simulation.initialize(createDefaultWorld))
    {
        std::cerr << "[SERVER] Failed to initialize simulation state!" << std::endl;
        return false;
    }

    // SERVER-ONLY: Initialize fluid system
    g_fluidSystem.initialize(m_simulation.getIslandSystem(), &g_ecs, m_simulation.getPhysicsSystem());
    std::cout << "[SERVER] Fluid system initialized" << std::endl;

    m_initialized = true;
    return true;
}

void ServerWorld::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    std::cout << "[SERVER] Shutting down ServerWorld..." << std::endl;
    m_simulation.shutdown();
    m_initialized = false;
}

void ServerWorld::update(float deltaTime)
{
    if (!m_initialized)
    {
        return;
    }

    // Update base simulation (physics, islands)
    m_simulation.updateSimulation(deltaTime);

    // SERVER-ONLY: Update fluid simulation
    g_fluidSystem.update(deltaTime);
}

void ServerWorld::updatePhysics(float deltaTime, PhysicsSystem* physics)
{
    if (!m_initialized)
    {
        return;
    }
    
    // Update simulation with provided physics system
    m_simulation.updatePhysics(deltaTime, physics);
}

void ServerWorld::updateSimulation(float deltaTime)
{
    if (!m_initialized)
    {
        return;
    }
    
    // Delegate to base simulation
    m_simulation.updateSimulation(deltaTime);
}

void ServerWorld::updateIslandActivation(const Vec3& playerPosition)
{
    if (!m_initialized)
    {
        return;
    }
    
    // Delegate to base simulation
    m_simulation.updateIslandActivation(playerPosition);
}

bool ServerWorld::setVoxelAuthoritative(uint32_t islandID, const Vec3& localPos, uint8_t voxelType)
{
    if (!m_initialized)
    {
        return false;
    }

    std::cout << "[SERVER] Authoritative voxel change - island " << islandID << " pos (" 
              << localPos.x << ", " << localPos.y << ", " << localPos.z 
              << ") type=" << (int)voxelType << std::endl;
    
    // Get old voxel type for water activation
    uint8_t oldVoxelType = m_simulation.getIslandSystem()->getVoxelFromIsland(islandID, localPos);
    
    // SERVER-ONLY PATH: Direct data modification, NO mesh operations
    m_simulation.getIslandSystem()->setVoxelServerOnly(islandID, localPos, voxelType);
    
    // SERVER-ONLY: Convert nearby water voxels to particles when breaking ANY block
    if (oldVoxelType != BlockID::AIR && voxelType == BlockID::AIR) {
        std::cout << "[SERVER] Block broken, checking neighbors for water..." << std::endl;
        
        // Check neighbors for water blocks
        std::vector<Vec3> neighborOffsets = {
            Vec3(1, 0, 0), Vec3(-1, 0, 0),
            Vec3(0, 1, 0), Vec3(0, -1, 0),
            Vec3(0, 0, 1), Vec3(0, 0, -1)
        };
        
        for (const Vec3& offset : neighborOffsets) {
            Vec3 neighborPos = localPos + offset;
            uint8_t neighborVoxel = m_simulation.getIslandSystem()->getVoxelFromIsland(islandID, neighborPos);
            
            if (neighborVoxel == BlockID::WATER) {
                std::cout << "[SERVER] Found water neighbor at (" << neighborPos.x << ", " 
                          << neighborPos.y << ", " << neighborPos.z << ") - waking it" << std::endl;
                g_fluidSystem.wakeFluidVoxel(islandID, neighborPos);
            }
        }
    }
    
    return true;
}
