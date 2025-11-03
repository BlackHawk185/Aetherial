// VoronoiIslandPlacer.cpp - Implementation of Voronoi-based island placement
#include "VoronoiIslandPlacer.h"
#include "../../libs/FastNoiseLite/FastNoiseLite.h"
#include <cmath>
#include <algorithm>

VoronoiIslandPlacer::VoronoiIslandPlacer() {
    // Constructor
}

std::vector<IslandDefinition> VoronoiIslandPlacer::generateIslands(
    uint32_t worldSeed,
    float regionSize,
    float islandDensity,
    float minRadius,
    float maxRadius)
{
    // Calculate target island count based on density and region size
    // islandDensity = islands per 1000x1000 area
    float areaMultiplier = (regionSize * regionSize) / (1000.0f * 1000.0f);
    int targetIslandCount = static_cast<int>(islandDensity * areaMultiplier);
    
    std::vector<IslandDefinition> islands;
    islands.reserve(targetIslandCount);
    
    // Calculate cell size for Voronoi distribution
    // We want roughly 'targetIslandCount' cells in the region
    float cellSize = regionSize / std::sqrt(static_cast<float>(targetIslandCount));
    
    // Use FastNoiseLite's Cellular (Voronoi) noise for island placement
    FastNoiseLite cellularNoise;
    cellularNoise.SetNoiseType(FastNoiseLite::NoiseType_Cellular);
    cellularNoise.SetSeed(worldSeed);
    cellularNoise.SetFrequency(1.0f / cellSize);  // Frequency based on desired cell size
    cellularNoise.SetCellularDistanceFunction(FastNoiseLite::CellularDistanceFunction_Euclidean);
    cellularNoise.SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance);
    
    // Second cellular noise to get distance to second-nearest (for better spacing)
    FastNoiseLite cellularNoise2;
    cellularNoise2.SetNoiseType(FastNoiseLite::NoiseType_Cellular);
    cellularNoise2.SetSeed(worldSeed);
    cellularNoise2.SetFrequency(1.0f / cellSize);
    cellularNoise2.SetCellularDistanceFunction(FastNoiseLite::CellularDistanceFunction_Euclidean);
    cellularNoise2.SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance2);
    
    // Noise for vertical variation
    FastNoiseLite heightNoise;
    heightNoise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    heightNoise.SetSeed(worldSeed + 2000);
    heightNoise.SetFrequency(heightNoiseFrequency);
    
    // Sample the region in a grid to find Voronoi cell centers
    int samplesPerAxis = static_cast<int>(std::sqrt(targetIslandCount * 4));  // Oversample
    float stepSize = regionSize / samplesPerAxis;
    
    std::vector<std::pair<Vec3, float>> candidateIslands;  // Position + cell size
    candidateIslands.reserve(samplesPerAxis * samplesPerAxis);
    
    // Sample in 2D (X-Z plane) for island positions
    for (int x = 0; x < samplesPerAxis; ++x) {
        for (int z = 0; z < samplesPerAxis; ++z) {
            float worldX = (x - samplesPerAxis / 2.0f) * stepSize;
            float worldZ = (z - samplesPerAxis / 2.0f) * stepSize;
            
            // Get cellular distance - this tells us how close we are to a cell center
            float distance1 = cellularNoise.GetNoise(worldX, worldZ);
            float distance2 = cellularNoise2.GetNoise(worldX, worldZ);
            
            // Cell centers have minimum distance value (close to 0)
            // We want local minima of distance, not maxima
            bool isLocalMin = true;
            const float checkRadius = stepSize * 0.5f;
            
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dz = -1; dz <= 1; ++dz) {
                    if (dx == 0 && dz == 0) continue;
                    
                    float neighborDist = cellularNoise.GetNoise(
                        worldX + dx * checkRadius,
                        worldZ + dz * checkRadius
                    );
                    
                    if (neighborDist < distance1) {
                        isLocalMin = false;
                        break;
                    }
                }
                if (!isLocalMin) break;
            }
            
            if (isLocalMin && distance1 < cellCenterThreshold) {  // Use configurable threshold
                // This is a Voronoi cell center - create an island here
                
                // Add vertical variation
                float heightVariation = heightNoise.GetNoise(worldX, worldZ);
                float worldY = heightVariation * verticalSpreadMultiplier;  // Use configurable spread
                
                // Calculate cell size: distance between nearest and second-nearest point
                // This represents how much "space" this cell has
                float cellSize = (distance2 - distance1) * regionSize;  // Convert to world units
                
                candidateIslands.push_back({Vec3(worldX, worldY, worldZ), cellSize});
            }
        }
    }
    
    // Sort candidates by distance from center to prioritize central islands
    std::sort(candidateIslands.begin(), candidateIslands.end(),
        [](const auto& a, const auto& b) {
            float distA = a.first.x * a.first.x + a.first.z * a.first.z;
            float distB = b.first.x * b.first.x + b.first.z * b.first.z;
            return distA < distB;
        }
    );
    
    // Take the closest N islands to the center
    int numIslands = std::min(targetIslandCount, static_cast<int>(candidateIslands.size()));
    
    // Create noise generator for size variation
    FastNoiseLite sizeNoise;
    sizeNoise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    sizeNoise.SetSeed(worldSeed + 1000);
    sizeNoise.SetFrequency(0.003f);  // Low frequency for smooth size variation
    
    for (int i = 0; i < numIslands; ++i) {
        Vec3 pos = candidateIslands[i].first;
        float cellSize = candidateIslands[i].second;
        
        // Map cell size to island radius with added noise variation
        // Larger cells = larger islands (they have more space)
        // Normalize cell size to [0, 1] range (approximate)
        float normalizedSize = std::min(1.0f, cellSize / (regionSize * 0.5f));
        
        // Add Perlin noise for additional size variation (range: -1 to 1)
        float sizeVariation = sizeNoise.GetNoise(pos.x, pos.z);
        
        // Combine cell size (60% weight) with noise variation (40% weight)
        // This gives us both spatial coherence AND interesting variation
        float combinedSize = (normalizedSize * 0.6f) + ((sizeVariation * 0.5f + 0.5f) * 0.4f);
        combinedSize = std::max(0.0f, std::min(1.0f, combinedSize));  // Clamp to [0, 1]
        
        // Map to radius range
        float radius = minRadius + combinedSize * (maxRadius - minRadius);
        
        // Generate unique seed for this island based on position
        uint32_t islandSeed = worldSeed;
        islandSeed ^= static_cast<uint32_t>(pos.x * 374761393.0f);
        islandSeed ^= static_cast<uint32_t>(pos.y * 668265263.0f);
        islandSeed ^= static_cast<uint32_t>(pos.z * 1274126177.0f);
        
        islands.push_back({pos, radius, islandSeed});
    }
    
    return islands;
}

