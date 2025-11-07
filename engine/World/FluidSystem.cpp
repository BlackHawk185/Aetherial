// FluidSystem.cpp - Implementation of sleeping particle fluid simulation
#include "FluidSystem.h"
#include "VoxelRaycaster.h"
#include "../Profiling/Profiler.h"
#include "../Physics/PhysicsSystem.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>

// Global fluid system instance
FluidSystem g_fluidSystem;

FluidSystem::FluidSystem() {
    // Default constructor
}

FluidSystem::~FluidSystem() {
    // Cleanup
}

void FluidSystem::initialize(IslandChunkSystem* islandSystem, ECSWorld* ecsWorld, PhysicsSystem* physics) {
    m_islandSystem = islandSystem;
    m_ecsWorld = ecsWorld;
    m_physics = physics;
    
    std::cout << "✨ Fluid system initialized with sleeping particle architecture" << std::endl;
}

void FluidSystem::update(float deltaTime) {
    PROFILE_SCOPE("FluidSystem::update");
    
    // Reset frame counters
    m_particlesWokenThisFrame = 0;
    
    // Update active particles
    updateActiveParticles(deltaTime);
    
    // Process state transitions (sleep/wake/destroy)
    processParticleTransitions();
    
    // Clean up destroyed particles
    cleanupDestroyedParticles();
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

void FluidSystem::updateParticlePhysics(EntityID /* particle */, FluidParticleComponent* fluidComp, 
                                       TransformComponent* transform, float deltaTime) {
    if (!m_physics) return;
    
    // Apply gravity
    fluidComp->velocity.y += m_settings.gravity * deltaTime;
    
    // Apply viscosity (simple damping)
    fluidComp->velocity = fluidComp->velocity * (1.0f - m_settings.viscosity * deltaTime);
    
    // Detect ground state using the same system as player
    const float raycastMargin = 0.1f;
    GroundInfo groundInfo = m_physics->detectGroundCapsule(
        transform->position, 
        m_settings.particleRadius,
        m_settings.particleRadius * 2.0f, // height = diameter for sphere
        raycastMargin
    );
    
    // If grounded and slow, apply pathfinding to flow downhill
    if (groundInfo.isGrounded && fluidComp->velocity.length() < 2.0f) {
        Vec3 pathfindingForce = calculatePathfindingForce(transform->position, fluidComp->sourceIslandID, fluidComp);
        fluidComp->velocity = fluidComp->velocity + pathfindingForce * deltaTime * 5.0f; // Pathfinding acceleration
    }
    
    // Use unified physics movement resolver with aggressive anti-stuck
    // This handles collision, step-up, and unstuck automatically
    // Fluid particles can only climb 10% of their height (very limited climbing)
    transform->position = m_physics->resolveCapsuleMovement(
        transform->position,
        fluidComp->velocity,
        deltaTime,
        m_settings.particleRadius,
        m_settings.particleRadius * 2.0f,  // height
        0.1f  // stepHeightRatio - water can barely climb (only 10% of height = ~0.08 blocks)
    );
}

void FluidSystem::updateParticleSettling(EntityID particle, FluidParticleComponent* fluidComp, 
                                        TransformComponent* transform, float deltaTime) {
    // Update alive timer for tracking purposes
    fluidComp->aliveTimer += deltaTime;
    
    // Check if particle is grounded
    if (!m_physics) return;
    
    GroundInfo groundInfo = m_physics->detectGroundCapsule(
        transform->position, 
        m_settings.particleRadius,
        m_settings.particleRadius * 2.0f,
        0.1f
    );
    
    if (!groundInfo.isGrounded) {
        return;
    }
    
    // Check if particle has reached its pathfinding target
    FloatingIsland* island = m_islandSystem->getIsland(fluidComp->sourceIslandID);
    if (!island) return;
    
    Vec3 islandPos = island->worldToLocal(transform->position);
    Vec3 currentVoxel = Vec3(std::floor(islandPos.x), std::floor(islandPos.y), std::floor(islandPos.z));
    Vec3 currentVoxelCenter = Vec3(currentVoxel.x + 0.5f, currentVoxel.y + 0.5f, currentVoxel.z + 0.5f);
    
    bool atTarget = false;
    if (fluidComp->hasPathfindingTarget) {
        Vec3 toTarget = fluidComp->pathfindingTarget - islandPos;
        toTarget.y = 0; // Only check horizontal distance
        atTarget = toTarget.length() < 0.15f; // Within 0.15 blocks of target
    }
    
    // If at target, check if there's a better place to go
    if (atTarget) {
        // Search for lower neighbors one more time
        float lowestHeight = currentVoxel.y;
        bool foundLower = false;
        
        Vec3 directions[] = {
            Vec3(1, 0, 0), Vec3(-1, 0, 0),
            Vec3(0, 0, 1), Vec3(0, 0, -1)
        };
        
        for (const Vec3& dir : directions) {
            Vec3 neighborVoxel = currentVoxel + dir;
            uint8_t voxelType = m_islandSystem->getVoxelFromIsland(fluidComp->sourceIslandID, neighborVoxel);
            
            if (voxelType == BlockID::AIR) {
                // Find ground level of this neighbor
                Vec3 checkPos = neighborVoxel;
                float groundLevel = neighborVoxel.y;
                
                for (int i = 0; i < 10; ++i) {
                    checkPos.y -= 1.0f;
                    uint8_t belowType = m_islandSystem->getVoxelFromIsland(fluidComp->sourceIslandID, checkPos);
                    if (belowType != BlockID::AIR) {
                        groundLevel = checkPos.y + 1.0f;
                        break;
                    }
                }
                
                // Check if this neighbor is lower
                if (groundLevel < lowestHeight - 0.05f) {
                    foundLower = true;
                    break;
                }
            }
        }
        
        // If no lower neighbor exists, settle immediately
        if (!foundLower) {
            // Round to nearest grid position
            fluidComp->targetGridPos = Vec3(
                std::round(islandPos.x),
                std::round(islandPos.y),
                std::round(islandPos.z)
            );
            
            std::cout << "[FLUID] Particle at target with no lower path - settling at (" 
                      << fluidComp->targetGridPos.x << ", " << fluidComp->targetGridPos.y << ", " 
                      << fluidComp->targetGridPos.z << ")" << std::endl;
            
            m_particlesToSleep.push_back(particle);
        }
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
        
        // If particle has moved more than tugDistance away, activate the water voxel
        if (distance > m_settings.tugDistance) {
            // Check if this voxel is still water
            uint8_t voxelType = m_islandSystem->getVoxelFromIsland(fluidComp->sourceIslandID, waterVoxelPos);
            if (voxelType == BlockID::WATER) {
                // Wake this water voxel
                wakeFluidVoxel(fluidComp->sourceIslandID, waterVoxelPos);
                m_particlesWokenThisFrame++;
                
                // Remove from watch list (we've activated it)
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
        m_ecsWorld->destroyEntity(particle);
    }
    m_particlesToDestroy.clear();
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
    
    std::cout << "[FLUID] Calling setVoxelInIsland to remove water..." << std::endl;
    try {
        m_islandSystem->setVoxelInIsland(islandID, islandRelativePos, 0);  // 0 = empty
        std::cout << "[FLUID] Voxel removed successfully" << std::endl;
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
    
    return particle;
}

void FluidSystem::registerNearbyWaterVoxels(FluidParticleComponent* fluidComp, const Vec3& islandRelativePos) {
    FloatingIsland* island = m_islandSystem->getIsland(fluidComp->sourceIslandID);
    if (!island) return;
    
    // Use the island-relative position directly (already in the right space)
    Vec3 islandPos = islandRelativePos;
    
    // Check neighbors within tugRadius for water voxels
    int searchRadius = static_cast<int>(std::ceil(m_settings.tugRadius));
    
    std::cout << "[FLUID] Scanning for water neighbors around (" << islandPos.x << ", " << islandPos.y << ", " << islandPos.z 
              << ") radius=" << searchRadius << std::endl;
    
    for (int dx = -searchRadius; dx <= searchRadius; dx++) {
        for (int dy = -searchRadius; dy <= searchRadius; dy++) {
            for (int dz = -searchRadius; dz <= searchRadius; dz++) {
                Vec3 neighborPos = islandPos + Vec3(dx, dy, dz);
                
                // Skip the particle's own position
                if (dx == 0 && dy == 0 && dz == 0) continue;
                
                // Check if this position has a water voxel
                uint8_t voxelType = m_islandSystem->getVoxelFromIsland(fluidComp->sourceIslandID, neighborPos);
                if (voxelType == BlockID::WATER) {
                    // Add to watch list
                    fluidComp->watchedWaterVoxels.push_back(neighborPos);
                    std::cout << "[FLUID]   - Found water at offset (" << dx << ", " << dy << ", " << dz 
                              << ") absolute (" << neighborPos.x << ", " << neighborPos.y << ", " << neighborPos.z << ")" << std::endl;
                }
            }
        }
    }
    
    std::cout << "[FLUID] Registered " << fluidComp->watchedWaterVoxels.size() << " nearby water voxels for particle" << std::endl;
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
        // Target position is occupied, destroy particle instead of sleeping
        std::cout << "[FLUID] Cannot sleep particle - position occupied by block " << (int)existingVoxel << std::endl;
        m_particlesToDestroy.push_back(particleEntity);
        return;
    }
    
    // Place fluid voxel in island at the target grid position
    m_islandSystem->setVoxelInIsland(targetIslandID, islandRelativePos, FLUID_VOXEL_TYPE);
    
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

Vec3 FluidSystem::calculatePathfindingForce(const Vec3& worldPosition, uint32_t islandID, FluidParticleComponent* fluidComp) {
    // Find the island this particle is on and pathfind to voxel centers
    FloatingIsland* island = m_islandSystem->getIsland(islandID);
    if (!island) return Vec3(0, 0, 0);
    
    // Convert world position to island-relative space
    Vec3 islandPos = island->worldToLocal(worldPosition);
    
    // Find current voxel grid position
    Vec3 currentVoxel = Vec3(std::floor(islandPos.x), std::floor(islandPos.y), std::floor(islandPos.z));
    
    // Calculate center of current voxel (island-relative)
    Vec3 currentVoxelCenter = Vec3(currentVoxel.x + 0.5f, currentVoxel.y + 0.5f, currentVoxel.z + 0.5f);
    
    // STEP 1: Find best target voxel by checking horizontal neighbors for LOWER terrain
    Vec3 bestTarget = currentVoxelCenter; // Default to current voxel center
    float lowestHeight = currentVoxel.y;
    bool foundLower = false;
    
    Vec3 directions[] = {
        Vec3(1, 0, 0),   // +X
        Vec3(-1, 0, 0),  // -X
        Vec3(0, 0, 1),   // +Z
        Vec3(0, 0, -1)   // -Z
    };
    
    for (const Vec3& dir : directions) {
        Vec3 neighborVoxel = currentVoxel + dir;
        
        // Check if neighbor is empty (air)
        uint8_t voxelType = m_islandSystem->getVoxelFromIsland(islandID, neighborVoxel);
        if (voxelType == BlockID::AIR) {
            // Check what's below the empty neighbor to find ground level
            Vec3 checkPos = neighborVoxel;
            float groundLevel = neighborVoxel.y;
            
            // Scan down to find ground
            for (int i = 0; i < 10; ++i) {
                checkPos.y -= 1.0f;
                uint8_t belowType = m_islandSystem->getVoxelFromIsland(islandID, checkPos);
                if (belowType != BlockID::AIR) {
                    groundLevel = checkPos.y + 1.0f; // Ground is one block up from solid
                    break;
                }
            }
            
            // ONLY accept neighbor if it has LOWER ground (not equal!)
            if (groundLevel < lowestHeight - 0.05f) {
                lowestHeight = groundLevel;
                bestTarget = Vec3(neighborVoxel.x + 0.5f, neighborVoxel.y + 0.5f, neighborVoxel.z + 0.5f);
                foundLower = true;
            }
        }
    }
    
    // STEP 2: Decide whether to commit to new target or keep existing one
    if (foundLower) {
        // Found a lower neighbor - only override existing target if it's better
        if (!fluidComp->hasPathfindingTarget || lowestHeight < fluidComp->pathfindingTarget.y - 0.05f) {
            fluidComp->pathfindingTarget = bestTarget;
            fluidComp->hasPathfindingTarget = true;
        }
    } else {
        // No lower neighbor found - commit to current voxel center if we don't have a target
        if (!fluidComp->hasPathfindingTarget) {
            fluidComp->pathfindingTarget = currentVoxelCenter;
            fluidComp->hasPathfindingTarget = true;
        }
    }
    
    // STEP 3: Calculate force toward committed target
    Vec3 directionToTarget = fluidComp->pathfindingTarget - islandPos;
    directionToTarget.y = 0; // Only horizontal movement
    
    float distanceToTarget = directionToTarget.length();
    
    // If we're very close to target (within 0.1 blocks), apply no force (we're settled)
    if (distanceToTarget < 0.1f) {
        return Vec3(0, 0, 0);
    }
    
    // Return force toward committed target
    Vec3 force = directionToTarget.normalized() * 3.0f;
    return force;
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