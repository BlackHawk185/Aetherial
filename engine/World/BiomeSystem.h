// BiomeSystem.h - Biome classification and block palette system
#pragma once

#include "../Math/Vec3.h"
#include <cstdint>

/**
 * Biome types that determine island appearance and block composition
 */
enum class BiomeType : uint8_t {
    GRASSLAND,      // Lush green islands with grass and dirt
    FOREST,         // Dense forested islands with high tree density
    DESERT,         // Sandy islands with sandstone
    SNOW,           // Icy islands with snow and ice
    VOLCANIC,       // Dark stone islands with coal/lava
    CRYSTAL,        // Rare islands with diamonds and precious blocks
    TROPICAL,       // Islands with sand beaches and limestone
    BARREN          // Rocky islands with minimal vegetation
};

/**
 * Block palette for a biome - defines what blocks appear at different layers
 */
struct BiomePalette {
    uint8_t surfaceBlock;       // Top layer block (grass, sand, snow, etc.)
    uint8_t subsurfaceBlock;    // Layer just below surface (dirt, sandstone, etc.)
    uint8_t deepBlock;          // Deep interior block (stone variants)
    uint8_t oreBlock;           // Ore type that spawns in this biome
    float vegetationDensity;    // 0.0-1.0, how many trees/decorations spawn
    float oreSpawnChance;       // 0.0-1.0, likelihood of ore veins
    uint8_t waterBlock;         // Water block type for this biome
    int minWaterDepth;          // Minimum depth for water features
    int maxWaterDepth;          // Maximum depth for water features
};

/**
 * BiomeSystem determines biome type based on world position using noise
 */
class BiomeSystem {
public:
    BiomeSystem();
    
    /**
     * Determine biome type for an island based on its world position
     * Uses noise for natural biome distribution
     */
    BiomeType getBiomeForPosition(const Vec3& worldPosition, uint32_t worldSeed) const;
    
    /**
     * Get the block palette for a specific biome
     */
    BiomePalette getPalette(BiomeType biome) const;
    
    /**
     * Get biome name for debugging/UI
     */
    const char* getBiomeName(BiomeType biome) const;
    
private:
    // Biome configuration
    BiomePalette m_palettes[8];  // One for each BiomeType
    
    void initializePalettes();
};
