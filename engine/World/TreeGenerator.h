// TreeGenerator.h - Procedural tree generation for biomes
#pragma once

#include "../Math/Vec3.h"
#include <cstdint>

class VoxelChunk;
class IslandChunkSystem;

enum class TreeType {
    OAK,          // Classic rounded canopy
    PINE,         // Tall conical evergreen
    WILLOW,       // Drooping branches
    BIRCH,        // Tall and slender
    JUNGLE,       // Massive with vines
    DEAD,         // Barren dead tree
    PALM          // Tropical palm tree
};

/**
 * Generates procedural trees made of voxel blocks
 * Trees are composed of wood trunk and leaf blocks
 */
class TreeGenerator {
public:
    /**
     * Generate a tree appropriate for the biome
     * Automatically selects tree type based on seed
     */
    static void generateTree(IslandChunkSystem* chunkSystem, uint32_t islandID, 
                            const Vec3& basePos, uint32_t seed, float biomeVegetationDensity);
    
private:
    static void generateOakTree(IslandChunkSystem* chunkSystem, uint32_t islandID, const Vec3& basePos, uint32_t seed);
    static void generatePineTree(IslandChunkSystem* chunkSystem, uint32_t islandID, const Vec3& basePos, uint32_t seed);
    static void generateWillowTree(IslandChunkSystem* chunkSystem, uint32_t islandID, const Vec3& basePos, uint32_t seed);
    static void generateBirchTree(IslandChunkSystem* chunkSystem, uint32_t islandID, const Vec3& basePos, uint32_t seed);
    static void generateJungleTree(IslandChunkSystem* chunkSystem, uint32_t islandID, const Vec3& basePos, uint32_t seed);
    static void generateDeadTree(IslandChunkSystem* chunkSystem, uint32_t islandID, const Vec3& basePos, uint32_t seed);
    static void generatePalmTree(IslandChunkSystem* chunkSystem, uint32_t islandID, const Vec3& basePos, uint32_t seed);
    
    // Helper to place a sphere of leaves
    static void placeSphere(IslandChunkSystem* chunkSystem, uint32_t islandID, const Vec3& center, int radius, uint8_t blockType);
};
