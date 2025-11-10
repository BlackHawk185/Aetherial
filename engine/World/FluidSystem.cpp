// FluidSystem.cpp
// Noclip pathfinding water flow system
// Particles follow BFS-generated waypoint paths through connected air spaces
// Movement: Pure noclip (position += velocity * dt), no physics or collision
// Pathfinding: Floodfill BFS within 5-block radius, FIFO queue for breadth-first exploration
// Target priority: Lowest reachable → same level horizontal → upward (fallback)
// Settling: Immediate when within 0.5 blocks (3D distance) of pathfinding target
// Tug system: Activates face-adjacent water when particle moves >3.0 blocks away

#include "FluidSystem.h"
#include "VoxelRaycaster.h"
#include "../Profiling/Profiler.h"
#include "../Physics/PhysicsSystem.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>

FluidSystem g_fluidSystem;

FluidSystem::FluidSystem() {}
FluidSystem::~FluidSystem() {}

void FluidSystem::initialize(IslandChunkSystem* islandSystem, ECSWorld* ecsWorld, PhysicsSystem* physics) {
    m_islandSystem = islandSystem;
    m_ecsWorld = ecsWorld;
    m_physics = physics;
    std::cout << "✨ Fluid system initialized" << std::endl;
}

void FluidSystem::update(float deltaTime) {
    PROFILE_SCOPE("FluidSystem::update");
    
    m_particlesWokenThisFrame = 0;
    updateActiveParticles(deltaTime);
    processParticleTransitions();
    cleanupDestroyedParticles();
    
    // Cascade loop: activate water, check tug for NEW particles only (not all particles)
    // Allows cascade to propagate in single frame without O(n²) cost
    int maxCascadeIterations = 10;
    
    for (int iter = 0; iter < maxCascadeIterations; ++iter) {
        if (m_waterToWake.empty()) break;
        
        auto* fluidStorage = m_ecsWorld->getStorage<FluidParticleComponent>();
        auto* transformStorage = m_ecsWorld->getStorage<TransformComponent>();
        if (!fluidStorage || !transformStorage) break;
        
        size_t entityCountBefore = fluidStorage->entities.size();
        
        processDeferredWaterActivation();
        
        // Only check tug system for particles created in THIS iteration
        // New particles are appended to the end of the storage
        for (size_t i = entityCountBefore; i < fluidStorage->entities.size(); ++i) {
            EntityID entity = fluidStorage->entities[i];
            FluidParticleComponent* fluidComp = &fluidStorage->components[i];
            TransformComponent* transform = transformStorage->getComponent(entity);
            
            if (fluidComp->state == FluidState::ACTIVE && transform) {
                updateParticleTugSystem(entity, fluidComp, transform);
            }
        }
    }
}

void FluidSystem::updateActiveParticles(float deltaTime) {
    PROFILE_SCOPE("FluidSystem::updateActiveParticles");
    
    // Get all active particles from ECS
    auto* fluidStorage = m_ecsWorld->getStorage<FluidParticleComponent>();
    auto* transformStorage = m_ecsWorld->getStorage<TransformComponent>();
    
    // Clear active particle list and rebuild it
    m_activeParticles.clear();
    
    for (size_t i = 0; i < fluidStorage->entities.size(); ++i) {
        EntityID entity = fluidStorage->entities[i];
        FluidParticleComponent* fluidComp = &fluidStorage->components[i];
        
        if (fluidComp->state == FluidState::ACTIVE || fluidComp->state == FluidState::SETTLING) {
            m_activeParticles.push_back(entity);
            
            // Get transform component
            TransformComponent* transform = transformStorage->getComponent(entity);
            if (!transform) continue;
            
            // Update particle physics
            updateParticlePhysics(entity, fluidComp, transform, deltaTime);
            
            // Update tug system (check if we need to activate nearby water)
            updateParticleTugSystem(entity, fluidComp, transform);
            
            // Update settling behavior
            updateParticleSettling(entity, fluidComp, transform, deltaTime);
        }
    }
}

