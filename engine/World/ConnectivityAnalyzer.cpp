// ConnectivityAnalyzer.cpp - Implementation of connectivity detection
#include "ConnectivityAnalyzer.h"
#include "IslandChunkSystem.h"
#include "VoxelChunk.h"
#include <iostream>
#include <chrono>

std::vector<ConnectedGroup> ConnectivityAnalyzer::analyzeIsland(const FloatingIsland* island)
{
    if (!island) return {};
    
    std::vector<ConnectedGroup> groups;
    std::unordered_set<Vec3> visited;
    
    // Iterate through all chunks and their voxels to find starting points
    for (const auto& [chunkCoord, chunk] : island->chunks)
    {
        if (!chunk) continue;
        
        // Check each voxel in the chunk
        Vec3 chunkWorldOffset = FloatingIsland::chunkCoordToWorldPos(chunkCoord);
        
        for (int x = 0; x < VoxelChunk::SIZE; x++)
        {
            for (int y = 0; y < VoxelChunk::SIZE; y++)
            {
                for (int z = 0; z < VoxelChunk::SIZE; z++)
                {
                    uint8_t voxelType = chunk->getVoxel(x, y, z);
                    if (voxelType == 0) continue;  // Skip air
                    
                    // Convert to island-relative position
                    Vec3 islandRelativePos = chunkWorldOffset + Vec3(x, y, z);
                    
                    // If not visited, start a new flood-fill
                    if (visited.find(islandRelativePos) == visited.end())
                    {
                        ConnectedGroup group = floodFill(island, islandRelativePos, visited);
                        if (group.voxelCount > 0)
                        {
                            groups.push_back(group);
                        }
                    }
                }
            }
        }
    }
    
    return groups;
}

