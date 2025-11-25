// ConnectivityAnalyzer.cpp - Runtime island fragmentation detection
#include "ConnectivityAnalyzer.h"
#include "IslandChunkSystem.h"
#include "VoxelChunk.h"
#include "BlockType.h"
#include <iostream>

std::vector<uint32_t> ConnectivityAnalyzer::splitIslandByConnectivity(
    IslandChunkSystem* system,
    uint32_t originalIslandID)
{
    if (!system) return {};
    
    FloatingIsland* originalIsland = system->getIsland(originalIslandID);
    if (!originalIsland) return {};
    
    // Analyze connectivity - inline flood-fill to find all groups
    std::vector<ConnectedGroup> groups;
    std::unordered_set<Vec3> visited;
    
    for (const auto& [chunkCoord, chunk] : originalIsland->chunks)
    {
        if (!chunk) continue;
        
        Vec3 chunkWorldOffset = FloatingIsland::chunkCoordToWorldPos(chunkCoord);
        
        for (int x = 0; x < VoxelChunk::SIZE; x++)
        {
            for (int y = 0; y < VoxelChunk::SIZE; y++)
            {
                for (int z = 0; z < VoxelChunk::SIZE; z++)
                {
                    uint8_t voxelType = chunk->getVoxel(x, y, z);
                    if (voxelType == 0) continue;
                    
                    Vec3 islandRelativePos = chunkWorldOffset + Vec3(x, y, z);
                    
                    if (visited.find(islandRelativePos) == visited.end())
                    {
                        ConnectedGroup group = floodFill(originalIsland, islandRelativePos, visited);
                        if (group.voxelCount > 0)
                        {
                            groups.push_back(group);
                        }
                    }
                }
            }
        }
    }
    
    // If only one group, no split needed
    if (groups.size() <= 1) return {};
    
    std::cout << "[ISLAND SPLIT] Island " << originalIslandID 
              << " split into " << groups.size() << " separate groups!" << std::endl;
    
    // Find largest group (keep as original island)
    size_t largestGroupIndex = 0;
    size_t largestVoxelCount = 0;
    
    for (size_t i = 0; i < groups.size(); i++)
    {
        if (groups[i].voxelCount > largestVoxelCount)
        {
            largestVoxelCount = groups[i].voxelCount;
            largestGroupIndex = i;
        }
    }
    
    std::vector<uint32_t> newIslandIDs;
    
    // Create new islands for all groups except the largest
    for (size_t i = 0; i < groups.size(); i++)
    {
        if (i == largestGroupIndex)
        {
            // Keep largest group as original island - clear and rebuild
            // (Implementation would clear original island and repopulate with group voxels)
            continue;
        }
        
        // Create new island for this group
        const ConnectedGroup& group = groups[i];
        uint32_t newIslandID = system->createIsland(originalIsland->physicsCenter + group.centerOfMass);
        
        // Copy voxels from group to new island
        for (const Vec3& voxelPos : group.voxelPositions)
        {
            // Get voxel type from original island
            Vec3 chunkCoord = FloatingIsland::islandPosToChunkCoord(voxelPos);
            Vec3 localPos = FloatingIsland::islandPosToLocalPos(voxelPos);
            
            // Note: Would need to implement getVoxel from island-relative position
            // For now, this is a placeholder showing the approach
            // system->setVoxelWithMesh(newIslandID, voxelPos, voxelType);
        }
        
        // Inherit velocity from original island (with slight randomness for natural separation)
        FloatingIsland* newIsland = system->getIsland(newIslandID);
        if (newIsland)
        {
            newIsland->velocity = originalIsland->velocity;
            // Add slight separation velocity
            Vec3 separationDir = (group.centerOfMass - originalIsland->physicsCenter).normalized();
            newIsland->velocity = newIsland->velocity + separationDir * 2.0f;
            newIsland->invalidateTransform();
        }
        
        newIslandIDs.push_back(newIslandID);
        
        std::cout << "[NEW ISLAND] Created island " << newIslandID 
                  << " with " << group.voxelCount << " voxels" << std::endl;
    }
    
    return newIslandIDs;
}

