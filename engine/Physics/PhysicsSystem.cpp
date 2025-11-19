// PhysicsSystem.cpp - Basic collision detection system
#include "PhysicsSystem.h"

#include <iostream>
#include <cmath>

#include "../World/IslandChunkSystem.h"
#include "../World/VoxelChunk.h"
#include "../World/BlockType.h"  // For BlockTypeRegistry and BlockTypeInfo
#include "../World/VoxelRaycaster.h"  // For DDA-based ground detection
#include "../Profiling/Profiler.h"

// Global g_physics removed - GameClient and GameServer now have separate instances

PhysicsSystem::PhysicsSystem() {}

PhysicsSystem::~PhysicsSystem()
{
    shutdown();
}

bool PhysicsSystem::initialize()
{
    return true;
}

void PhysicsSystem::update(float deltaTime)
{
    PROFILE_FUNCTION();
    // Apply physics to all entities with Transform and Velocity components
    updateEntities(deltaTime);
}

void PhysicsSystem::shutdown()
{
}

void PhysicsSystem::updateEntities(float deltaTime)
{
    // Physics updates now handled by PlayerController using capsule collision
    // This function intentionally left minimal - entity physics is application-specific
}

// Debug and testing methods
void PhysicsSystem::debugCollisionInfo(const Vec3& playerPos, float playerRadius)
{
    if (!m_islandSystem)
    {
        std::cout << "PhysicsSystem: No island system connected" << std::endl;
        return;
    }

    std::cout << "=== Collision Debug Info ===" << std::endl;
    std::cout << "Player pos: (" << playerPos.x << ", " << playerPos.y << ", " << playerPos.z << ")" << std::endl;
    std::cout << "Player radius: " << playerRadius << std::endl;

    const auto& islands = m_islandSystem->getIslands();
    std::cout << "Total islands: " << islands.size() << std::endl;

    int totalFaces = 0;
    for (const auto& islandPair : islands)
    {
        const FloatingIsland* island = &islandPair.second;
        if (!island) continue;

        std::cout << "Island " << islandPair.first << " at (" << island->physicsCenter.x << ", " << island->physicsCenter.y << ", " << island->physicsCenter.z << ")" << std::endl;
        std::cout << "  Chunks: " << island->chunks.size() << std::endl;

        for (const auto& chunkPair : island->chunks)
        {
            const VoxelChunk* chunk = chunkPair.second.get();
            if (!chunk) continue;

            // Voxel-based collision - no face mesh needed
            std::cout << "    Chunk at (" << chunkPair.first.x << ", " << chunkPair.first.y << ", " << chunkPair.first.z << "): Using voxel-based collision" << std::endl;
        }
    }

    std::cout << "Using voxel-based collision (no face meshes)" << std::endl;
    std::cout << "==========================" << std::endl;
}

int PhysicsSystem::getTotalCollisionFaces() const
{
    // Voxel-based collision doesn't use face meshes
    return 0;
}

// ============================================================================
// CAPSULE COLLISION SYSTEM - VOXEL-BASED OPTIMIZATION
// ============================================================================
// Capsule is a cylinder with hemispherical caps on top and bottom
// Perfect for humanoid character collision (narrow width, proper height)
//
// OPTIMIZATION: Instead of iterating 10K-100K collision faces (slow at 256^3 chunks),
// we query voxels within the capsule AABB, then only test faces for solid voxels.
// This reduces checks from O(all_faces) to O(voxels_in_capsule_bounds).