// Per-frame particle movement: noclip pathfinding
// 1. Check if need new path (no target or reached target)
// 2. Calculate velocity toward current waypoint (or recalculate path if needed)
// 3. Apply velocity: position += velocity * dt (noclip - no collision checks)
void FluidSystem::updateParticlePhysics(EntityID /* particle */, FluidParticleComponent* fluidComp, 
                                       TransformComponent* transform, float deltaTime) {
    if (!m_physics) return;
    
    bool needsNewTarget = !fluidComp->hasPathfindingTarget;
    
    if (fluidComp->hasPathfindingTarget) {
        FloatingIsland* island = m_islandSystem->getIsland(fluidComp->sourceIslandID);
        if (island) {
            Vec3 islandPos = island->worldToLocal(transform->position);
            Vec3 toTarget = fluidComp->pathfindingTarget - islandPos;
            
            // Check if at target (3D distance)
            bool atTarget = (toTarget.length() < 0.5f);
            needsNewTarget = atTarget;
        }
    }
    
    // Get pathfinding force (returns desired velocity direction)
    Vec3 pathfindingForce = calculatePathfindingForce(transform->position, fluidComp->sourceIslandID, fluidComp, needsNewTarget);
    
    // Set velocity directly to pathfinding force (no physics, no accumulation)
    fluidComp->velocity = pathfindingForce;
    
    // Apply movement (noclip - resolveFluidMovement is pure velocity application now)
    transform->position = m_physics->resolveFluidMovement(
        transform->position,
        fluidComp->velocity,
        deltaTime,
        m_settings.particleRadius
    );
}

// Settling: check if particle reached target, attempt to place water voxel
// Success: particle → voxel, entity destroyed, broadcast to clients
// Failure (occupied): invalidate target, recalculate path next frame, keep flowing
void FluidSystem::updateParticleSettling(EntityID particle, FluidParticleComponent* fluidComp, 
                                        TransformComponent* transform, float deltaTime) {
    fluidComp->aliveTimer += deltaTime;
    if (!m_physics) return;
    
    FloatingIsland* island = m_islandSystem->getIsland(fluidComp->sourceIslandID);
    if (!island) return;
    
    Vec3 islandPos = island->worldToLocal(transform->position);
    
    bool atTarget = false;
    if (fluidComp->hasPathfindingTarget) {
        Vec3 toTarget = fluidComp->pathfindingTarget - islandPos;
        atTarget = toTarget.length() < 0.5f;  // 3D distance check (not just horizontal)
    }
    
    if (atTarget && fluidComp->hasPathfindingTarget) {
        fluidComp->targetGridPos = Vec3(
            std::floor(fluidComp->pathfindingTarget.x),
            std::floor(fluidComp->pathfindingTarget.y),
            std::floor(fluidComp->pathfindingTarget.z)
        );
        
        std::cout << "[FLUID] Particle reached target - settling at (" 
                  << fluidComp->targetGridPos.x << ", " << fluidComp->targetGridPos.y << ", " 
                  << fluidComp->targetGridPos.z << ")" << std::endl;
        
        m_particlesToSleep.push_back(particle);
        fluidComp->hasPathfindingTarget = false;  // Invalidate to prevent re-settling loop
    }
}

void FluidSystem::updateParticleTugSystem(EntityID particle, FluidParticleComponent* fluidComp,
                                         TransformComponent* transform) {
    if (m_particlesWokenThisFrame >= m_settings.maxParticlesPerFrame) return;
    
    // Get the island this particle belongs to
    FloatingIsland* island = m_islandSystem->getIsland(fluidComp->sourceIslandID);
    if (!island) return;
    
    // Convert particle world position back to island-relative position
    Vec3 particleIslandPos = island->worldToLocal(transform->position);
    
    // Check each watched water voxel
    for (size_t i = 0; i < fluidComp->watchedWaterVoxels.size(); ++i) {
        const Vec3& waterVoxelPos = fluidComp->watchedWaterVoxels[i];
        
        // Calculate distance between particle and water voxel (in island space)
        float distance = (particleIslandPos - waterVoxelPos).length();
        
        // If particle has moved more than tugDistance away, queue water for activation
        if (distance > m_settings.tugDistance) {
            // Check if this voxel is still water
            uint8_t voxelType = m_islandSystem->getVoxelFromIsland(fluidComp->sourceIslandID, waterVoxelPos);
            if (voxelType == BlockID::WATER) {
                // Queue this water voxel for activation (deferred to avoid ECS invalidation)
                m_waterToWake.push_back({fluidComp->sourceIslandID, waterVoxelPos});
                m_particlesWokenThisFrame++;
                
                // Remove from watch list (we've queued it for activation)
                fluidComp->watchedWaterVoxels.erase(fluidComp->watchedWaterVoxels.begin() + i);
                --i;  // Adjust index since we removed an element
                
                if (m_particlesWokenThisFrame >= m_settings.maxParticlesPerFrame) break;
            } else {
                // Water is gone, remove from watch list
                fluidComp->watchedWaterVoxels.erase(fluidComp->watchedWaterVoxels.begin() + i);
                --i;
            }
        }
    }
}

void FluidSystem::processParticleTransitions() {
    // Sleep particles that are ready
    for (EntityID particle : m_particlesToSleep) {
        sleepFluidParticle(particle);
    }
    m_particlesToSleep.clear();
}