ConnectedGroup ConnectivityAnalyzer::floodFill(
    const FloatingIsland* island,
    const Vec3& startPos,
    std::unordered_set<Vec3>& visited)
{
    ConnectedGroup group;
    group.voxelCount = 0;
    group.centerOfMass = Vec3(0, 0, 0);
    
    std::queue<Vec3> queue;
    queue.push(startPos);
    visited.insert(startPos);
    
    while (!queue.empty())
    {
        Vec3 current = queue.front();
        queue.pop();
        
        // Add to group
        group.voxelPositions.push_back(current);
        group.centerOfMass = group.centerOfMass + current;
        group.voxelCount++;
        
        // Check all 6 neighbors
        for (const Vec3& neighbor : getNeighbors(current))
        {
            // Skip if already visited
            if (visited.find(neighbor) != visited.end()) continue;
            
            // Skip if not solid
            if (!isSolidVoxel(island, neighbor)) continue;
            
            // Mark as visited and add to queue
            visited.insert(neighbor);
            queue.push(neighbor);
        }
    }
    
    // Calculate center of mass
    if (group.voxelCount > 0)
    {
        group.centerOfMass = group.centerOfMass / static_cast<float>(group.voxelCount);
    }
    
    return group;
}

std::vector<Vec3> ConnectivityAnalyzer::getNeighbors(const Vec3& pos)
{
    return {
        Vec3(pos.x + 1, pos.y, pos.z),  // +X
        Vec3(pos.x - 1, pos.y, pos.z),  // -X
        Vec3(pos.x, pos.y + 1, pos.z),  // +Y
        Vec3(pos.x, pos.y - 1, pos.z),  // -Y
        Vec3(pos.x, pos.y, pos.z + 1),  // +Z
        Vec3(pos.x, pos.y, pos.z - 1),  // -Z
    };
}

std::vector<Vec3> ConnectivityAnalyzer::getSolidNeighbors(const FloatingIsland* island, const Vec3& pos)
{
    std::vector<Vec3> solidNeighbors;
    for (const Vec3& neighbor : getNeighbors(pos))
    {
        if (isSolidVoxel(island, neighbor))
        {
            solidNeighbors.push_back(neighbor);
        }
    }
    return solidNeighbors;
}

int ConnectivityAnalyzer::floodFillCount(const FloatingIsland* island, const Vec3& startPos, const Vec3& excludePos)
{
    if (!isSolidVoxel(island, startPos))
        return 0;
    
    std::unordered_set<Vec3> visited;
    std::queue<Vec3> queue;
    queue.push(startPos);
    visited.insert(startPos);
    
    int count = 0;
    
    while (!queue.empty())
    {
        Vec3 current = queue.front();
        queue.pop();
        count++;
        
        for (const Vec3& neighbor : getNeighbors(current))
        {
            // Skip the excluded position (the broken block)
            if (neighbor == excludePos) continue;
            
            // Skip if already visited
            if (visited.find(neighbor) != visited.end()) continue;
            
            // Skip if not solid
            if (!isSolidVoxel(island, neighbor)) continue;
            
            visited.insert(neighbor);
            queue.push(neighbor);
        }
    }
    
    return count;
}

