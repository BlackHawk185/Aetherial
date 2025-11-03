// ConnectivityAnalyzer.h - Detects separate connected voxel groups
#pragma once

#include <vector>
#include <unordered_set>
#include <queue>
#include "../Math/Vec3.h"

struct FloatingIsland;
class IslandChunkSystem;

// Result of connectivity analysis
struct ConnectedGroup
{
    std::vector<Vec3> voxelPositions;  // All voxel positions in this group
    Vec3 centerOfMass;                  // Center of mass for physics
    size_t voxelCount;                  // Number of voxels
};

// Analyzes voxel connectivity for runtime island fragmentation
class ConnectivityAnalyzer
{
public:
    // **ULTRA-FAST SPLIT CHECK** - Check if breaking a block would split the island
    // Returns true if the block has exactly 2 non-adjacent neighbors (causing a split)
    // outFragmentAnchor will be set to one of the neighbors for fragment extraction
    static bool wouldBreakingCauseSplit(const FloatingIsland* island, const Vec3& islandRelativePos, Vec3& outFragmentAnchor);
    
    // **FRAGMENT EXTRACTION** - Extract a disconnected fragment to a new island
    // Flood-fills from fragmentAnchor to find all voxels in the fragment
    // Returns the ID of the newly created island
    // outRemovedVoxels will contain all voxel positions removed from the original island (for network sync)
    static uint32_t extractFragmentToNewIsland(IslandChunkSystem* system, uint32_t originalIslandID, const Vec3& fragmentAnchor, std::vector<Vec3>* outRemovedVoxels = nullptr);
    
    // Split an island into multiple islands based on connectivity
    // Returns the IDs of newly created islands (original island is modified to keep largest group)
    static std::vector<uint32_t> splitIslandByConnectivity(
        IslandChunkSystem* system, 
        uint32_t originalIslandID
    );

private:
    // 3D flood-fill to find all voxels connected to a starting position
    static ConnectedGroup floodFill(
        const FloatingIsland* island,
        const Vec3& startPos,
        std::unordered_set<Vec3>& visited
    );
    
    // Count voxels reachable from start position (for fragment size comparison)
    static int floodFillCount(const FloatingIsland* island, const Vec3& startPos, const Vec3& excludePos);
    
    // Get all solid neighbors of a position
    static std::vector<Vec3> getSolidNeighbors(const FloatingIsland* island, const Vec3& pos);
    
    // Get all 6 neighbors (±X, ±Y, ±Z) for connectivity check
    static std::vector<Vec3> getNeighbors(const Vec3& pos);
    
    // Check if a voxel exists at island-relative position
    static bool isSolidVoxel(const FloatingIsland* island, const Vec3& islandRelativePos);
};