void FluidSystem::cleanupDestroyedParticles() {
    for (EntityID particle : m_particlesToDestroy) {
        // Get particle data before destroying
        FluidParticleComponent* fluidComp = m_ecsWorld->getComponent<FluidParticleComponent>(particle);
        
        // Notify server to broadcast despawn (particle destroyed, no voxel placement)
        if (m_onParticleDespawn && fluidComp) {
            m_onParticleDespawn(particle, fluidComp->sourceIslandID, Vec3(0, 0, 0), false);
        }
        
        m_ecsWorld->destroyEntity(particle);
    }
    m_particlesToDestroy.clear();
}

void FluidSystem::processDeferredWaterActivation() {
    // Process all queued water activations
    for (const WaterToWake& water : m_waterToWake) {
        wakeFluidVoxel(water.islandID, water.position);
    }
    m_waterToWake.clear();
}

void FluidSystem::triggerFluidActivation(uint32_t islandID, const Vec3& islandRelativePos, float disturbanceForce) {
    // Start tug chain from the disturbance point
    propagateTugForce(islandID, islandRelativePos, disturbanceForce, 0);
}

void FluidSystem::propagateTugForce(uint32_t islandID, const Vec3& islandRelativePos, 
                                   float tugForce, int chainDepth) {
    // Legacy force-based tug system - kept for backward compatibility
    // The new distance-based tug system is now used instead (updateParticleTugSystem)
    // This is only called from triggerFluidActivation which is not currently used
    (void)islandID;
    (void)islandRelativePos;
    (void)tugForce;
    (void)chainDepth;
}

EntityID FluidSystem::wakeFluidVoxel(uint32_t islandID, const Vec3& islandRelativePos) {
    // Safety check: system must be initialized
    if (!m_islandSystem || !m_ecsWorld) {
        std::cerr << "[FLUID] ERROR: FluidSystem not initialized - cannot wake voxel!" << std::endl;
        std::cerr << "[FLUID]   m_islandSystem=" << (void*)m_islandSystem << " m_ecsWorld=" << (void*)m_ecsWorld << std::endl;
        return 0;
    }
    
    std::cout << "[FLUID] wakeFluidVoxel called for island " << islandID << " pos (" 
              << islandRelativePos.x << ", " << islandRelativePos.y << ", " << islandRelativePos.z << ")" << std::endl;
    
    std::cout << "[FLUID] Checking if island exists..." << std::endl;
    // Safety check: Verify island exists
    const FloatingIsland* island = m_islandSystem->getIsland(islandID);
    std::cout << "[FLUID] Island pointer: " << (void*)island << std::endl;
    if (!island) {
        std::cout << "[FLUID] ERROR: Island " << islandID << " does not exist in this island system - aborting wake" << std::endl;
        return 0;
    }
    
    std::cout << "[FLUID] Step 1: Removing voxel..." << std::endl;
    // CRITICAL: Remove voxel FIRST before registering neighbors
    // This ensures we don't register the voxel we just broke
    // Safety: Check if the voxel actually exists first
    std::cout << "[FLUID] Getting existing voxel type..." << std::endl;
    uint8_t existingVoxel = 0;
    try {
        existingVoxel = m_islandSystem->getVoxelFromIsland(islandID, islandRelativePos);
        std::cout << "[FLUID] Existing voxel type: " << (int)existingVoxel << std::endl;
    } catch (...) {
        std::cerr << "[FLUID] EXCEPTION caught while getting voxel!" << std::endl;
        return 0;
    }
    
    if (existingVoxel != BlockID::WATER) {
        std::cout << "[FLUID] WARNING: Expected water at (" << islandRelativePos.x << ", " 
                  << islandRelativePos.y << ", " << islandRelativePos.z 
                  << ") but found blockID=" << (int)existingVoxel << " - aborting wake" << std::endl;
        return 0;
    }
    
    std::cout << "[FLUID] Step 1b: Removing water voxel..." << std::endl;
    try {
        // Use server-only path - no mesh generation (server doesn't need meshes)
        m_islandSystem->setVoxelServerOnly(islandID, islandRelativePos, 0);  // 0 = empty
        std::cout << "[FLUID] Voxel removed successfully" << std::endl;
        
        // Notify server to broadcast voxel removal to clients
        if (m_onVoxelChange) {
            m_onVoxelChange(islandID, islandRelativePos, 0);
        }
    } catch (...) {
        std::cerr << "[FLUID] EXCEPTION caught while setting voxel!" << std::endl;
        return 0;
    }
    
    std::cout << "[FLUID] Step 2: Removing from sleeping voxels..." << std::endl;
    // Remove from sleeping voxels
    removeSleepingVoxel(islandID, islandRelativePos);
    
    std::cout << "[FLUID] Step 3: Creating entity..." << std::endl;
    // Create active particle entity
    EntityID particle = m_ecsWorld->createEntity();
    
    std::cout << "[FLUID] Step 4: Converting to world position..." << std::endl;
    // Convert island-relative position to world position
    Vec3 worldPos = islandRelativePos;
    if (const FloatingIsland* island = m_islandSystem->getIsland(islandID)) {
        // Apply island transform to get world position
        // Center the particle in the voxel (add 0.5 offset)
        glm::vec4 localPos(islandRelativePos.x + 0.5f, islandRelativePos.y + 0.5f, islandRelativePos.z + 0.5f, 1.0f);
        glm::vec4 worldPos4 = island->getTransformMatrix() * localPos;
        worldPos = Vec3(worldPos4.x, worldPos4.y, worldPos4.z);
    }
    
    std::cout << "[FLUID] Step 5: Adding transform component..." << std::endl;
    
    std::cout << "[FLUID] Step 5: Adding transform component..." << std::endl;
    // Add transform component
    TransformComponent transform;
    transform.position = worldPos;
    m_ecsWorld->addComponent(particle, transform);
    
    std::cout << "[FLUID] Step 6: Creating fluid component..." << std::endl;
    // Add fluid component
    FluidParticleComponent fluidComp;
    fluidComp.state = FluidState::ACTIVE;
    fluidComp.velocity = Vec3(0, -0.1f, 0);  // Start with small downward velocity to trigger physics
    fluidComp.sourceIslandID = islandID;
    fluidComp.originalVoxelPos = islandRelativePos;
    fluidComp.chainDepth = 0;
    
    std::cout << "[FLUID] Step 7: Registering nearby water voxels..." << std::endl;
    // Register nearby water voxels for tug system (use island-relative position, NOT world)
    registerNearbyWaterVoxels(&fluidComp, islandRelativePos);
    
    std::cout << "[FLUID] Step 8: Adding fluid component to entity..." << std::endl;
    m_ecsWorld->addComponent(particle, fluidComp);
    
    std::cout << "[FLUID] Particle created at world: (" << worldPos.x << ", " << worldPos.y << ", " << worldPos.z 
              << ") island-rel: (" << islandRelativePos.x << ", " << islandRelativePos.y << ", " << islandRelativePos.z 
              << ") watching " << fluidComp.watchedWaterVoxels.size() << " water voxels" << std::endl;
    
    // Notify server to broadcast spawn to clients
    if (m_onParticleSpawn) {
        m_onParticleSpawn(particle, islandID, worldPos, fluidComp.velocity, islandRelativePos);
    }
    
    return particle;
}

