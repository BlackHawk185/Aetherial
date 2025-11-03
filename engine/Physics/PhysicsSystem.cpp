// PhysicsSystem.cpp - Basic collision detection system
#include "PhysicsSystem.h"

#include <iostream>
#include <cmath>

#include "../World/IslandChunkSystem.h"
#include "../World/VoxelChunk.h"
#include "../World/BlockType.h"  // For BlockTypeRegistry and BlockTypeInfo
#include "../World/VoxelRaycaster.h"  // For DDA-based ground detection
#include "../Profiling/Profiler.h"

PhysicsSystem g_physics;

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

bool PhysicsSystem::checkRayCollision(const Vec3& rayOrigin, const Vec3& rayDirection,
                                      float maxDistance, Vec3& hitPoint, Vec3& hitNormal)
{
    PROFILE_FUNCTION();
    if (!m_islandSystem)
        return false;

    // Check ray collision with all active islands
    const auto& islands = m_islandSystem->getIslands();
    for (const auto& islandPair : islands)
    {
        const FloatingIsland* island = &islandPair.second;
        if (!island)
            continue;

        // Convert ray origin to island-local coordinates
        Vec3 localRayOrigin = rayOrigin - island->physicsCenter;
        Vec3 rayEnd = localRayOrigin + (rayDirection * maxDistance);
        
        // Calculate bounding box of ray path
        float minX = std::min(localRayOrigin.x, rayEnd.x);
        float maxX = std::max(localRayOrigin.x, rayEnd.x);
        float minY = std::min(localRayOrigin.y, rayEnd.y);
        float maxY = std::max(localRayOrigin.y, rayEnd.y);
        float minZ = std::min(localRayOrigin.z, rayEnd.z);
        float maxZ = std::max(localRayOrigin.z, rayEnd.z);
        
        // Calculate chunk coordinate bounds that the ray could intersect
        int minChunkX = static_cast<int>(std::floor(minX / VoxelChunk::SIZE));
        int maxChunkX = static_cast<int>(std::ceil(maxX / VoxelChunk::SIZE));
        int minChunkY = static_cast<int>(std::floor(minY / VoxelChunk::SIZE));
        int maxChunkY = static_cast<int>(std::ceil(maxY / VoxelChunk::SIZE));
        int minChunkZ = static_cast<int>(std::floor(minZ / VoxelChunk::SIZE));
        int maxChunkZ = static_cast<int>(std::ceil(maxZ / VoxelChunk::SIZE));

        // Only check chunks that the ray could potentially intersect
        for (int chunkX = minChunkX; chunkX <= maxChunkX; ++chunkX)
        {
            for (int chunkY = minChunkY; chunkY <= maxChunkY; ++chunkY)
            {
                for (int chunkZ = minChunkZ; chunkZ <= maxChunkZ; ++chunkZ)
                {
                    Vec3 chunkCoord(chunkX, chunkY, chunkZ);
                    
                    // Check if this chunk exists in the island
                    auto chunkIt = island->chunks.find(chunkCoord);
                    if (chunkIt == island->chunks.end() || !chunkIt->second)
                        continue;

                    // Calculate chunk world position
                    Vec3 chunkWorldPos = island->physicsCenter + FloatingIsland::chunkCoordToWorldPos(chunkCoord);
                    
                    // Convert ray to chunk-local coordinates
                    Vec3 chunkLocalRayOrigin = rayOrigin - chunkWorldPos;

                    // Check ray collision with this chunk
                    Vec3 localHitPoint, localHitNormal;
                    if (chunkIt->second->checkRayCollision(chunkLocalRayOrigin, rayDirection, maxDistance,
                                                          localHitPoint, localHitNormal))
                    {
                        // Convert back to world coordinates
                        hitPoint = localHitPoint + chunkWorldPos;
                        hitNormal = localHitNormal;
                        return true;
                    }
                }
            }
        }
    }

    return false;
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
// CAPSULE COLLISION SYSTEM
// ============================================================================
// Capsule is a cylinder with hemispherical caps on top and bottom
// Perfect for humanoid character collision (narrow width, proper height)

bool PhysicsSystem::checkChunkCapsuleCollision(const VoxelChunk* chunk, const Vec3& capsuleCenter,
                                               const Vec3& /*chunkWorldPos*/, Vec3& outNormal,
                                               float radius, float height)
{
    // Get collision mesh (thread-safe atomic load)
    auto collisionMesh = chunk->getCollisionMesh();
    if (!collisionMesh)
        return false;
    
    // Capsule breakdown:
    // - Total height: height
    // - Cylinder height: height - 2*radius (middle section)
    // - Top sphere center: capsuleCenter + (0, cylinderHeight/2, 0)
    // - Bottom sphere center: capsuleCenter - (0, cylinderHeight/2, 0)
    
    float cylinderHalfHeight = (height - 2.0f * radius) * 0.5f;
    Vec3 topSphereCenter = capsuleCenter + Vec3(0, cylinderHalfHeight, 0);
    Vec3 bottomSphereCenter = capsuleCenter - Vec3(0, cylinderHalfHeight, 0);
    
    for (const auto& face : collisionMesh->faces)
    {
        Vec3 faceToCenter = capsuleCenter - face.position;
        float distanceToPlane = faceToCenter.dot(face.normal);
        
        // Check if capsule intersects this face's plane
        // For a capsule, we need to check:
        // 1. The cylindrical middle section (XZ circle at multiple Y heights)
        // 2. The top hemisphere
        // 3. The bottom hemisphere
        
        // Quick reject: if center is too far from plane
        if (abs(distanceToPlane) > (height * 0.5f + 0.1f))
            continue;
        
        // Determine which part of the capsule to test based on face position
        Vec3 closestPointOnAxis;
        
        // Project face position onto capsule's vertical axis
        float yOffset = (face.position.y - capsuleCenter.y);
        
        if (yOffset > cylinderHalfHeight)
        {
            // Check top hemisphere
            closestPointOnAxis = topSphereCenter;
        }
        else if (yOffset < -cylinderHalfHeight)
        {
            // Check bottom hemisphere
            closestPointOnAxis = bottomSphereCenter;
        }
        else
        {
            // Check cylinder - clamp to cylinder height
            closestPointOnAxis = capsuleCenter + Vec3(0, yOffset, 0);
        }
        
        // Now do sphere-to-face test from closestPointOnAxis
        Vec3 faceToPoint = closestPointOnAxis - face.position;
        float distToPlane = faceToPoint.dot(face.normal);
        
        if (abs(distToPlane) <= radius)
        {
            // Project point onto face plane
            Vec3 projectedPoint = closestPointOnAxis - face.normal * distToPlane;
            Vec3 localPoint = projectedPoint - face.position;
            
            // Check if projected point overlaps with face bounds (using greedy merged dimensions)
            float halfWidth = face.width * 0.5f;
            float halfHeight = face.height * 0.5f;
            
            bool withinBounds = true;
            if (abs(face.normal.x) > 0.5f)
            {
                // X-facing face - check YZ bounds (width=Z, height=Y)
                withinBounds = (abs(localPoint.y) <= (halfHeight + radius)) && 
                              (abs(localPoint.z) <= (halfWidth + radius));
            }
            else if (abs(face.normal.z) > 0.5f)
            {
                // Z-facing face - check XY bounds (width=X, height=Y)
                withinBounds = (abs(localPoint.x) <= (halfWidth + radius)) && 
                              (abs(localPoint.y) <= (halfHeight + radius));
            }
            else
            {
                // Y-facing face - check XZ bounds (width=X, height=Z)
                withinBounds = (abs(localPoint.x) <= (halfWidth + radius)) && 
                              (abs(localPoint.z) <= (halfHeight + radius));
            }
            
            if (withinBounds)
            {
                outNormal = face.normal;
                return true;
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
