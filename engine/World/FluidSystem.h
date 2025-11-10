// FluidSystem.h
// Noclip pathfinding-based water flow system
// Water particles follow BFS-generated paths to lowest reachable air blocks
// Server-authoritative: all simulation runs server-side, clients only render
#pragma once

#include "../Math/Vec3.h"
#include "../ECS/ECS.h"
#include "IslandChunkSystem.h"
#include "BlockType.h"
#include "FluidComponents.h"

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <functional>

class PhysicsSystem;

// Network callbacks for client synchronization
using FluidParticleSpawnCallback = std::function<void(EntityID, uint32_t islandID, const Vec3& worldPos, const Vec3& velocity, const Vec3& originalVoxelPos)>;
using FluidParticleDespawnCallback = std::function<void(EntityID, uint32_t islandID, const Vec3& settledVoxelPos, bool shouldCreateVoxel)>;
using FluidVoxelChangeCallback = std::function<void(uint32_t islandID, const Vec3& position, uint8_t voxelType)>;

constexpr uint8_t FLUID_VOXEL_TYPE = BlockID::WATER;

struct FluidSettings {
    float gravity = -9.81f;           // Unused (noclip mode)
    float viscosity = 0.05f;          // Unused (noclip mode)
    float surfaceTension = 0.05f;     // Unused (noclip mode)
    float gridAttractionStrength = 2.0f;  // Unused (noclip mode)
    float gridSnapDistance = 0.1f;    // Unused (noclip mode)
    
    // Tug system: cascade activation when particle flows away from source
    float tugRadius = 1.0f;           // Search radius for face-adjacent water neighbors
    float tugDistance = 3.0f;         // Activate neighbor when particle moves >3 blocks away
    int maxTugChainDepth = 10;        // Cascade iteration limit
    
    int maxActiveParticles = 1000;
    int maxParticlesPerFrame = 50;    // Limit activations per frame
    float particleRadius = 0.2f;      // Visual size (0.4 diameter)
};
class FluidSystem {
public:
    FluidSystem();
    ~FluidSystem();
    
    void initialize(IslandChunkSystem* islandSystem, ECSWorld* ecsWorld, PhysicsSystem* physics = nullptr);
    void update(float deltaTime);
    
    // Activation API
    void triggerFluidActivation(uint32_t islandID, const Vec3& islandRelativePos, float disturbanceForce);
    EntityID wakeFluidVoxel(uint32_t islandID, const Vec3& islandRelativePos);  // Voxel → particle
    EntityID convertWaterVoxelToParticle(uint32_t islandID, const Vec3& islandRelativePos);
    void sleepFluidParticle(EntityID particleEntity);  // Particle → voxel
    
    // Queries
    bool isFluidVoxel(uint32_t islandID, const Vec3& islandRelativePos) const;
    int getActiveParticleCount() const;
    int getSleepingVoxelCount() const;
    
    // World generation
    void addSleepingVoxel(uint32_t islandID, const Vec3& islandRelativePos, float tugStrength = 1.0f);
    void removeSleepingVoxel(uint32_t islandID, const Vec3& islandRelativePos);
    
    FluidSettings& getSettings() { return m_settings; }
    const FluidSettings& getSettings() const { return m_settings; }
    
    // Network callbacks (server broadcasts to clients)
    void setParticleSpawnCallback(FluidParticleSpawnCallback callback) { m_onParticleSpawn = callback; }
    void setParticleDespawnCallback(FluidParticleDespawnCallback callback) { m_onParticleDespawn = callback; }
    void setVoxelChangeCallback(FluidVoxelChangeCallback callback) { m_onVoxelChange = callback; }

private:
    IslandChunkSystem* m_islandSystem = nullptr;
    ECSWorld* m_ecsWorld = nullptr;
    PhysicsSystem* m_physics = nullptr;
    FluidSettings m_settings;
    
    std::vector<EntityID> m_activeParticles;
    std::vector<EntityID> m_particlesToSleep;
    std::vector<EntityID> m_particlesToDestroy;
    
    struct WaterToWake { uint32_t islandID; Vec3 position; };
    std::vector<WaterToWake> m_waterToWake;  // Deferred activation queue
    int m_particlesWokenThisFrame = 0;
    
    FluidParticleSpawnCallback m_onParticleSpawn;
    FluidParticleDespawnCallback m_onParticleDespawn;
    FluidVoxelChangeCallback m_onVoxelChange;
    
    // Per-frame update stages
    void updateActiveParticles(float deltaTime);
    void updateParticlePhysics(EntityID, FluidParticleComponent*, TransformComponent*, float deltaTime);
    void updateParticleSettling(EntityID, FluidParticleComponent*, TransformComponent*, float deltaTime);
    void updateParticleTugSystem(EntityID, FluidParticleComponent*, TransformComponent*);
    void processParticleTransitions();
    void cleanupDestroyedParticles();
    void processDeferredWaterActivation();
    
    // Tug cascade system
    void registerNearbyWaterVoxels(FluidParticleComponent*, const Vec3& worldPosition);
    void propagateTugForce(uint32_t islandID, const Vec3& islandRelativePos, float tugForce, int chainDepth = 0);
    std::vector<Vec3> getNeighborPositions(const Vec3& center);
    
    // Pathfinding (floodfill BFS)
    Vec3 calculateGridAlignmentForce(const Vec3& position, const Vec3& velocity);  // Unused
    Vec3 calculatePathfindingForce(const Vec3& worldPosition, uint32_t islandID, FluidParticleComponent*, bool recalculateTarget);
    Vec3 findNearestValidGridPosition(const Vec3& worldPosition);  // Unused
    
    uint64_t hashPosition(uint32_t islandID, const Vec3& islandRelativePos) const;
    bool checkParticleCollision(const Vec3& position, float radius);  // Unused (noclip)
    Vec3 calculateCollisionResponse(const Vec3& position, const Vec3& velocity);  // Unused (noclip)
};

// Global fluid system instance
extern FluidSystem g_fluidSystem;