void FluidSystem::registerNearbyWaterVoxels(FluidParticleComponent* fluidComp, const Vec3& islandRelativePos) {
    FloatingIsland* island = m_islandSystem->getIsland(fluidComp->sourceIslandID);
    if (!island) return;
    
    // Use the island-relative position directly (already in the right space)
    Vec3 islandPos = islandRelativePos;
    
    // Check only immediate face-adjacent neighbors (6 directions)
    const Vec3 faceNeighbors[6] = {
        Vec3(1, 0, 0),   // +X
        Vec3(-1, 0, 0),  // -X
        Vec3(0, 1, 0),   // +Y
        Vec3(0, -1, 0),  // -Y
        Vec3(0, 0, 1),   // +Z
        Vec3(0, 0, -1)   // -Z
    };
    
    std::cout << "[FLUID] Scanning for water in 6 face-adjacent neighbors around (" 
              << islandPos.x << ", " << islandPos.y << ", " << islandPos.z << ")" << std::endl;
    
    for (int i = 0; i < 6; i++) {
        Vec3 neighborPos = islandPos + faceNeighbors[i];
        
        // Check if this position has a water voxel
        uint8_t voxelType = m_islandSystem->getVoxelFromIsland(fluidComp->sourceIslandID, neighborPos);
        if (voxelType == BlockID::WATER) {
            fluidComp->watchedWaterVoxels.push_back(neighborPos);
            std::cout << "[FLUID]   - Found water at (" << neighborPos.x << ", " 
                      << neighborPos.y << ", " << neighborPos.z << ")" << std::endl;
        }
    }
    
    std::cout << "[FLUID] Registered " << fluidComp->watchedWaterVoxels.size() << " face-adjacent water voxels for particle" << std::endl;
}

