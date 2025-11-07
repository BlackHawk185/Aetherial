// SimulationState.h - Pure game simulation without rendering dependencies
// This is the core game world state that can run headless on servers
#pragma once

#include "../Math/Vec3.h"
#include "../World/IslandChunkSystem.h"
#include "../Physics/PhysicsSystem.h"
#include <memory>
#include <vector>

/**
 * SimulationState contains the pure game world state.
 * This class has NO rendering dependencies and can run headless on servers.
 * 
 * Key principles:
 * - No OpenGL, no meshes, no GPU operations
 * - Deterministic simulation
 * - Thread-safe where possible
 * - Used as base for both ServerWorld and ClientWorld
 */
class SimulationState {
public:
    SimulationState();
    virtual ~SimulationState();
    
    // ================================
    // INITIALIZATION
    // ================================
    
    /**
     * Initialize the simulation state
     * @param createDefaultWorld - Whether to generate a default world
     */
    virtual bool initialize(bool createDefaultWorld = true);
    
    /**
     * Shutdown and cleanup
     */
    virtual void shutdown();
    
    // ================================
    // SIMULATION UPDATE
    // ================================
    
    /**
     * Update the game simulation
     * @param deltaTime - Time step for this update
     */
    virtual void updateSimulation(float deltaTime);
    
    /**
     * Update physics simulation
     */
    virtual void updatePhysics(float deltaTime, PhysicsSystem* physics);
    
    /**
     * Update island activation based on player position
     */
    virtual void updateIslandActivation(const Vec3& playerPosition);
    
    // ================================
    // WORLD ACCESS
    // ================================
    
    /**
     * Get the island system
     */
    IslandChunkSystem* getIslandSystem() { return &m_islandSystem; }
    const IslandChunkSystem* getIslandSystem() const { return &m_islandSystem; }
    
    /**
     * Get physics system
     */
    PhysicsSystem* getPhysicsSystem() { return m_physicsSystem.get(); }
    const PhysicsSystem* getPhysicsSystem() const { return m_physicsSystem.get(); }
    
    // ================================
    // VOXEL DATA ACCESS (Read-only)
    // ================================
    
    /**
     * Get a voxel from the world
     */
    uint8_t getVoxel(uint32_t islandID, const Vec3& localPos) const;
    
    /**
     * Get island center position
     */
    Vec3 getIslandCenter(uint32_t islandID) const;
    
    /**
     * Get player spawn position
     */
    Vec3 getPlayerSpawnPosition() const { return m_playerSpawnPosition; }
    
    /**
     * Get list of all island IDs
     */
    const std::vector<uint32_t>& getIslandIDs() const { return m_islandIDs; }
    
protected:
    // ================================
    // WORLD CREATION
    // ================================
    
    /**
     * Create the default game world
     */
    void createDefaultWorld();
    
    /**
     * Update player simulation
     */
    virtual void updatePlayer(float deltaTime);
    
    // ================================
    // INTERNAL STATE
    // ================================
    
    IslandChunkSystem m_islandSystem;
    std::unique_ptr<PhysicsSystem> m_physicsSystem;
    
    Vec3 m_primaryPlayerPosition{0, 0, 0};
    Vec3 m_playerSpawnPosition{0, 0, 0};
    std::vector<uint32_t> m_islandIDs;
    
    bool m_initialized = false;
};
