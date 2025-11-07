// ClientWorld.h - Client-side world with prediction and rendering
#pragma once

#include "SimulationState.h"
#include <unordered_map>

/**
 * ClientWorld wraps SimulationState and adds client-specific features:
 * - Client-side prediction tracking
 * - Mesh generation callbacks
 * - Interpolation/smoothing
 * - NO server simulation (fluids, etc.)
 */
class ClientWorld {
public:
    ClientWorld();
    ~ClientWorld();
    
    // ================================
    // INITIALIZATION
    // ================================
    
    bool initialize(bool createDefaultWorld = true);
    void shutdown();
    
    // ================================
    // SIMULATION UPDATE
    // ================================
    
    /**
     * Update client simulation (physics, interpolation)
     * Does NOT run fluid simulation - that's server-only
     */
    void update(float deltaTime);
    
    // ================================
    // CLIENT-SIDE PREDICTION
    // ================================
    
    struct PendingVoxelChange {
        uint32_t islandID;
        Vec3 localPos;
        uint8_t predictedType;
        uint8_t previousType;
    };
    
    /**
     * Apply client-side predicted voxel change (for responsive input)
     */
    uint32_t applyPredictedVoxelChange(uint32_t islandID, const Vec3& localPos, uint8_t voxelType, uint32_t sequenceNumber);
    
    /**
     * Reconcile with server's authoritative update
     */
    void reconcileVoxelChange(uint32_t sequenceNumber, uint32_t islandID, const Vec3& localPos, uint8_t voxelType);
    
    /**
     * Apply server-authoritative voxel change (non-predicted)
     */
    void applyServerVoxelChange(uint32_t islandID, const Vec3& localPos, uint8_t voxelType);
    
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
    std::unordered_map<uint32_t, PendingVoxelChange> m_pendingVoxelChanges;
    bool m_initialized = false;
};