void FluidSystem::sleepFluidParticle(EntityID particleEntity) {
    // Get particle components
    FluidParticleComponent* fluidComp = m_ecsWorld->getComponent<FluidParticleComponent>(particleEntity);
    TransformComponent* transform = m_ecsWorld->getComponent<TransformComponent>(particleEntity);
    
    if (!fluidComp || !transform) {
        std::cout << "[FLUID] ERROR: Cannot sleep particle - missing components" << std::endl;
        return;
    }
    
    // targetGridPos is already in island-relative space (set in updateParticleSettling)
    Vec3 islandRelativePos = fluidComp->targetGridPos;
    uint32_t targetIslandID = fluidComp->sourceIslandID;
    
    std::cout << "[FLUID] Attempting to sleep particle at island " << targetIslandID 
              << " pos (" << islandRelativePos.x << ", " << islandRelativePos.y << ", " << islandRelativePos.z << ")" << std::endl;
    
    // Verify island exists
    FloatingIsland* island = m_islandSystem->getIsland(targetIslandID);
    if (!island) {
        std::cout << "[FLUID] ERROR: Target island " << targetIslandID << " does not exist - destroying particle" << std::endl;
        m_particlesToDestroy.push_back(particleEntity);
        return;
    }
    
    // Check if target position is empty (should be air to place water)
    uint8_t existingVoxel = m_islandSystem->getVoxelFromIsland(targetIslandID, islandRelativePos);
    if (existingVoxel != BlockID::AIR) {
        // Target occupied (another particle beat us to it) - invalidate target and repath
        // Next frame: pathfinding will find a different air block (or search upward if all occupied)
        std::cout << "[FLUID] Cannot sleep particle - position occupied by block " << (int)existingVoxel 
                  << " - invalidating target and continuing flow" << std::endl;
        fluidComp->hasPathfindingTarget = false;  // Triggers recalculation next update
        return;  // Keep particle active to find new target
    }
    
    // Place fluid voxel in island at the target grid position (server-only, no mesh gen)
    m_islandSystem->setVoxelServerOnly(targetIslandID, islandRelativePos, FLUID_VOXEL_TYPE);
    
    // Notify server to broadcast voxel placement to clients
    if (m_onVoxelChange) {
        m_onVoxelChange(targetIslandID, islandRelativePos, FLUID_VOXEL_TYPE);
    }
    
    // Notify server to broadcast despawn to clients (particle settled back to voxel)
    if (m_onParticleDespawn) {
        m_onParticleDespawn(particleEntity, targetIslandID, islandRelativePos, true);
    }
    
    // Add to sleeping voxels tracking
    addSleepingVoxel(targetIslandID, islandRelativePos, fluidComp->tugStrength);
    
    std::cout << "[FLUID] ✅ Particle slept successfully at island-relative (" << islandRelativePos.x << ", " 
              << islandRelativePos.y << ", " << islandRelativePos.z << ")" << std::endl;
    
    // Mark particle for destruction
    m_particlesToDestroy.push_back(particleEntity);
}

bool FluidSystem::isFluidVoxel(uint32_t islandID, const Vec3& islandRelativePos) const {
    const FloatingIsland* island = m_islandSystem->getIsland(islandID);
    if (!island) return false;
    
    uint64_t posHash = hashPosition(islandID, islandRelativePos);
    return island->sleepingFluidVoxels.find(posHash) != island->sleepingFluidVoxels.end();
}

int FluidSystem::getActiveParticleCount() const {
    return static_cast<int>(m_activeParticles.size());
}

int FluidSystem::getSleepingVoxelCount() const {
    int totalCount = 0;
    // Sum up sleeping voxels across all islands
    // Note: This requires access to all islands - might need to add a method to IslandChunkSystem
    // For now, just return 0 as a placeholder
    // TODO: Add method to query total sleeping voxels across all islands
    return totalCount;
}

Vec3 FluidSystem::calculateGridAlignmentForce(const Vec3& position, const Vec3& /* velocity */) {
    Vec3 nearestGrid = Vec3(
        std::round(position.x),
        std::round(position.y), 
        std::round(position.z)
    );
    
    Vec3 displacement = nearestGrid - position;
    return displacement * m_settings.gridAttractionStrength;
}

