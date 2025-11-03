// VoxelRaycaster.cpp - Integrated thin wrapper around IslandChunkSystem
#include "VoxelRaycaster.h"

#include <algorithm>
#include <cmath>
#include <iostream>

#include "VoxelChunk.h"
#include "World/IslandChunkSystem.h"

RayHit VoxelRaycaster::raycast(const Vec3& origin, const Vec3& direction, float maxDistance,
                               IslandChunkSystem* islandSystem)
{
    return performDDA(origin, direction, maxDistance, islandSystem);
}

Vec3 VoxelRaycaster::getPlacementPosition(const RayHit& hit)
{
    if (!hit.hit)
        return Vec3(0.0f, 0.0f, 0.0f);

    // **INTEGRATED APPROACH**: Use island-relative coordinates with proper normal
    // Place block adjacent to hit face (in island-relative coordinates)
    return hit.localBlockPos + hit.normal;
}

RayHit VoxelRaycaster::performDDA(const Vec3& rayStart, const Vec3& rayDirection, float maxDistance,
                                  IslandChunkSystem* islandSystem)
{
    RayHit result;
    
    // Normalize direction
    Vec3 dir = rayDirection;
    float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (length < 0.0001f) {
        return result; // Invalid direction
    }
    dir.x /= length;
    dir.y /= length;
    dir.z /= length;
    
    const auto& islands = islandSystem->getIslands();
    
    RayHit closestHit;
    closestHit.distance = maxDistance + 1.0f;
    
    // Check each island for intersection (islands are spatially separate)
    for (const auto& [islandID, island] : islands)
    {
        // Calculate actual island bounds from its chunks
        if (island.chunks.empty()) continue;
        
        // Get min/max chunk coordinates
        int minChunkX = 999999, minChunkY = 999999, minChunkZ = 999999;
        int maxChunkX = -999999, maxChunkY = -999999, maxChunkZ = -999999;
        
        for (const auto& [chunkCoord, chunk] : island.chunks) {
            minChunkX = std::min(minChunkX, static_cast<int>(chunkCoord.x));
            minChunkY = std::min(minChunkY, static_cast<int>(chunkCoord.y));
            minChunkZ = std::min(minChunkZ, static_cast<int>(chunkCoord.z));
            maxChunkX = std::max(maxChunkX, static_cast<int>(chunkCoord.x));
            maxChunkY = std::max(maxChunkY, static_cast<int>(chunkCoord.y));
            maxChunkZ = std::max(maxChunkZ, static_cast<int>(chunkCoord.z));
        }
        
        // Convert chunk bounds to world space with proper margin
        Vec3 islandMin = island.physicsCenter + Vec3(
            minChunkX * VoxelChunk::SIZE - 1,
            minChunkY * VoxelChunk::SIZE - 1,
            minChunkZ * VoxelChunk::SIZE - 1
        );
        Vec3 islandMax = island.physicsCenter + Vec3(
            (maxChunkX + 1) * VoxelChunk::SIZE + 1,
            (maxChunkY + 1) * VoxelChunk::SIZE + 1,
            (maxChunkZ + 1) * VoxelChunk::SIZE + 1
        );
        
        // Simple ray-AABB intersection test
        float tMin = 0.0f;
        float tMax = maxDistance;
        
        for (int i = 0; i < 3; ++i) {
            float invD = 1.0f / (i == 0 ? dir.x : (i == 1 ? dir.y : dir.z));
            float origin = (i == 0 ? rayStart.x : (i == 1 ? rayStart.y : rayStart.z));
            float boxMin = (i == 0 ? islandMin.x : (i == 1 ? islandMin.y : islandMin.z));
            float boxMax = (i == 0 ? islandMax.x : (i == 1 ? islandMax.y : islandMax.z));
            
            float t0 = (boxMin - origin) * invD;
            float t1 = (boxMax - origin) * invD;
            if (invD < 0.0f) {
                float tmp = t0;
                t0 = t1;
                t1 = tmp;
            }
            
            tMin = t0 > tMin ? t0 : tMin;
            tMax = t1 < tMax ? t1 : tMax;
            
            if (tMax < tMin) {
                goto next_island; // Ray misses this island's AABB
            }
        }
        
        // Ray hits island AABB - perform DDA in island-local space
        {
            // Transform world-space ray to island-local space (accounts for rotation!)
            Vec3 localStart = island.worldToLocal(rayStart);
            Vec3 localDir = island.worldDirToLocal(dir);
            
            // DDA voxel traversal (Amanatides & Woo algorithm)
            int x = static_cast<int>(std::floor(localStart.x));
            int y = static_cast<int>(std::floor(localStart.y));
            int z = static_cast<int>(std::floor(localStart.z));
            
            int stepX = localDir.x > 0 ? 1 : -1;
            int stepY = localDir.y > 0 ? 1 : -1;
            int stepZ = localDir.z > 0 ? 1 : -1;
            
            float tDeltaX = (localDir.x != 0) ? std::abs(1.0f / localDir.x) : 1e30f;
            float tDeltaY = (localDir.y != 0) ? std::abs(1.0f / localDir.y) : 1e30f;
            float tDeltaZ = (localDir.z != 0) ? std::abs(1.0f / localDir.z) : 1e30f;
            
            float tMaxX, tMaxY, tMaxZ;
            
            if (localDir.x > 0) {
                tMaxX = (std::floor(localStart.x) + 1.0f - localStart.x) * tDeltaX;
            } else {
                tMaxX = (localStart.x - std::floor(localStart.x)) * tDeltaX;
            }
            
            if (localDir.y > 0) {
                tMaxY = (std::floor(localStart.y) + 1.0f - localStart.y) * tDeltaY;
            } else {
                tMaxY = (localStart.y - std::floor(localStart.y)) * tDeltaY;
            }
            
            if (localDir.z > 0) {
                tMaxZ = (std::floor(localStart.z) + 1.0f - localStart.z) * tDeltaZ;
            } else {
                tMaxZ = (localStart.z - std::floor(localStart.z)) * tDeltaZ;
            }
            
            Vec3 normal(0, 0, 0);
            float currentDistance = 0.0f;
            const int maxSteps = static_cast<int>(maxDistance * 2.0f);
            
            for (int step = 0; step < maxSteps; ++step)
            {
                // Check voxel at current position
                Vec3 checkPos(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                uint8_t blockID = islandSystem->getVoxelFromIsland(islandID, checkPos);
                
                if (blockID != 0) // Hit solid block
                {
                    // Track this hit if it's closer than our current best
                    if (currentDistance < closestHit.distance) {
                        closestHit.hit = true;
                        closestHit.islandID = islandID;
                        closestHit.localBlockPos = checkPos;
                        closestHit.normal = normal;
                        closestHit.distance = currentDistance;
                    }
                    break; // Found hit on this island, check other islands
                }
                
                // Step to next voxel boundary
                if (tMaxX < tMaxY) {
                    if (tMaxX < tMaxZ) {
                        currentDistance = tMaxX;
                        x += stepX;
                        tMaxX += tDeltaX;
                        normal = Vec3(static_cast<float>(-stepX), 0, 0);
                    } else {
                        currentDistance = tMaxZ;
                        z += stepZ;
                        tMaxZ += tDeltaZ;
                        normal = Vec3(0, 0, static_cast<float>(-stepZ));
                    }
                } else {
                    if (tMaxY < tMaxZ) {
                        currentDistance = tMaxY;
                        y += stepY;
                        tMaxY += tDeltaY;
                        normal = Vec3(0, static_cast<float>(-stepY), 0);
                    } else {
                        currentDistance = tMaxZ;
                        z += stepZ;
                        tMaxZ += tDeltaZ;
                        normal = Vec3(0, 0, static_cast<float>(-stepZ));
                    }
                }
                
                if (currentDistance > maxDistance) {
                    break; // Exceeded max distance
                }
            }
        }
        
        next_island:;
    }
    
    return closestHit; // Return closest hit across all islands (or miss if none)
}
