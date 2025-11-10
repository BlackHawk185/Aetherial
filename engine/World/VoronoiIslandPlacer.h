// VoronoiIslandPlacer.h - Procedural island placement using Voronoi/Cellular noise
#pragma once

#include "../Math/Vec3.h"
#include "BiomeSystem.h"
#include <vector>
#include <cstdint>

struct IslandDefinition {
    Vec3 position;      // Island center position
    float radius;       // Island size/radius
    uint32_t seed;      // Unique seed for terrain generation
    BiomeType biome;    // Biome type for this island
};

class VoronoiIslandPlacer {
public:
    VoronoiIslandPlacer();
    
    // Tunable parameters for advanced control
    float verticalSpreadMultiplier = 100.0f;    // Vertical Y-axis spread (±units)
    float heightNoiseFrequency = 0.005f;         // Frequency for vertical variation
    float cellCenterThreshold = 0.1f;            // Threshold for detecting cell centers (lower = stricter)
    
    // Generate island placements using Voronoi/Cellular noise
    // worldSeed: Master seed for reproducible world generation
    // regionSize: Size of the world region to fill with islands
    // minCellSize/maxCellSize: Voronoi cell size range (determines both spacing AND island size)
    //   - Cell size directly controls island spacing (cells are evenly distributed)
    //   - Island radius is proportional to cell size (larger cells = larger islands)
    //   - Typical values: 800-1200 for medium density, 1500-2000 for sparse, 400-600 for dense
    // islandToVoronoiRatio: Island radius as ratio of cell size (default 0.35 = 35% of cell size)
    std::vector<IslandDefinition> generateIslands(
        uint32_t worldSeed,
        float regionSize,
        float minCellSize,
        float maxCellSize,
        float islandToVoronoiRatio = 0.35f
    );
    
private:
    // Generate a Voronoi cell point
    Vec3 generateVoronoiPoint(int cellX, int cellY, int cellZ, uint32_t seed);
    
    // Calculate distance to nearest Voronoi point
    float getVoronoiDistance(const Vec3& position, uint32_t seed, Vec3& nearestPoint);
};