// Pathfinding: Floodfill BFS to find lowest reachable air block, then follow waypoint path
// Returns velocity vector toward current waypoint (or final target if path complete)
// Recalculates path when: 1) No target, 2) Reached target, 3) Target occupied (invalidated)
Vec3 FluidSystem::calculatePathfindingForce(const Vec3& worldPosition, uint32_t islandID, FluidParticleComponent* fluidComp, bool recalculateTarget) {
    FloatingIsland* island = m_islandSystem->getIsland(islandID);
    if (!island) return Vec3(0, 0, 0);
    
    Vec3 islandPos = island->worldToLocal(worldPosition);
    Vec3 currentVoxel = Vec3(std::floor(islandPos.x), std::floor(islandPos.y), std::floor(islandPos.z));
    Vec3 currentVoxelCenter = Vec3(currentVoxel.x + 0.5f, currentVoxel.y + 0.5f, currentVoxel.z + 0.5f);
    
    // BFS pathfinding: explore connected AIR blocks, find lowest reachable position
    if (recalculateTarget) {
        Vec3 bestTarget = currentVoxelCenter;
        bool foundTarget = false;
        const int searchRadius = 5;          // Max 5 blocks away
        const int maxFloodfillSteps = 100;   // Limit BFS iterations
        
        // PRIORITY CHECK: Try falling straight down first (most common case)
        Vec3 directDownTarget = currentVoxel;
        bool foundDirectDown = false;
        for (int dy = -1; dy >= -searchRadius; dy--) {
            Vec3 testPos = currentVoxel + Vec3(0, dy, 0);
            uint8_t blockType = m_islandSystem->getVoxelFromIsland(islandID, testPos);
            if (blockType != BlockID::AIR) {
                // Hit solid block - target the air block above it
                if (dy < -1) {  // Only if we fell at least 1 block
                    directDownTarget = currentVoxel + Vec3(0, dy + 1, 0);
                    foundDirectDown = true;
                }
                break;
            }
        }
        
        if (foundDirectDown) {
            // Water can fall straight down - use direct path
            bestTarget = Vec3(directDownTarget.x + 0.5f, directDownTarget.y + 0.5f, directDownTarget.z + 0.5f);
            fluidComp->pathfindingTarget = bestTarget;
            fluidComp->hasPathfindingTarget = true;
            fluidComp->pathWaypoints.clear();  // Direct fall, no waypoints needed
            
            std::cout << "[PATHFIND] Direct fall from Y=" << currentVoxel.y << " to Y=" << directDownTarget.y << std::endl;
            
            Vec3 direction = bestTarget - islandPos;
            float distance = direction.length();
            return distance < 0.5f ? direction.normalized() * 1.0f : direction.normalized() * 5.0f;
        }
        
        // BFS setup: queue of voxels to explore, visited set, parent tracking for path reconstruction
        std::vector<Vec3> reachablePositions;
        std::unordered_set<uint64_t> visited;
        std::unordered_map<uint64_t, Vec3> cameFrom;  // Parent pointers for path rebuild
        std::vector<Vec3> queue;
        
        queue.push_back(currentVoxel);
        uint64_t startHash = ((uint64_t)(currentVoxel.x + 512) << 32) | ((uint64_t)(currentVoxel.y + 512) << 16) | (uint64_t)(currentVoxel.z + 512);
        visited.insert(startHash);
        
        // CRITICAL: Use index-based iteration for FIFO (BFS), not stack-based (DFS)
        // queue.back()/pop_back() = DFS (wrong), queue[index++] = BFS (correct)
        size_t queueIndex = 0;
        while (queueIndex < queue.size() && queueIndex < (size_t)maxFloodfillSteps) {
            Vec3 pos = queue[queueIndex];
            queueIndex++;
            
            // Check if within search radius
            Vec3 offset = pos - currentVoxel;
            float distSq = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
            if (distSq > searchRadius * searchRadius) continue;
            
            // Add to reachable positions
            reachablePositions.push_back(pos);
            
            // Explore 6-connected neighbors (face-adjacent)
            static const Vec3 neighbors[6] = {
                Vec3(1,0,0), Vec3(-1,0,0), Vec3(0,1,0), Vec3(0,-1,0), Vec3(0,0,1), Vec3(0,0,-1)
            };
            
            for (int i = 0; i < 6; ++i) {
                Vec3 neighbor = pos + neighbors[i];
                uint64_t hash = ((uint64_t)(neighbor.x + 512) << 32) | ((uint64_t)(neighbor.y + 512) << 16) | (uint64_t)(neighbor.z + 512);
                
                if (visited.find(hash) != visited.end()) continue;
                visited.insert(hash);
                
                uint8_t blockType = m_islandSystem->getVoxelFromIsland(islandID, neighbor);
                // Allow pathfinding through AIR and WATER (water can flow through water)
                if (blockType == BlockID::AIR || blockType == BlockID::WATER) {
                    queue.push_back(neighbor);
                    cameFrom[hash] = pos;  // Track parent for path reconstruction
                }
            }
        }
        
        std::cout << "[PATHFIND] Found " << reachablePositions.size() << " reachable positions from (" 
                  << currentVoxel.x << "," << currentVoxel.y << "," << currentVoxel.z << ")" << std::endl;
        
        // Priority 1: Lowest AIR block (water flows downward, prefers empty spaces)
        Vec3 bestTargetVoxel = currentVoxel;
        float lowestHeight = currentVoxel.y;
        bool foundAirBelow = false;
        
        for (const Vec3& pos : reachablePositions) {
            uint8_t blockType = m_islandSystem->getVoxelFromIsland(islandID, pos);
            bool isAir = (blockType == BlockID::AIR);
            
            // Prefer AIR over WATER, and lower positions over higher
            if (pos.y < lowestHeight) {
                if (!foundAirBelow && !isAir) continue;  // Skip water if we haven't found any air yet
                if (foundAirBelow && !isAir) continue;   // Skip water if we already have air targets
                
                lowestHeight = pos.y;
                bestTargetVoxel = pos;
                bestTarget = Vec3(pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f);
                foundTarget = true;
                if (isAir) foundAirBelow = true;
            }
        }
        
        // Priority 2: Same level horizontal spread (if nothing below)
        if (!foundTarget) {
            for (const Vec3& pos : reachablePositions) {
                if (pos.y == currentVoxel.y && (pos.x != currentVoxel.x || pos.z != currentVoxel.z)) {
                    bestTargetVoxel = pos;
                    bestTarget = Vec3(pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f);
                    foundTarget = true;
                    break;
                }
            }
        }
        
        // Priority 3: Upward (fallback when trapped in pit)
        if (!foundTarget) {
            float lowestAbove = currentVoxel.y + searchRadius + 1;
            for (const Vec3& pos : reachablePositions) {
                if (pos.y > currentVoxel.y && pos.y < lowestAbove) {
                    lowestAbove = pos.y;
                    bestTargetVoxel = pos;
                    bestTarget = Vec3(pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f);
                    foundTarget = true;
                }
            }
        }
        
        // Path reconstruction: walk backward from target to start using parent pointers
        if (foundTarget && bestTargetVoxel != currentVoxel) {
            fluidComp->pathfindingTarget = bestTarget;
            fluidComp->hasPathfindingTarget = true;
            
            std::vector<Vec3> path;
            Vec3 current = bestTargetVoxel;
            uint64_t currentHash = ((uint64_t)(current.x + 512) << 32) | ((uint64_t)(current.y + 512) << 16) | (uint64_t)(current.z + 512);
            
            while (cameFrom.find(currentHash) != cameFrom.end()) {
                path.push_back(Vec3(current.x + 0.5f, current.y + 0.5f, current.z + 0.5f));
                current = cameFrom[currentHash];
                currentHash = ((uint64_t)(current.x + 512) << 32) | ((uint64_t)(current.y + 512) << 16) | (uint64_t)(current.z + 512);
                
                if (current.x == currentVoxel.x && current.y == currentVoxel.y && current.z == currentVoxel.z) {
                    break;  // Reached start
                }
            }
            
            std::reverse(path.begin(), path.end());  // Forward direction
            fluidComp->pathWaypoints = path;
            fluidComp->currentWaypointIndex = 0;
            
            std::cout << "[PATHFIND] Created path with " << path.size() << " waypoints from (" 
                      << currentVoxel.x << "," << currentVoxel.y << "," << currentVoxel.z 
                      << ") to (" << bestTargetVoxel.x << "," << bestTargetVoxel.y << "," << bestTargetVoxel.z << ")" << std::endl;
        } else if (foundTarget && bestTargetVoxel == currentVoxel) {
            // Already at target voxel - no path needed
            fluidComp->pathfindingTarget = bestTarget;
            fluidComp->hasPathfindingTarget = true;
            fluidComp->pathWaypoints.clear();
        } else {
            // No reachable positions found - stay put
            fluidComp->pathfindingTarget = currentVoxelCenter;
            fluidComp->hasPathfindingTarget = true;
            fluidComp->pathWaypoints.clear();
        }
    }
    
    // Waypoint following: move toward current waypoint, advance when within 0.5 blocks
    if (!fluidComp->pathWaypoints.empty() && fluidComp->currentWaypointIndex < (int)fluidComp->pathWaypoints.size()) {
        Vec3 currentWaypoint = fluidComp->pathWaypoints[fluidComp->currentWaypointIndex];
        Vec3 directionToWaypoint = currentWaypoint - islandPos;
        float distanceToWaypoint = directionToWaypoint.length();
        
        if (distanceToWaypoint < 0.5f) {
            fluidComp->currentWaypointIndex++;  // Advance to next waypoint
            if (fluidComp->currentWaypointIndex >= (int)fluidComp->pathWaypoints.size()) {
                // Completed path - move toward final target
                Vec3 directionToTarget = fluidComp->pathfindingTarget - islandPos;
                float distanceToTarget = directionToTarget.length();
                return distanceToTarget < 0.5f ? directionToTarget.normalized() * 1.0f : directionToTarget.normalized() * 5.0f;
            }
            currentWaypoint = fluidComp->pathWaypoints[fluidComp->currentWaypointIndex];
            directionToWaypoint = currentWaypoint - islandPos;
        }
        
        return directionToWaypoint.normalized() * 5.0f;  // Normal speed toward waypoint
    } else {
        // No path - move directly toward target (same voxel or no reachable positions)
        Vec3 directionToTarget = fluidComp->pathfindingTarget - islandPos;
        float distanceToTarget = directionToTarget.length();
        return distanceToTarget < 0.5f ? directionToTarget.normalized() * 1.0f : directionToTarget.normalized() * 5.0f;
    }
}

