// GameState.h - Core game world state management
// This class manages all game world data independently of rendering/input
#pragma once

#include "../Math/Vec3.h"
#include "../World/IslandChunkSystem.h"
#include "../Physics/PhysicsSystem.h"  // Re-enabled with fixed BodyID handling
#include <memory>
#include <vector>

/**
 * GameState manages the authoritative game world state.
 * This class is designed to be used by both client and server,
 * with the server being the authoritative source.
 * 
 * Key design principles:
 * - No rendering dependencies (can run headless)
 * - No input dependencies (input is fed in via methods)
 * - Thread-safe where possible
 * - Deterministic simulation
 */
class GameState {
public:
    GameState();
    ~GameState();
    
    // ================================
    // INITIALIZATION & SHUTDOWN
    // ================================
    
    /**
     * Initialize the game state with default world
     * @param shouldCreateDefaultWorld - Whether to create the standard 3-island world
     */
    bool initialize(bool shouldCreateDefaultWorld = true);
    
    /**
     * Shutdown and cleanup all systems
     */
    void shutdown();
    
    // ================================
    // SIMULATION UPDATE
    // ================================
    
    /**
     * Update the game world simulation
     * @param deltaTime - Time step for this update
     */
    void updateSimulation(float deltaTime);
    
    // ================================
    // PLAYER MANAGEMENT
    // ================================
    
    /**
     * Set player position (typically called from input system)
     */
    void setPrimaryPlayerPosition(const Vec3& position);
    
    // ================================
    // WORLD ACCESS
    // ================================
    
    /**
     * Get the island system for world queries
     */
    IslandChunkSystem* getIslandSystem() { return &m_islandSystem; }
    const IslandChunkSystem* getIslandSystem() const { return &m_islandSystem; }
    
    /**
     * Get physics system
     */
    PhysicsSystem* getPhysicsSystem() { return m_physicsSystem.get(); }
    const PhysicsSystem* getPhysicsSystem() const { return m_physicsSystem.get(); }
    
    // ================================
    // WORLD MODIFICATION
    // ================================
    
    /**
     * Set a voxel in the world (for block breaking/placing)
     */
    bool setVoxel(uint32_t islandID, const Vec3& localPos, uint8_t voxelType);
    
    /**
     * Get a voxel from the world
     */
    uint8_t getVoxel(uint32_t islandID, const Vec3& localPos) const;
    
    // ================================
    // WORLD QUERIES
    // ================================
    
    /**
     * Get the center position of an island
     */
    Vec3 getIslandCenter(uint32_t islandID) const;
    
    /**
     * Get all islands for rendering/networking
     */
    const std::vector<uint32_t>& getAllIslandIDs() const { return m_islandIDs; }
    
    /**
     * Get the calculated player spawn position (set during world generation)
     */
    Vec3 getPlayerSpawnPosition() const { return m_playerSpawnPosition; }
    
    /**
     * Update physics systems (called by GameServer with server physics)
     */
    void updatePhysics(float deltaTime, PhysicsSystem* physics);
    
private:
    // Core systems
    IslandChunkSystem m_islandSystem;
    std::unique_ptr<PhysicsSystem> m_physicsSystem;  // Re-enabled with fixed BodyID handling
    
    // World state
    std::vector<uint32_t> m_islandIDs;  // Track all created islands
    Vec3 m_playerSpawnPosition;          // Calculated spawn position above first island
    
    // State flags
    bool m_initialized = false;
    
    // ================================
    // INTERNAL METHODS
    // ================================
    
    /**
     * Create the default 3-island world for testing
     */
    void createDefaultWorld();
    
    /**
     * Update player systems
     */
    void updatePlayer(float deltaTime);
};