int ConnectivityAnalyzer::cleanupSatellites(FloatingIsland* island, const Vec3& mainIslandAnchor)
{
    if (!island) return 0;
    
    auto totalStart = std::chrono::high_resolution_clock::now();
    
    // **FAST PATH**: Single flood-fill from anchor point to mark main island
    std::unordered_set<Vec3> mainIslandVoxels;
    
    auto anchorCheckStart = std::chrono::high_resolution_clock::now();
    
    long long anchorCheckDuration = 0;
    long long findSolidDuration = 0;
    long long floodFillDuration = 0;
    
    // Check if anchor point is solid
    if (!isSolidVoxel(island, mainIslandAnchor))
    {
        auto anchorCheckEnd = std::chrono::high_resolution_clock::now();
        anchorCheckDuration = std::chrono::duration_cast<std::chrono::milliseconds>(anchorCheckEnd - anchorCheckStart).count();
        
        auto findSolidStart = std::chrono::high_resolution_clock::now();
        
        // Anchor is air - find nearest solid voxel (fallback)
        bool foundSolid = false;
        Vec3 solidPos;
        
        for (const auto& [chunkCoord, chunk] : island->chunks)
        {
            if (!chunk || foundSolid) break;
            
            Vec3 chunkWorldOffset = FloatingIsland::chunkCoordToWorldPos(chunkCoord);
            for (int x = 0; x < VoxelChunk::SIZE && !foundSolid; x++)
            {
                for (int y = 0; y < VoxelChunk::SIZE && !foundSolid; y++)
                {
                    for (int z = 0; z < VoxelChunk::SIZE && !foundSolid; z++)
                    {
                        if (chunk->getVoxel(x, y, z) != 0)
                        {
                            solidPos = chunkWorldOffset + Vec3(x, y, z);
                            foundSolid = true;
                        }
                    }
                }
            }
        }
        
        if (!foundSolid)
        {
            // No solid voxels found - nothing to clean up
            return 0;
        }
        
        auto findSolidEnd = std::chrono::high_resolution_clock::now();
        findSolidDuration = std::chrono::duration_cast<std::chrono::milliseconds>(findSolidEnd - findSolidStart).count();
        
        auto floodFillStart = std::chrono::high_resolution_clock::now();
        
        // Start flood-fill from first solid voxel found
        std::queue<Vec3> queue;
        queue.push(solidPos);
        mainIslandVoxels.insert(solidPos);
        
        // Flood-fill to mark all connected voxels
        while (!queue.empty())
        {
            Vec3 current = queue.front();
            queue.pop();
            
            for (const Vec3& neighbor : getNeighbors(current))
            {
                if (mainIslandVoxels.find(neighbor) != mainIslandVoxels.end()) continue;
                if (!isSolidVoxel(island, neighbor)) continue;
                
                mainIslandVoxels.insert(neighbor);
                queue.push(neighbor);
            }
        }
        
        auto floodFillEnd = std::chrono::high_resolution_clock::now();
        floodFillDuration = std::chrono::duration_cast<std::chrono::milliseconds>(floodFillEnd - floodFillStart).count();
        
        std::cout << "   ├─ Anchor Check: " << anchorCheckDuration << "ms (anchor was air)" << std::endl;
        std::cout << "   ├─ Find Solid: " << findSolidDuration << "ms" << std::endl;
        std::cout << "   └─ Flood Fill: " << floodFillDuration << "ms (" << mainIslandVoxels.size() << " voxels marked)" << std::endl;
    }
    else
    {
        auto anchorCheckEnd = std::chrono::high_resolution_clock::now();
        anchorCheckDuration = std::chrono::duration_cast<std::chrono::milliseconds>(anchorCheckEnd - anchorCheckStart).count();
        
        auto floodFillStart = std::chrono::high_resolution_clock::now();
        
        // Anchor is solid - flood-fill from anchor point
        std::queue<Vec3> queue;
        queue.push(mainIslandAnchor);
        mainIslandVoxels.insert(mainIslandAnchor);
        
        while (!queue.empty())
        {
            Vec3 current = queue.front();
            queue.pop();
            
            for (const Vec3& neighbor : getNeighbors(current))
            {
                if (mainIslandVoxels.find(neighbor) != mainIslandVoxels.end()) continue;
                if (!isSolidVoxel(island, neighbor)) continue;
                
                mainIslandVoxels.insert(neighbor);
                queue.push(neighbor);
            }
        }
        
        auto floodFillEnd = std::chrono::high_resolution_clock::now();
        floodFillDuration = std::chrono::duration_cast<std::chrono::milliseconds>(floodFillEnd - floodFillStart).count();
        
        std::cout << "   ├─ Anchor Check: " << anchorCheckDuration << "ms (anchor was solid)" << std::endl;
        std::cout << "   └─ Flood Fill: " << floodFillDuration << "ms (" << mainIslandVoxels.size() << " voxels marked)" << std::endl;
    }
    
    auto deletionStart = std::chrono::high_resolution_clock::now();
    
    // Delete everything NOT in main island
    int voxelsRemoved = 0;
    for (auto& [chunkCoord, chunk] : island->chunks)
    {
        if (!chunk) continue;
        
        Vec3 chunkWorldOffset = FloatingIsland::chunkCoordToWorldPos(chunkCoord);
        
        for (int x = 0; x < VoxelChunk::SIZE; x++)
        {
            for (int y = 0; y < VoxelChunk::SIZE; y++)
            {
                for (int z = 0; z < VoxelChunk::SIZE; z++)
                {
                    if (chunk->getVoxel(x, y, z) == 0) continue;
                    
                    Vec3 islandRelativePos = chunkWorldOffset + Vec3(x, y, z);
                    
                    // If this voxel is NOT in the main island, remove it
                    if (mainIslandVoxels.find(islandRelativePos) == mainIslandVoxels.end())
                    {
                        chunk->setVoxel(x, y, z, 0);
                        voxelsRemoved++;
                    }
                }
            }
        }
    }
    
    auto deletionEnd = std::chrono::high_resolution_clock::now();
    auto deletionDuration = std::chrono::duration_cast<std::chrono::milliseconds>(deletionEnd - deletionStart).count();
    
    std::cout << "   └─ Satellite Deletion: " << deletionDuration << "ms (" << voxelsRemoved << " voxels removed)" << std::endl;
    
    return voxelsRemoved;
}