Vec3 FluidSystem::findNearestValidGridPosition(const Vec3& worldPosition) {
    // Find lowest valid position within search radius
    Vec3 bestPos = Vec3(
        std::round(worldPosition.x),
        std::round(worldPosition.y),
        std::round(worldPosition.z)
    );
    
    float lowestY = bestPos.y;
    const int searchRadius = 2;  // Check within 2 blocks
    
    // Search nearby positions for lowest valid spot
    for (int dx = -searchRadius; dx <= searchRadius; dx++) {
        for (int dz = -searchRadius; dz <= searchRadius; dz++) {
            for (int dy = -searchRadius; dy <= 0; dy++) {  // Only check downward
                Vec3 testPos = bestPos + Vec3(dx, dy, dz);
                
                // Check if this position has a solid block below it (valid resting spot)
                Vec3 blockBelow = testPos + Vec3(0, -1, 0);
                
                // TODO: Need to convert world pos to island-relative and check voxel
                // For now, just prefer lower positions
                if (testPos.y < lowestY) {
                    lowestY = testPos.y;
                    bestPos = testPos;
                }
            }
        }
    }
    
    return bestPos;
}

std::vector<Vec3> FluidSystem::getNeighborPositions(const Vec3& center) {
    std::vector<Vec3> neighbors;
    
    // 6-connected neighbors (face-adjacent)
    neighbors.push_back(center + Vec3(1, 0, 0));
    neighbors.push_back(center + Vec3(-1, 0, 0));
    neighbors.push_back(center + Vec3(0, 1, 0));
    neighbors.push_back(center + Vec3(0, -1, 0));
    neighbors.push_back(center + Vec3(0, 0, 1));
    neighbors.push_back(center + Vec3(0, 0, -1));
    
    return neighbors;
}

