// FluidSystem.h - Sleeping particle fluid simulation system
#pragma once

#include "../Math/Vec3.h"
#include "../ECS/ECS.h"
#include "IslandChunkSystem.h"
#include "BlockType.h"
#include "FluidComponents.h"  // Shared component definitions

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>

// Forward declarations
class PhysicsSystem;

// Use existing water block type for sleeping fluid voxels
constexpr uint8_t FLUID_VOXEL_TYPE = BlockID::WATER;

// Fluid system settings
struct FluidSettings {
    // Physics constants
    float gravity = -9.81f;
    float viscosity = 0.1f;
    float surfaceTension = 0.05f;
    
    // Grid alignment forces
    float gridAttractionStrength = 2.0f;
    float gridSnapDistance = 0.1f;
    
    // Tug system
    float tugRadius = 1.0f;           // How far to search for neighboring water voxels (1.0 = immediate neighbors only)
    float tugDistance = 0.5f;         // Distance threshold - activate water if particle moves this far away
    int maxTugChainDepth = 10;        // Prevent infinite tug chains
    
    // Performance limits
    int maxActiveParticles = 1000;
    int maxParticlesPerFrame = 50;    // Max particles to wake per frame
    float particleRadius = 0.4f;     // For collision detection
};

// Main fluid simulation system
class FluidSystem {
public:
    FluidSystem();
    ~FluidSystem();
    
    // Initialize with reference to island system, ECS world, and physics system
    void initialize(IslandChunkSystem* islandSystem, ECSWorld* ecsWorld, PhysicsSystem* physics = nullptr);
    
    // Main update loop
    void update(float deltaTime);
    
    // Trigger fluid activation (e.g., when breaking blocks near water)
    void triggerFluidActivation(uint32_t islandID, const Vec3& islandRelativePos, float disturbanceForce);
    
    // Convert sleeping voxel to active particle
    EntityID wakeFluidVoxel(uint32_t islandID, const Vec3& islandRelativePos);
    
    // Convert any water voxel (tracked or untracked) to active particle
    EntityID convertWaterVoxelToParticle(uint32_t islandID, const Vec3& islandRelativePos);
    
    // Convert active particle back to sleeping voxel
    void sleepFluidParticle(EntityID particleEntity);
    
    // Query functions
    bool isFluidVoxel(uint32_t islandID, const Vec3& islandRelativePos) const;
    int getActiveParticleCount() const;
    int getSleepingVoxelCount() const;
    
    // Manual fluid voxel management (for world generation)
    void addSleepingVoxel(uint32_t islandID, const Vec3& islandRelativePos, float tugStrength = 1.0f);
    void removeSleepingVoxel(uint32_t islandID, const Vec3& islandRelativePos);
    
    // Settings access
    FluidSettings& getSettings() { return m_settings; }
    const FluidSettings& getSettings() const { return m_settings; }

private:
    // Core systems
    IslandChunkSystem* m_islandSystem = nullptr;
    ECSWorld* m_ecsWorld = nullptr;
    PhysicsSystem* m_physics = nullptr;
    FluidSettings m_settings;
    
    // Active particle management
    std::vector<EntityID> m_activeParticles;
    std::vector<EntityID> m_particlesToSleep;     // Particles ready to sleep
    std::vector<EntityID> m_particlesToDestroy;   // Particles to clean up
    
    // Performance tracking
    int m_particlesWokenThisFrame = 0;
    
    // Internal update functions
    void updateActiveParticles(float deltaTime);
    void updateParticlePhysics(EntityID particle, FluidParticleComponent* fluidComp, 
                              TransformComponent* transform, float deltaTime);
    void updateParticleSettling(EntityID particle, FluidParticleComponent* fluidComp, 
                               TransformComponent* transform, float deltaTime);
    void updateParticleTugSystem(EntityID particle, FluidParticleComponent* fluidComp,
                                TransformComponent* transform);
    void processParticleTransitions();
    void cleanupDestroyedParticles();
    
    // Tug system - distance based
    void registerNearbyWaterVoxels(FluidParticleComponent* fluidComp, const Vec3& worldPosition);
    void propagateTugForce(uint32_t islandID, const Vec3& islandRelativePos, 
                          float tugForce, int chainDepth = 0);
    std::vector<Vec3> getNeighborPositions(const Vec3& center);
    
    // Grid alignment helpers
    Vec3 calculateGridAlignmentForce(const Vec3& position, const Vec3& velocity);
    Vec3 calculatePathfindingForce(const Vec3& worldPosition, uint32_t islandID, FluidParticleComponent* fluidComp);
    Vec3 findNearestValidGridPosition(const Vec3& worldPosition);
    
    // Utility functions
    uint64_t hashPosition(uint32_t islandID, const Vec3& islandRelativePos) const;
    
    // Collision detection
    bool checkParticleCollision(const Vec3& position, float radius);
    Vec3 calculateCollisionResponse(const Vec3& position, const Vec3& velocity);
};

// Global fluid system instance
extern FluidSystem g_fluidSystem;