bool ConnectivityAnalyzer::wouldBreakingCauseSplit(const FloatingIsland* island, const Vec3& islandRelativePos, Vec3& outFragmentAnchor)
{
    if (!island) return false;
    
    // Check if the block being broken is actually solid
    if (!isSolidVoxel(island, islandRelativePos))
    {
        return false; // Can't split if we're not breaking a solid block
    }
    
    // Get all solid neighbors of the block we're about to break
    std::vector<Vec3> neighbors = getSolidNeighbors(island, islandRelativePos);
    
    std::cout << "[SPLIT CHECK] Block at (" << islandRelativePos.x << ", " << islandRelativePos.y << ", " << islandRelativePos.z 
              << ") has " << neighbors.size() << " solid neighbors" << std::endl;
    
    // Need at least 2 neighbors to potentially cause a split
    if (neighbors.size() < 2)
    {
        return false; // 0 or 1 neighbors can't cause a split
    }
    
    // Strategy: Run TWO parallel flood-fills from different neighbors
    // If they overlap, there's no split. If both complete without overlapping, there IS a split.
    // Extract whichever side is SMALLER to minimize network data.
    
    const int MAX_VOXELS_PER_SIDE = 5000; // Abort if one side exceeds this (too expensive)
    
    std::unordered_set<Vec3> visited1; // Flood-fill from first neighbor
    std::unordered_set<Vec3> visited2; // Flood-fill from second neighbor
    std::queue<Vec3> queue1;
    std::queue<Vec3> queue2;
    
    // Mark the broken block as excluded for both searches
    visited1.insert(islandRelativePos);
    visited2.insert(islandRelativePos);
    
    // Start from opposite neighbors
    queue1.push(neighbors[0]);
    visited1.insert(neighbors[0]);
    
    queue2.push(neighbors[1]);
    visited2.insert(neighbors[1]);
    
    bool flood1Complete = false;
    bool flood2Complete = false;
    bool overlap = false;
    
    // Expand both flood-fills in parallel until EITHER completes or they overlap
    // Whichever completes first is the smaller fragment
    while (!overlap && !flood1Complete && !flood2Complete)
    {
        // Expand flood-fill 1 by one layer
        if (!queue1.empty())
        {
            int layerSize = queue1.size();
            for (int i = 0; i < layerSize; i++)
            {
                Vec3 current = queue1.front();
                queue1.pop();
                
                for (const Vec3& neighbor : getNeighbors(current))
                {
                    if (visited1.find(neighbor) != visited1.end()) continue;
                    if (!isSolidVoxel(island, neighbor)) continue;
                    
                    // Check for overlap with flood-fill 2
                    if (visited2.find(neighbor) != visited2.end())
                    {
                        overlap = true;
                        break;
                    }
                    
                    visited1.insert(neighbor);
                    queue1.push(neighbor);
                }
                
                if (overlap) break;
            }
            
            if (queue1.empty()) flood1Complete = true;
            
            // Safety: abort if one side is too large
            if (visited1.size() > MAX_VOXELS_PER_SIDE)
            {
                std::cout << "[SPLIT CHECK] Side 1 too large (" << visited1.size() << " voxels), aborting" << std::endl;
                return false;
            }
        }
        
        if (overlap || flood1Complete) break;
        
        // Expand flood-fill 2 by one layer
        if (!queue2.empty())
        {
            int layerSize = queue2.size();
            for (int i = 0; i < layerSize; i++)
            {
                Vec3 current = queue2.front();
                queue2.pop();
                
                for (const Vec3& neighbor : getNeighbors(current))
                {
                    if (visited2.find(neighbor) != visited2.end()) continue;
                    if (!isSolidVoxel(island, neighbor)) continue;
                    
                    // Check for overlap with flood-fill 1
                    if (visited1.find(neighbor) != visited1.end())
                    {
                        overlap = true;
                        break;
                    }
                    
                    visited2.insert(neighbor);
                    queue2.push(neighbor);
                }
                
                if (overlap) break;
            }
            
            if (queue2.empty()) flood2Complete = true;
            
            // Safety: abort if one side is too large
            if (visited2.size() > MAX_VOXELS_PER_SIDE)
            {
                std::cout << "[SPLIT CHECK] Side 2 too large (" << visited2.size() << " voxels), aborting" << std::endl;
                return false;
            }
        }
    }
    
    // If the flood-fills overlapped, no split occurred
    if (overlap)
    {
        std::cout << "[SPLIT CHECK] No split - flood-fills overlapped (side1=" << visited1.size() 
                  << " side2=" << visited2.size() << ")" << std::endl;
        return false;
    }
    
    // One side completed first = it's the smaller fragment = SPLIT!
    std::cout << "[SPLIT CHECK] SPLIT DETECTED! Side 1=" << visited1.size() 
              << " voxels, Side 2=" << visited2.size() << " voxels" << std::endl;
    
    // Whichever side completed first is the smaller fragment
    if (flood1Complete)
    {
        outFragmentAnchor = neighbors[0];
        std::cout << "[SPLIT CHECK] Extracting side 1 (completed first with " << visited1.size() << " voxels)" << std::endl;
    }
    else
    {
        outFragmentAnchor = neighbors[1];
        std::cout << "[SPLIT CHECK] Extracting side 2 (completed first with " << visited2.size() << " voxels)" << std::endl;
    }
    
    return true;
}