Vec3 VoronoiIslandPlacer::generateVoronoiPoint(int cellX, int cellY, int cellZ, uint32_t seed) {
    // Generate a pseudo-random point within a Voronoi cell
    uint32_t h = seed;
    h ^= static_cast<uint32_t>(cellX) * 374761393u;
    h ^= static_cast<uint32_t>(cellY) * 668265263u;
    h ^= static_cast<uint32_t>(cellZ) * 1274126177u;
    h ^= h >> 13;
    h *= 1103515245u;
    h ^= h >> 16;
    
    float rx = (h & 0xFFFF) / 65535.0f;
    h = h * 1103515245u + 12345u;
    float ry = (h & 0xFFFF) / 65535.0f;
    h = h * 1103515245u + 12345u;
    float rz = (h & 0xFFFF) / 65535.0f;
    
    return Vec3(
        cellX + rx,
        cellY + ry,
        cellZ + rz
    );
}

float VoronoiIslandPlacer::getVoronoiDistance(const Vec3& position, uint32_t seed, Vec3& nearestPoint) {
    int cellX = static_cast<int>(std::floor(position.x));
    int cellY = static_cast<int>(std::floor(position.y));
    int cellZ = static_cast<int>(std::floor(position.z));
    
    float minDistance = 1e10f;
    
    // Check neighboring cells
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                Vec3 point = generateVoronoiPoint(cellX + dx, cellY + dy, cellZ + dz, seed);
                float dist = (point - position).length();
                
                if (dist < minDistance) {
                    minDistance = dist;
                    nearestPoint = point;
                }
            }
        }
    }
    
    return minDistance;
}