bool PhysicsSystem::checkChunkCapsuleCollision(const VoxelChunk* chunk, const Vec3& capsuleCenter,
                                               const Vec3& /*chunkWorldPos*/, Vec3& outNormal,
                                               float radius, float height)
{
    // Capsule breakdown:
    // - Total height: height
    // - Cylinder height: height - 2*radius (middle section)
    // - Top sphere center: capsuleCenter + (0, cylinderHeight/2, 0)
    // - Bottom sphere center: capsuleCenter - (0, cylinderHeight/2, 0)
    
    float cylinderHalfHeight = (height - 2.0f * radius) * 0.5f;
    Vec3 topSphereCenter = capsuleCenter + Vec3(0, cylinderHalfHeight, 0);
    Vec3 bottomSphereCenter = capsuleCenter - Vec3(0, cylinderHalfHeight, 0);
    
    // Calculate AABB of capsule in chunk-local voxel coordinates
    float capsuleHalfHeight = height * 0.5f;
    int minX = static_cast<int>(std::floor(capsuleCenter.x - radius));
    int maxX = static_cast<int>(std::ceil(capsuleCenter.x + radius));
    int minY = static_cast<int>(std::floor(capsuleCenter.y - capsuleHalfHeight));
    int maxY = static_cast<int>(std::ceil(capsuleCenter.y + capsuleHalfHeight));
    int minZ = static_cast<int>(std::floor(capsuleCenter.z - radius));
    int maxZ = static_cast<int>(std::ceil(capsuleCenter.z + radius));
    
    // Clamp to chunk bounds
    minX = std::max(0, minX);
    maxX = std::min(VoxelChunk::SIZE - 1, maxX);
    minY = std::max(0, minY);
    maxY = std::min(VoxelChunk::SIZE - 1, maxY);
    minZ = std::max(0, minZ);
    maxZ = std::min(VoxelChunk::SIZE - 1, maxZ);
    
    // VOXEL-BASED CULLING: Only check voxels within capsule AABB
    // This replaces iterating 100K faces with checking ~10-50 voxels
    for (int x = minX; x <= maxX; ++x)
    {
        for (int y = minY; y <= maxY; ++y)
        {
            for (int z = minZ; z <= maxZ; ++z)
            {
                // Skip air blocks
                uint8_t blockType = chunk->getVoxel(x, y, z);
                if (blockType == 0)
                    continue;
                
                // Skip OBJ/model blocks (they don't have collision - decorative only)
                auto& registry = BlockTypeRegistry::getInstance();
                const BlockTypeInfo* blockInfo = registry.getBlockType(blockType);
                if (blockInfo && blockInfo->renderType == BlockRenderType::OBJ) {
                    continue;  // OBJ blocks are decorative - no collision
                }
                
                // Found solid voxel - do precise capsule-to-AABB collision
                Vec3 voxelMin(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                Vec3 voxelMax = voxelMin + Vec3(1.0f, 1.0f, 1.0f);
                
                // Find closest point on voxel AABB to capsule axis
                Vec3 closestPointOnAxis;
                
                // Determine which part of capsule to test
                float voxelCenterY = voxelMin.y + 0.5f;
                float yOffset = voxelCenterY - capsuleCenter.y;
                
                if (yOffset > cylinderHalfHeight)
                {
                    closestPointOnAxis = topSphereCenter;
                }
                else if (yOffset < -cylinderHalfHeight)
                {
                    closestPointOnAxis = bottomSphereCenter;
                }
                else
                {
                    closestPointOnAxis = capsuleCenter + Vec3(0, yOffset, 0);
                }
                
                // Find closest point on AABB to capsule axis point
                Vec3 closestPointOnAABB(
                    std::max(voxelMin.x, std::min(closestPointOnAxis.x, voxelMax.x)),
                    std::max(voxelMin.y, std::min(closestPointOnAxis.y, voxelMax.y)),
                    std::max(voxelMin.z, std::min(closestPointOnAxis.z, voxelMax.z))
                );
                
                // Check if closest point is within capsule radius
                Vec3 delta = closestPointOnAABB - closestPointOnAxis;
                float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
                
                if (distSq <= radius * radius)
                {
                    // Collision detected - calculate penetration normal
                    // Use closest point method for accurate push-out direction
                    Vec3 penetrationVec = closestPointOnAxis - closestPointOnAABB;
                    float penetrationDist = std::sqrt(penetrationVec.x * penetrationVec.x + 
                                                     penetrationVec.y * penetrationVec.y + 
                                                     penetrationVec.z * penetrationVec.z);
                    
                    if (penetrationDist > 0.0001f)
                    {
                        // Normal points from collision point toward capsule (push-out direction)
                        outNormal = Vec3(penetrationVec.x / penetrationDist, 
                                        penetrationVec.y / penetrationDist, 
                                        penetrationVec.z / penetrationDist);
                    }
                    else
                    {
                        // Capsule center exactly on AABB surface - find nearest face
                        // This handles the "stuck inside block" case
                        Vec3 voxelCenter = voxelMin + Vec3(0.5f, 0.5f, 0.5f);
                        Vec3 delta = closestPointOnAxis - voxelCenter;
                        
                        // Find the axis with largest absolute displacement
                        float absX = std::abs(delta.x);
                        float absY = std::abs(delta.y);
                        float absZ = std::abs(delta.z);
                        
                        if (absX > absY && absX > absZ)
                        {
                            // Push out along X
                            outNormal = Vec3(delta.x > 0 ? 1.0f : -1.0f, 0, 0);
                        }
                        else if (absY > absZ)
                        {
                            // Push out along Y
                            outNormal = Vec3(0, delta.y > 0 ? 1.0f : -1.0f, 0);
                        }
                        else
                        {
                            // Push out along Z
                            outNormal = Vec3(0, 0, delta.z > 0 ? 1.0f : -1.0f);
                        }
                    }
                    
                    return true;
                }
            }
        }
    }
    
    return false;
}

bool PhysicsSystem::checkCapsuleCollision(const Vec3& capsuleCenter, float radius, float height,
                                         Vec3& outNormal, const FloatingIsland** outIsland)
{
    PROFILE_FUNCTION();
    if (!m_islandSystem)
        return false;
    
    const auto& islands = m_islandSystem->getIslands();
    
    // SPATIAL CULLING: Only check islands within reasonable distance
    // Most islands are far away and can't possibly collide with player
    const float maxIslandCheckDistance = 512.0f;  // Only check islands within 512 units
    const float maxDistSq = maxIslandCheckDistance * maxIslandCheckDistance;
    
    for (const auto& islandPair : islands)
    {
        const FloatingIsland* island = &islandPair.second;
        if (!island)
            continue;
        
        // OPTIMIZATION: Distance cull islands before expensive collision checks
        Vec3 delta = island->physicsCenter - capsuleCenter;
        float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        if (distSq > maxDistSq)
            continue;  // Island too far away to collide
        
        // Transform world-space capsule to island-local space (accounts for rotation!)
        Vec3 localPos = island->worldToLocal(capsuleCenter);
        
        // Calculate chunk bounds - capsule can span multiple chunks vertically
        float checkRadius = radius + VoxelChunk::SIZE;
        float checkHeight = height * 0.5f + VoxelChunk::SIZE;
        
        int minChunkX = static_cast<int>(std::floor((localPos.x - checkRadius) / VoxelChunk::SIZE));
        int maxChunkX = static_cast<int>(std::ceil((localPos.x + checkRadius) / VoxelChunk::SIZE));
        int minChunkY = static_cast<int>(std::floor((localPos.y - checkHeight) / VoxelChunk::SIZE));
        int maxChunkY = static_cast<int>(std::ceil((localPos.y + checkHeight) / VoxelChunk::SIZE));
        int minChunkZ = static_cast<int>(std::floor((localPos.z - checkRadius) / VoxelChunk::SIZE));
        int maxChunkZ = static_cast<int>(std::ceil((localPos.z + checkRadius) / VoxelChunk::SIZE));
        
        for (int chunkX = minChunkX; chunkX <= maxChunkX; ++chunkX)
        {
            for (int chunkY = minChunkY; chunkY <= maxChunkY; ++chunkY)
            {
                for (int chunkZ = minChunkZ; chunkZ <= maxChunkZ; ++chunkZ)
                {
                    Vec3 chunkCoord(chunkX, chunkY, chunkZ);
                    auto chunkIt = island->chunks.find(chunkCoord);
                    if (chunkIt == island->chunks.end() || !chunkIt->second)
                        continue;
                    
                    // Calculate chunk position in island-local space
                    Vec3 chunkLocalOffset = FloatingIsland::chunkCoordToWorldPos(chunkCoord);
                    Vec3 capsuleInChunkLocal = localPos - chunkLocalOffset;
                    
                    Vec3 collisionNormalLocal;
                    // NOTE: We're passing world positions for backward compatibility
                    // but collision detection now happens in island-local space
                    Vec3 chunkWorldPos = island->physicsCenter + chunkLocalOffset;
                    if (checkChunkCapsuleCollision(chunkIt->second.get(), capsuleInChunkLocal, chunkWorldPos,
                                                   collisionNormalLocal, radius, height))
                    {
                        // Transform collision normal from island-local to world space
                        outNormal = island->localDirToWorld(collisionNormalLocal);
                        if (outIsland)
                            *outIsland = island;
                        return true;
                    }
                }
            }
        }
    }
    
    return false;
}

GroundInfo PhysicsSystem::detectGroundCapsule(const Vec3& capsuleCenter, float radius, float height, float rayMargin)
{
    PROFILE_FUNCTION();
    GroundInfo info;
    info.isGrounded = false;
    
    if (!m_islandSystem)
        return info;
    
    // Raycast from bottom of capsule downward using DDA
    float cylinderHalfHeight = (height - 2.0f * radius) * 0.5f;
    float bottomY = capsuleCenter.y - cylinderHalfHeight - radius;
    
    // Start ray from well above feet to ensure we detect ground reliably
    Vec3 rayOrigin = Vec3(capsuleCenter.x, bottomY + 0.5f, capsuleCenter.z);
    Vec3 rayDirection = Vec3(0, -1, 0);
    
    // More generous detection range
    float generousMargin = rayMargin + 1.0f;
    
    // Use VoxelRaycaster DDA for accurate ground detection (same as block breaking)
    RayHit hit = VoxelRaycaster::raycast(rayOrigin, rayDirection, generousMargin, m_islandSystem);
    
    if (hit.hit && hit.distance <= generousMargin)
    {
        // Get the island we hit
        FloatingIsland* island = m_islandSystem->getIsland(hit.islandID);
        if (island)
        {
            info.isGrounded = true;
            info.standingOnIslandID = hit.islandID;
            info.groundNormal = hit.normal;
            info.groundVelocity = island->velocity;
            info.distanceToGround = hit.distance;
            
            // Calculate world-space contact point
            // Hit returns island-local block position, convert to world
            Vec3 hitPointLocal = hit.localBlockPos + Vec3(0.5f, 0.5f, 0.5f); // Center of block
            info.groundContactPoint = island->localToWorld(hitPointLocal);
        }
    }
    
    return info;
}

Vec3 PhysicsSystem::resolveCapsuleMovement(const Vec3& currentPos, Vec3& velocity, float deltaTime,
                                            float radius, float height, float stepHeightRatio)
{
    PROFILE_FUNCTION();
    
    if (!m_islandSystem)
    {
        // No collision system - just apply velocity directly
        return currentPos + velocity * deltaTime;
    }
    
    // Calculate max step height based on entity height
    // Taller entities can climb bigger obstacles
    // Small entities (like water droplets) can barely climb anything
    float maxStepHeight = height * stepHeightRatio;
    
    // Calculate intended movement
    Vec3 intendedMovement = velocity * deltaTime;
    Vec3 intendedPosition = currentPos + intendedMovement;
    
    Vec3 collisionNormal;
    Vec3 finalPosition = currentPos;
    
    // Check if we're currently stuck (before any movement)
    bool wasStuck = checkCapsuleCollision(currentPos, radius, height, collisionNormal, nullptr);
    
    // Calculate horizontal movement magnitude
    float horizontalMovement = std::sqrt(intendedMovement.x * intendedMovement.x + 
                                         intendedMovement.z * intendedMovement.z);
    
    // If stuck AND trying to move horizontally, aggressively unstuck first
    if (wasStuck && horizontalMovement > 0.001f)
    {
        // Use smaller increments (0.2 * height) for gentle unstuck
        float unstuckIncrement = height * 0.2f;
        for (float unstuckHeight = unstuckIncrement; unstuckHeight <= maxStepHeight * 2.0f; unstuckHeight += unstuckIncrement)
        {
            Vec3 unstuckPos = currentPos + Vec3(0, unstuckHeight, 0);
            if (!checkCapsuleCollision(unstuckPos, radius, height, collisionNormal, nullptr))
            {
                finalPosition = unstuckPos;
                break;
            }
        }
    }
    
    // Check if intended position is valid
    if (!checkCapsuleCollision(intendedPosition, radius, height, collisionNormal, nullptr))
    {
        // No collision - move freely
        return intendedPosition;
    }
    
    // Collision detected - use axis-separated movement with step-up
    
    // ===== PHASE 1: Try vertical movement first =====
    Vec3 testPos = finalPosition + Vec3(0, intendedMovement.y, 0);
    if (!checkCapsuleCollision(testPos, radius, height, collisionNormal, nullptr))
    {
        finalPosition = testPos;
    }
    else
    {
        velocity.y = 0; // Stop vertical movement
    }
    
    // ===== PHASE 2: Try horizontal X with step-up =====
    testPos = finalPosition + Vec3(intendedMovement.x, 0, 0);
    if (!checkCapsuleCollision(testPos, radius, height, collisionNormal, nullptr))
    {
        finalPosition = testPos;
    }
    else
    {
        // Blocked horizontally - try step-up based on entity height
        bool stepped = false;
        float stepIncrement = maxStepHeight * 0.25f; // Use 25% of max step as increment
        for (float stepHeight = stepIncrement; stepHeight <= maxStepHeight; stepHeight += stepIncrement)
        {
            Vec3 stepUpPos = finalPosition + Vec3(intendedMovement.x, stepHeight, 0);
            if (!checkCapsuleCollision(stepUpPos, radius, height, collisionNormal, nullptr))
            {
                // Success - apply horizontal movement and step up
                finalPosition.x += intendedMovement.x;
                finalPosition.y += stepHeight;
                stepped = true;
                break;
            }
        }
        
        if (!stepped)
        {
            velocity.x = 0; // Stop horizontal movement
        }
    }
    
    // ===== PHASE 3: Try horizontal Z with step-up =====
    testPos = finalPosition + Vec3(0, 0, intendedMovement.z);
    if (!checkCapsuleCollision(testPos, radius, height, collisionNormal, nullptr))
    {
        finalPosition = testPos;
    }
    else
    {
        // Blocked horizontally - try step-up based on entity height
        bool stepped = false;
        float stepIncrement = maxStepHeight * 0.25f; // Use 25% of max step as increment
        for (float stepHeight = stepIncrement; stepHeight <= maxStepHeight; stepHeight += stepIncrement)
        {
            Vec3 stepUpPos = finalPosition + Vec3(0, stepHeight, intendedMovement.z);
            if (!checkCapsuleCollision(stepUpPos, radius, height, collisionNormal, nullptr))
            {
                // Success - apply horizontal movement and step up
                finalPosition.z += intendedMovement.z;
                finalPosition.y += stepHeight;
                stepped = true;
                break;
            }
        }
        
        if (!stepped)
        {
            velocity.z = 0; // Stop horizontal movement
        }
    }
    
    return finalPosition;
}

// ============================================================================
// SPHERE COLLISION SYSTEM - SIMPLER THAN CAPSULE
// ============================================================================
// Sphere is perfect for small objects like fluid particles
// Much simpler than capsule - just one center point and radius

bool PhysicsSystem::checkChunkSphereCollision(const VoxelChunk* chunk, const Vec3& sphereCenter,
                                              const Vec3& /*chunkWorldPos*/, Vec3& outNormal, float radius)
{
    // Calculate AABB of sphere in chunk-local voxel coordinates
    int minX = static_cast<int>(std::floor(sphereCenter.x - radius));
    int maxX = static_cast<int>(std::ceil(sphereCenter.x + radius));
    int minY = static_cast<int>(std::floor(sphereCenter.y - radius));
    int maxY = static_cast<int>(std::ceil(sphereCenter.y + radius));
    int minZ = static_cast<int>(std::floor(sphereCenter.z - radius));
    int maxZ = static_cast<int>(std::ceil(sphereCenter.z + radius));
    
    // Clamp to chunk bounds
    minX = std::max(0, minX);
    maxX = std::min(VoxelChunk::SIZE - 1, maxX);
    minY = std::max(0, minY);
    maxY = std::min(VoxelChunk::SIZE - 1, maxY);
    minZ = std::max(0, minZ);
    maxZ = std::min(VoxelChunk::SIZE - 1, maxZ);
    
    // Check voxels within sphere AABB
    for (int x = minX; x <= maxX; ++x)
    {
        for (int y = minY; y <= maxY; ++y)
        {
            for (int z = minZ; z <= maxZ; ++z)
            {
                // Skip air blocks
                uint8_t blockType = chunk->getVoxel(x, y, z);
                if (blockType == 0)
                    continue;
                
                // Skip OBJ/model blocks (decorative only)
                auto& registry = BlockTypeRegistry::getInstance();
                const BlockTypeInfo* blockInfo = registry.getBlockType(blockType);
                if (blockInfo && blockInfo->renderType == BlockRenderType::OBJ) {
                    continue;
                }
                
                // Found solid voxel - do sphere-to-AABB collision
                Vec3 voxelMin(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                Vec3 voxelMax = voxelMin + Vec3(1.0f, 1.0f, 1.0f);
                
                // Find closest point on AABB to sphere center
                Vec3 closestPoint(
                    std::max(voxelMin.x, std::min(sphereCenter.x, voxelMax.x)),
                    std::max(voxelMin.y, std::min(sphereCenter.y, voxelMax.y)),
                    std::max(voxelMin.z, std::min(sphereCenter.z, voxelMax.z))
                );
                
                // Check if closest point is within sphere radius
                Vec3 delta = sphereCenter - closestPoint;
                float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
                
                if (distSq <= radius * radius)
                {
                    // Collision detected - calculate penetration normal
                    float dist = std::sqrt(distSq);
                    
                    if (dist > 0.0001f)
                    {
                        // Normal points from collision point toward sphere center (push-out direction)
                        outNormal = Vec3(delta.x / dist, delta.y / dist, delta.z / dist);
                    }
                    else
                    {
                        // Sphere center exactly on AABB surface - use voxel center direction
                        Vec3 voxelCenter = voxelMin + Vec3(0.5f, 0.5f, 0.5f);
                        Vec3 centerDelta = sphereCenter - voxelCenter;
                        
                        float absX = std::abs(centerDelta.x);
                        float absY = std::abs(centerDelta.y);
                        float absZ = std::abs(centerDelta.z);
                        
                        if (absX > absY && absX > absZ)
                            outNormal = Vec3(centerDelta.x > 0 ? 1.0f : -1.0f, 0, 0);
                        else if (absY > absZ)
                            outNormal = Vec3(0, centerDelta.y > 0 ? 1.0f : -1.0f, 0);
                        else
                            outNormal = Vec3(0, 0, centerDelta.z > 0 ? 1.0f : -1.0f);
                    }
                    
                    return true;
                }
            }
        }
    }
    
    return false;
}

bool PhysicsSystem::checkSphereCollision(const Vec3& sphereCenter, float radius, Vec3& outNormal, const FloatingIsland** outIsland)
{
    if (!m_islandSystem)
        return false;
    
    // SPATIAL CULLING: Only check islands within reasonable distance
    const float maxIslandCheckDistance = 512.0f;
    const float maxDistSq = maxIslandCheckDistance * maxIslandCheckDistance;
    
    // Iterate through all islands
    const auto& islands = m_islandSystem->getIslands();
    for (const auto& islandPair : islands)
    {
        const FloatingIsland* island = &islandPair.second;
        if (!island) continue;
        
        // OPTIMIZATION: Distance cull islands before expensive collision checks
        Vec3 delta = island->physicsCenter - sphereCenter;
        float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        if (distSq > maxDistSq)
            continue;  // Island too far away to collide
        
        // Transform sphere center to island-local space
        Vec3 localSphereCenter = island->worldToLocal(sphereCenter);
        
        // SPATIAL CULLING: Calculate which chunks could possibly contain the sphere
        float checkRadius = radius + VoxelChunk::SIZE;
        int minChunkX = static_cast<int>(std::floor((localSphereCenter.x - checkRadius) / VoxelChunk::SIZE));
        int maxChunkX = static_cast<int>(std::ceil((localSphereCenter.x + checkRadius) / VoxelChunk::SIZE));
        int minChunkY = static_cast<int>(std::floor((localSphereCenter.y - checkRadius) / VoxelChunk::SIZE));
        int maxChunkY = static_cast<int>(std::ceil((localSphereCenter.y + checkRadius) / VoxelChunk::SIZE));
        int minChunkZ = static_cast<int>(std::floor((localSphereCenter.z - checkRadius) / VoxelChunk::SIZE));
        int maxChunkZ = static_cast<int>(std::ceil((localSphereCenter.z + checkRadius) / VoxelChunk::SIZE));
        
        // Check only nearby chunks instead of ALL chunks
        for (int chunkX = minChunkX; chunkX <= maxChunkX; ++chunkX)
        {
            for (int chunkY = minChunkY; chunkY <= maxChunkY; ++chunkY)
            {
                for (int chunkZ = minChunkZ; chunkZ <= maxChunkZ; ++chunkZ)
                {
                    Vec3 chunkCoord(chunkX, chunkY, chunkZ);
                    auto chunkIt = island->chunks.find(chunkCoord);
                    if (chunkIt == island->chunks.end() || !chunkIt->second)
                        continue;
                    
                    const VoxelChunk* chunk = chunkIt->second.get();
                    if (!chunk) continue;
            
                    // Transform to chunk-local space
                    Vec3 chunkOrigin(
                        chunkX * VoxelChunk::SIZE,
                        chunkY * VoxelChunk::SIZE,
                        chunkZ * VoxelChunk::SIZE
                    );
                    Vec3 chunkLocalSphereCenter = localSphereCenter - chunkOrigin;
                    
                    // Check collision with this chunk
                    if (checkChunkSphereCollision(chunk, chunkLocalSphereCenter, chunkOrigin, outNormal, radius))
                    {
                        // Transform collision normal from island-local to world space
                        outNormal = island->localDirToWorld(outNormal);
                        
                        if (outIsland)
                            *outIsland = island;
                        
                        return true;
                    }
                }
            }
        }
    }
    
    return false;
}

GroundInfo PhysicsSystem::detectGroundSphere(const Vec3& sphereCenter, float radius, float rayMargin)
{
    GroundInfo result;
    result.isGrounded = false;
    
    if (!m_islandSystem)
        return result;
    
    // Cast ray downward from sphere bottom
    Vec3 rayStart = sphereCenter - Vec3(0, radius, 0);
    Vec3 rayEnd = rayStart - Vec3(0, rayMargin, 0);
    
    // Iterate through islands to find ground
    const auto& islands = m_islandSystem->getIslands();
    for (const auto& islandPair : islands)
    {
        const FloatingIsland* island = &islandPair.second;
        if (!island) continue;
        
        // Transform ray to island-local space
        Vec3 localRayStart = island->worldToLocal(rayStart);
        Vec3 localRayEnd = island->worldToLocal(rayEnd);
        
        // Check voxel at ray end position
        int voxelX = static_cast<int>(std::floor(localRayEnd.x));
        int voxelY = static_cast<int>(std::floor(localRayEnd.y));
        int voxelZ = static_cast<int>(std::floor(localRayEnd.z));
        
        uint8_t voxelType = m_islandSystem->getVoxelFromIsland(islandPair.first, Vec3(voxelX, voxelY, voxelZ));
        
        if (voxelType != 0)  // Not air
        {
            result.isGrounded = true;
            result.groundNormal = Vec3(0, 1, 0);  // Assume flat ground
            result.distanceToGround = rayMargin;
            return result;
        }
    }
    
    return result;
}

Vec3 PhysicsSystem::resolveSphereMovement(const Vec3& currentPos, Vec3& velocity, float deltaTime,
                                          float radius, float stepHeightRatio)
{
    PROFILE_FUNCTION();
    
    if (!m_islandSystem)
    {
        // No collision system - just apply velocity directly
        return currentPos + velocity * deltaTime;
    }
    
    // Calculate max step height (spheres use diameter as "height")
    float diameter = radius * 2.0f;
    float maxStepHeight = diameter * stepHeightRatio;
    
    // Calculate intended movement
    Vec3 intendedMovement = velocity * deltaTime;
    Vec3 intendedPosition = currentPos + intendedMovement;
    
    Vec3 collisionNormal;
    Vec3 finalPosition = currentPos;
    
    // Check if we're currently stuck
    bool wasStuck = checkSphereCollision(currentPos, radius, collisionNormal, nullptr);
    
    // If stuck, try to unstuck by pushing up
    if (wasStuck)
    {
        float unstuckIncrement = diameter * 0.2f;
        for (float unstuckHeight = unstuckIncrement; unstuckHeight <= maxStepHeight * 2.0f; unstuckHeight += unstuckIncrement)
        {
            Vec3 unstuckPos = currentPos + Vec3(0, unstuckHeight, 0);
            if (!checkSphereCollision(unstuckPos, radius, collisionNormal, nullptr))
            {
                finalPosition = unstuckPos;
                break;
            }
        }
    }
    
    // Check if intended position is valid
    if (!checkSphereCollision(intendedPosition, radius, collisionNormal, nullptr))
    {
        return intendedPosition;
    }
    
    // Collision detected - use axis-separated movement with step-up
    
    // Try vertical movement first
    Vec3 testPos = finalPosition + Vec3(0, intendedMovement.y, 0);
    if (!checkSphereCollision(testPos, radius, collisionNormal, nullptr))
    {
        finalPosition = testPos;
    }
    else
    {
        velocity.y = 0;
    }
    
    // Try X movement with step-up
    float stepIncrement = maxStepHeight * 0.25f;
    for (float stepHeight = stepIncrement; stepHeight <= maxStepHeight; stepHeight += stepIncrement)
    {
        Vec3 stepUpPos = finalPosition + Vec3(intendedMovement.x, stepHeight, 0);
        if (!checkSphereCollision(stepUpPos, radius, collisionNormal, nullptr))
        {
            finalPosition.x += intendedMovement.x;
            finalPosition.y += stepHeight;
            break;
        }
    }
    
    // Try Z movement with step-up
    for (float stepHeight = stepIncrement; stepHeight <= maxStepHeight; stepHeight += stepIncrement)
    {
        Vec3 stepUpPos = finalPosition + Vec3(0, stepHeight, intendedMovement.z);
        if (!checkSphereCollision(stepUpPos, radius, collisionNormal, nullptr))
        {
            finalPosition.z += intendedMovement.z;
            finalPosition.y += stepHeight;
            break;
        }
    }
    
    return finalPosition;
}

Vec3 PhysicsSystem::resolveFluidMovement(const Vec3& currentPos, Vec3& velocity, float deltaTime, float radius)
{
    PROFILE_FUNCTION();
    
    // NO COLLISION - pure noclip movement for testing
    // Just apply velocity directly
    return currentPos + velocity * deltaTime;
}