uint32_t ConnectivityAnalyzer::extractFragmentToNewIsland(IslandChunkSystem* system, uint32_t originalIslandID, const Vec3& fragmentAnchor, std::vector<Vec3>* outRemovedVoxels)
{
    std::cout << "[EXTRACT] Starting fragment extraction from island " << originalIslandID 
              << " anchor=(" << fragmentAnchor.x << "," << fragmentAnchor.y << "," << fragmentAnchor.z << ")" << std::endl;
    
    if (!system) {
        std::cerr << "[EXTRACT] ERROR: Null system pointer" << std::endl;
        return 0;
    }
    
    FloatingIsland* mainIsland = system->getIsland(originalIslandID);
    if (!mainIsland) {
        std::cerr << "[EXTRACT] ERROR: Could not find island " << originalIslandID << std::endl;
        return 0;
    }
    
    // Flood-fill from fragment anchor to find all fragment voxels
    // LIMIT: Cap fragment size to prevent massive extractions from blocking network thread
    const int MAX_FRAGMENT_SIZE = 5000; // Don't extract fragments larger than 5000 blocks
    
    std::unordered_set<Vec3> fragmentVoxels;
    std::queue<Vec3> queue;
    queue.push(fragmentAnchor);
    fragmentVoxels.insert(fragmentAnchor);
    
    Vec3 centerOfMass(0, 0, 0);
    
    while (!queue.empty())
    {
        Vec3 current = queue.front();
        queue.pop();
        
        centerOfMass = centerOfMass + current;
        
        for (const Vec3& neighbor : getNeighbors(current))
        {
            if (fragmentVoxels.find(neighbor) != fragmentVoxels.end()) continue;
            if (!isSolidVoxel(mainIsland, neighbor)) continue;
            
            fragmentVoxels.insert(neighbor);
            queue.push(neighbor);
            
            // Safety check: Don't extract massive fragments (would stall network thread)
            if (fragmentVoxels.size() >= MAX_FRAGMENT_SIZE)
            {
                std::cerr << "[EXTRACT] WARNING: Fragment too large (" << fragmentVoxels.size() 
                          << " voxels), aborting extraction to prevent timeout" << std::endl;
                return 0;
            }
        }
    }
    
    if (fragmentVoxels.empty()) {
        std::cerr << "[EXTRACT] ERROR: Fragment flood-fill found 0 voxels" << std::endl;
        return 0;
    }
    
    std::cout << "[EXTRACT] Found fragment with " << fragmentVoxels.size() << " voxels" << std::endl;
    
    centerOfMass = centerOfMass / static_cast<float>(fragmentVoxels.size());
    
    // Create new island for fragment
    // Physics center should be in WORLD space (main island world pos + fragment's local center of mass)
    Vec3 worldCenterOfMass = mainIsland->physicsCenter + centerOfMass;
    
    std::cout << "[EXTRACT] Creating new island at world pos (" << worldCenterOfMass.x << "," 
              << worldCenterOfMass.y << "," << worldCenterOfMass.z << ")" << std::endl;
    
    uint32_t newIslandID = system->createIsland(worldCenterOfMass);
    FloatingIsland* newIsland = system->getIsland(newIslandID);
    
    if (!newIsland) return 0;
    
    // Copy voxels from main island to fragment island and remove from main
    for (const Vec3& voxelPos : fragmentVoxels)
    {
        // Get voxel type from main island (before we delete it)
        uint8_t voxelType = 0;
        Vec3 chunkCoord = FloatingIsland::islandPosToChunkCoord(voxelPos);
        Vec3 localPos = FloatingIsland::islandPosToLocalPos(voxelPos);
        
        auto it = mainIsland->chunks.find(chunkCoord);
        if (it != mainIsland->chunks.end() && it->second)
        {
            int lx = static_cast<int>(localPos.x);
            int ly = static_cast<int>(localPos.y);
            int lz = static_cast<int>(localPos.z);
            
            if (lx >= 0 && lx < VoxelChunk::SIZE &&
                ly >= 0 && ly < VoxelChunk::SIZE &&
                lz >= 0 && lz < VoxelChunk::SIZE)
            {
                voxelType = it->second->getVoxel(lx, ly, lz);
            }
        }
        
        if (voxelType != 0)
        {
            // Place voxel in new island at position relative to fragment's center of mass
            // This makes the fragment centered at (0,0,0) in the new island's local space
            Vec3 newIslandRelativePos = voxelPos - centerOfMass;
            
            // Use server-only method (no mesh generation on server)
            system->setVoxelServerOnly(newIslandID, newIslandRelativePos, voxelType);
            
            // Remove from main island (server-only, no mesh operations)
            system->setVoxelServerOnly(originalIslandID, voxelPos, 0);
            
            // Track removed voxel for network broadcast
            if (outRemovedVoxels)
            {
                outRemovedVoxels->push_back(voxelPos);
            }
        }
    }
    
    // Apply separation physics
    Vec3 separationDir = centerOfMass.normalized();
    if (separationDir.length() < 0.01f)
    {
        // If center of mass is at origin, use random direction
        separationDir = Vec3(1, 0, 0);
    }
    newIsland->velocity = mainIsland->velocity + separationDir * 0.5f;
    newIsland->invalidateTransform();
    
    std::cout << "🌊 Island split! Fragment with " << fragmentVoxels.size() 
              << " voxels broke off and became island " << newIslandID << std::endl;
    
    return newIslandID;
}

bool ConnectivityAnalyzer::isSolidVoxel(const FloatingIsland* island, const Vec3& islandRelativePos)
{
    if (!island) return false;
    
    // Convert to chunk coordinates
    Vec3 chunkCoord = FloatingIsland::islandPosToChunkCoord(islandRelativePos);
    Vec3 localPos = FloatingIsland::islandPosToLocalPos(islandRelativePos);
    
    // Get chunk
    auto it = island->chunks.find(chunkCoord);
    if (it == island->chunks.end()) return false;
    
    const VoxelChunk* chunk = it->second.get();
    if (!chunk) return false;
    
    // Get voxel
    int lx = static_cast<int>(localPos.x);
    int ly = static_cast<int>(localPos.y);
    int lz = static_cast<int>(localPos.z);
    
    if (lx < 0 || lx >= VoxelChunk::SIZE ||
        ly < 0 || ly >= VoxelChunk::SIZE ||
        lz < 0 || lz >= VoxelChunk::SIZE)
    {
        return false;
    }
    
    uint8_t voxelType = chunk->getVoxel(lx, ly, lz);
    
    // Ignore fluid blocks for connectivity - they don't provide structural support
    return (voxelType != 0 && voxelType != BlockID::WATER);
}