std::vector<uint32_t> ConnectivityAnalyzer::splitIslandByConnectivity(
    IslandChunkSystem* system,
    uint32_t originalIslandID)
{
    if (!system) return {};
    
    FloatingIsland* originalIsland = system->getIsland(originalIslandID);
    if (!originalIsland) return {};
    
    // Analyze connectivity
    std::vector<ConnectedGroup> groups = analyzeIsland(originalIsland);
    
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
            // system->setVoxelInIsland(newIslandID, voxelPos, voxelType);
        }
        
        // Inherit velocity from original island (with slight randomness for natural separation)
        FloatingIsland* newIsland = system->getIsland(newIslandID);
        if (newIsland)
        {
            newIsland->velocity = originalIsland->velocity;
            // Add slight separation velocity
            Vec3 separationDir = (group.centerOfMass - originalIsland->physicsCenter).normalized();
            newIsland->velocity = newIsland->velocity + separationDir * 2.0f;
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
    
    // Get all solid neighbors of the block we're about to break
    std::vector<Vec3> neighbors = getSolidNeighbors(island, islandRelativePos);
    
    // Only blocks with exactly 2 neighbors can cause a split
    if (neighbors.size() != 2)
    {
        return false; // Can't split with 0, 1, 3, 4, 5, or 6 neighbors
    }
    
    // Check if the 2 neighbors are adjacent to each other (6-way connectivity)
    Vec3 diff = neighbors[0] - neighbors[1];
    int manhattanDist = abs(diff.x) + abs(diff.y) + abs(diff.z);
    
    if (manhattanDist == 1)
    {
        // Neighbors are adjacent - they can connect directly, no split
        return false;
    }
    
    // The 2 neighbors are NOT adjacent - breaking this block will split the island!
    // OPTIMIZATION: Race flood-fill from both neighbors to find which side is smaller
    // This avoids flood-filling a massive island when we only need the small fragment
    
    std::unordered_set<Vec3> visited0;
    std::unordered_set<Vec3> visited1;
    std::queue<Vec3> queue0;
    std::queue<Vec3> queue1;
    
    visited0.insert(islandRelativePos); // Exclude the broken block
    visited1.insert(islandRelativePos);
    
    queue0.push(neighbors[0]);
    queue1.push(neighbors[1]);
    visited0.insert(neighbors[0]);
    visited1.insert(neighbors[1]);
    
    int count0 = 1;
    int count1 = 1;
    
    // Race both flood-fills, alternating one step at a time
    while (!queue0.empty() || !queue1.empty())
    {
        // Expand fragment 0 by one layer
        if (!queue0.empty())
        {
            int layerSize = queue0.size();
            for (int i = 0; i < layerSize; i++)
            {
                Vec3 current = queue0.front();
                queue0.pop();
                
                for (const Vec3& neighbor : getNeighbors(current))
                {
                    if (visited0.find(neighbor) != visited0.end()) continue;
                    if (!isSolidVoxel(island, neighbor)) continue;
                    
                    visited0.insert(neighbor);
                    queue0.push(neighbor);
                    count0++;
                }
            }
        }
        
        // Expand fragment 1 by one layer
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
                    
                    visited1.insert(neighbor);
                    queue1.push(neighbor);
                    count1++;
                }
            }
        }
        
        // If one side finished (found all its voxels), it's the smaller fragment
        if (queue0.empty() && !queue1.empty())
        {
            outFragmentAnchor = neighbors[0];
            return true;
        }
        if (queue1.empty() && !queue0.empty())
        {
            outFragmentAnchor = neighbors[1];
            return true;
        }
    }
    
    // Both finished at same time - pick the smaller count
    outFragmentAnchor = (count0 <= count1) ? neighbors[0] : neighbors[1];
    return true;
}

uint32_t ConnectivityAnalyzer::extractFragmentToNewIsland(IslandChunkSystem* system, uint32_t originalIslandID, const Vec3& fragmentAnchor, std::vector<Vec3>* outRemovedVoxels)
{
    if (!system) return 0;
    
    FloatingIsland* mainIsland = system->getIsland(originalIslandID);
    if (!mainIsland) return 0;
    
    // Flood-fill from fragment anchor to find all fragment voxels
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
        }
    }
    
    if (fragmentVoxels.empty()) return 0;
    
    centerOfMass = centerOfMass / static_cast<float>(fragmentVoxels.size());
    
    // Create new island for fragment
    // Physics center should be in WORLD space (main island world pos + fragment's local center of mass)
    Vec3 worldCenterOfMass = mainIsland->physicsCenter + centerOfMass;
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
            system->setVoxelInIsland(newIslandID, newIslandRelativePos, voxelType);
            
            // Remove from main island using setVoxelInIsland to properly rebuild meshes
            system->setVoxelInIsland(originalIslandID, voxelPos, 0);
            
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
    
    return (chunk->getVoxel(lx, ly, lz) != 0);
}