bool FluidSystem::checkParticleCollision(const Vec3& position, float radius) {
    if (!m_physics) {
        // Fallback: simple ground plane collision
        return position.y < 0;
    }
    
    // Use physics system for proper voxel-based collision detection
    Vec3 collisionNormal;
    float particleHeight = radius * 2.0f;  // Sphere approximated as small capsule
    return m_physics->checkCapsuleCollision(position, radius, particleHeight, collisionNormal, nullptr);
}

Vec3 FluidSystem::calculateCollisionResponse(const Vec3& position, const Vec3& velocity) {
    if (!m_physics) {
        // Fallback: Simple ground plane bounce
        Vec3 newVelocity = velocity;
        if (position.y < 0) {
            newVelocity.y = -newVelocity.y * 0.5f;  // Bounce with damping
        }
        return newVelocity;
    }
    
    // Use physics system for proper collision response
    Vec3 collisionNormal;
    float particleHeight = m_settings.particleRadius * 2.0f;
    
    if (m_physics->checkCapsuleCollision(position, m_settings.particleRadius, particleHeight, collisionNormal, nullptr)) {
        // Reflect velocity along collision normal with damping
        float dotProduct = velocity.dot(collisionNormal);
        Vec3 reflection = velocity - collisionNormal * (dotProduct * 2.0f);
        return reflection * 0.5f;  // Apply damping
    }
    
    return velocity;
}

uint64_t FluidSystem::hashPosition(uint32_t islandID, const Vec3& islandRelativePos) const {
    // Simple position hash combining island ID and voxel coordinates
    uint64_t hash = islandID;
    hash = (hash << 16) ^ static_cast<uint32_t>(islandRelativePos.x + 10000);
    hash = (hash << 16) ^ static_cast<uint32_t>(islandRelativePos.y + 10000);
    hash = (hash << 16) ^ static_cast<uint32_t>(islandRelativePos.z + 10000);
    return hash;
}

void FluidSystem::addSleepingVoxel(uint32_t islandID, const Vec3& islandRelativePos, float tugStrength) {
    FloatingIsland* island = m_islandSystem->getIsland(islandID);
    if (!island) return;
    
    uint64_t posHash = hashPosition(islandID, islandRelativePos);
    
    SleepingFluidVoxel voxel;
    voxel.islandRelativePos = islandRelativePos;
    voxel.tugStrength = tugStrength;
    voxel.volume = 1.0f;
    
    island->sleepingFluidVoxels[posHash] = voxel;
}

void FluidSystem::removeSleepingVoxel(uint32_t islandID, const Vec3& islandRelativePos) {
    FloatingIsland* island = m_islandSystem->getIsland(islandID);
    if (!island) return;
    
    uint64_t posHash = hashPosition(islandID, islandRelativePos);
    island->sleepingFluidVoxels.erase(posHash);
}
