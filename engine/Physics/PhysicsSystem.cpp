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

            auto mesh = chunk->getCollisionMesh();
            int faces = mesh ? mesh->faces.size() : 0;
            totalFaces += faces;
            std::cout << "    Chunk at (" << chunkPair.first.x << ", " << chunkPair.first.y << ", " << chunkPair.first.z << "): " << faces << " collision faces" << std::endl;
        }
    }

    std::cout << "Total collision faces: " << totalFaces << std::endl;
    std::cout << "==========================" << std::endl;
}

int PhysicsSystem::getTotalCollisionFaces() const
{
    if (!m_islandSystem) return 0;

    int total = 0;
    const auto& islands = m_islandSystem->getIslands();
    for (const auto& islandPair : islands)
    {
        const FloatingIsland* island = &islandPair.second;
        if (!island) continue;

        for (const auto& chunkPair : island->chunks)
        {
            const VoxelChunk* chunk = chunkPair.second.get();
            if (chunk)
            {
                auto mesh = chunk->getCollisionMesh();
                if (mesh)
                    total += mesh->faces.size();
            }
        }
    }
    return total;
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

// OLD APPROACH (KEPT FOR REFERENCE - DELETE AFTER TESTING):
// This iterated ALL collision faces (10K-100K at 256^3 chunks)
// New approach checks only voxels within capsule AABB (~10-50 voxels)
/*
bool PhysicsSystem::checkChunkCapsuleCollision_OLD_FACE_ITERATION(const VoxelChunk* chunk, const Vec3& capsuleCenter,
                                               float radius, float height)
{
    auto collisionMesh = chunk->getCollisionMesh();
    if (!collisionMesh)
        return false;
    
    float cylinderHalfHeight = (height - 2.0f * radius) * 0.5f;
    Vec3 topSphereCenter = capsuleCenter + Vec3(0, cylinderHalfHeight, 0);
    Vec3 bottomSphereCenter = capsuleCenter - Vec3(0, cylinderHalfHeight, 0);
    
    for (const auto& face : collisionMesh->faces)
    {
        // ... old face iteration logic ...
    }
    
    return false;
}
*/

bool PhysicsSystem::checkCapsuleCollision(const Vec3& capsuleCenter, float radius, float height,
                                         Vec3& outNormal, const FloatingIsland** outIsland)
{
    PROFILE_FUNCTION();
    if (!m_islandSystem)
        return false;
    
    const auto& islands = m_islandSystem->getIslands();
    
    for (const auto& islandPair : islands)
    {
        const FloatingIsland* island = &islandPair.second;
        if (!island)
            continue;
        
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
