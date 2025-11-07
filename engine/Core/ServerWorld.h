// ServerWorld.h - Server-side world management with authority and validation
#pragma once

#include "SimulationState.h"
#include "../World/FluidSystem.h"
#include "../ECS/ECS.h"

/**
 * ServerWorld wraps SimulationState and adds server-specific logic:
 * - Authoritative voxel changes
 * - Fluid simulation
 * - Validation and anti-cheat
 * - NO rendering or mesh operations
 */
class ServerWorld {
public:
    ServerWorld();
    ~ServerWorld();
    
    // ================================
    // INITIALIZATION
    // ================================
    
    bool initialize(bool createDefaultWorld = true);
    void shutdown();
    
    // ================================
    // SIMULATION UPDATE
    // ================================
    
    /**
     * Update server simulation (physics, fluids, game logic)
     */
    void update(float deltaTime);
    
    /**
     * Update physics simulation
     */
    void updatePhysics(float deltaTime, PhysicsSystem* physics);
    
    /**
     * Update world simulation
     */
    void updateSimulation(float deltaTime);
    
    /**
     * Update island activation based on player position
     */
    void updateIslandActivation(const Vec3& playerPosition);
    
    // ================================
    // AUTHORITATIVE VOXEL MODIFICATION
    // ================================
    
    /**
     * Server-authoritative voxel change (data-only, no mesh operations)
     * This is the ONLY way server should modify voxels
     */
    bool setVoxelAuthoritative(uint32_t islandID, const Vec3& localPos, uint8_t voxelType);
    
    // ================================
    // WORLD ACCESS (Read-only)
    // ================================
    
    SimulationState* getSimulation() { return &m_simulation; }
    const SimulationState* getSimulation() const { return &m_simulation; }
    
    IslandChunkSystem* getIslandSystem() { return m_simulation.getIslandSystem(); }
    const IslandChunkSystem* getIslandSystem() const { return m_simulation.getIslandSystem(); }
    
    PhysicsSystem* getPhysicsSystem() { return m_simulation.getPhysicsSystem(); }
    
    uint8_t getVoxel(uint32_t islandID, const Vec3& localPos) const 
    { 
        return m_simulation.getVoxel(islandID, localPos); 
    }
    
    Vec3 getIslandCenter(uint32_t islandID) const 
    { 
        return m_simulation.getIslandCenter(islandID); 
    }
    
    Vec3 getPlayerSpawnPosition() const 
    { 
        return m_simulation.getPlayerSpawnPosition(); 
    }
    
    const std::vector<uint32_t>& getIslandIDs() const 
    { 
        return m_simulation.getIslandIDs(); 
    }
    
private:
    SimulationState m_simulation;
    bool m_initialized = false;
};
