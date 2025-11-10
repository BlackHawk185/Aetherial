// BiomeSystem.cpp - Implementation of biome system
#include "BiomeSystem.h"
#include "BlockType.h"
#include "../../libs/FastNoiseLite/FastNoiseLite.h"
#include <cmath>

BiomeSystem::BiomeSystem()
{
    initializePalettes();
}

void BiomeSystem::initializePalettes()
{
    // GRASSLAND - Default lush biome
    m_palettes[static_cast<int>(BiomeType::GRASSLAND)] = {
        BlockID::DIRT,           // surfaceBlock
        BlockID::DIRT,           // subsurfaceBlock
        BlockID::STONE,          // deepBlock
        BlockID::COAL,           // oreBlock
        0.08f,                   // vegetationDensity (ultra-sparse trees - true open grassland)
        0.3f,                    // oreSpawnChance
        BlockID::WATER,          // waterBlock
        2,                       // minWaterDepth
        6                        // maxWaterDepth
    };
    
    // FOREST - Dense forested biome with moss and rich soil
    m_palettes[static_cast<int>(BiomeType::FOREST)] = {
        BlockID::MOSS,           // surfaceBlock (mossy forest floor)
        BlockID::DIRT,           // subsurfaceBlock
        BlockID::GRANITE,        // deepBlock (hard bedrock)
        BlockID::EMERALD_BLOCK,  // oreBlock (rare emeralds in ancient forests)
        0.95f,                   // vegetationDensity (very high - dense forest)
        0.2f,                    // oreSpawnChance (less ore, more nature)
        BlockID::WATER,          // waterBlock
        3,                       // minWaterDepth
        8                        // maxWaterDepth
    };
    
    // DESERT - Sandy and dry with sandstone layers
    m_palettes[static_cast<int>(BiomeType::DESERT)] = {
        BlockID::SAND,           // surfaceBlock
        BlockID::SANDSTONE,      // subsurfaceBlock (compressed sand)
        BlockID::LIMESTONE,      // deepBlock
        BlockID::GOLD_BLOCK,     // oreBlock (desert gold deposits)
        0.1f,                    // vegetationDensity (sparse)
        0.4f,                    // oreSpawnChance (gold in deserts)
        BlockID::WATER,          // waterBlock
        1,                       // minWaterDepth (oases)
        3                        // maxWaterDepth (small pools)
    };
    
    // SNOW - Frozen tundra with packed ice
    m_palettes[static_cast<int>(BiomeType::SNOW)] = {
        BlockID::SNOW,           // surfaceBlock
        BlockID::PACKED_ICE,     // subsurfaceBlock (permafrost)
        BlockID::MARBLE,         // deepBlock (metamorphic rock)
        BlockID::SAPPHIRE_BLOCK, // oreBlock (icy blue gems)
        0.2f,                    // vegetationDensity
        0.35f,                   // oreSpawnChance
        BlockID::ICE,            // waterBlock (frozen water)
        2,                       // minWaterDepth
        5                        // maxWaterDepth
    };
    
    // VOLCANIC - Dark basalt with glowing magma
    m_palettes[static_cast<int>(BiomeType::VOLCANIC)] = {
        BlockID::LAVA_ROCK,      // surfaceBlock (cooled lava)
        BlockID::BASALT,         // subsurfaceBlock (volcanic rock)
        BlockID::OBSIDIAN,       // deepBlock (volcanic glass)
        BlockID::RUBY_BLOCK,     // oreBlock (fire gems)
        0.05f,                   // vegetationDensity (almost none)
        0.7f,                    // oreSpawnChance (very rich in minerals)
        BlockID::LAVA,           // waterBlock (lava pools)
        1,                       // minWaterDepth
        4                        // maxWaterDepth
    };
    
    // CRYSTAL - Magical rare biome with crystal formations
    m_palettes[static_cast<int>(BiomeType::CRYSTAL)] = {
        BlockID::CRYSTAL_PURPLE, // surfaceBlock (shimmering crystals)
        BlockID::QUARTZ,         // subsurfaceBlock
        BlockID::AMETHYST,       // deepBlock
        BlockID::DIAMOND_BLOCK,  // oreBlock
        0.3f,                    // vegetationDensity
        0.9f,                    // oreSpawnChance (extremely valuable)
        BlockID::WATER,          // waterBlock (crystal-clear water)
        3,                       // minWaterDepth
        10                       // maxWaterDepth (deep pools)
    };
    
    // TROPICAL - Beach paradise with coral and sand
    m_palettes[static_cast<int>(BiomeType::TROPICAL)] = {
        BlockID::DIRT,           // surfaceBlock
        BlockID::SAND,           // subsurfaceBlock (sandy beach)
        BlockID::CORAL,          // deepBlock (coral reef base)
        BlockID::COPPER_BLOCK,   // oreBlock
        0.8f,                    // vegetationDensity (lush palm trees)
        0.25f,                   // oreSpawnChance
        BlockID::WATER,          // waterBlock
        3,                       // minWaterDepth
        12                       // maxWaterDepth (lagoons)
    };
    
    // BARREN - Rocky wasteland with gravel
    m_palettes[static_cast<int>(BiomeType::BARREN)] = {
        BlockID::GRAVEL,         // surfaceBlock (loose rocks)
        BlockID::STONE,          // subsurfaceBlock
        BlockID::GRANITE,        // deepBlock (hard granite bedrock)
        BlockID::IRON_BLOCK,     // oreBlock
        0.0f,                    // vegetationDensity (none)
        0.5f,                    // oreSpawnChance (decent ore)
        BlockID::WATER,          // waterBlock
        1,                       // minWaterDepth
        3                        // maxWaterDepth (shallow puddles)
    };
}

BiomeType BiomeSystem::getBiomeForPosition(const Vec3& worldPosition, uint32_t worldSeed) const
{
    // Generate pseudo-random biome based on position and seed
    uint32_t hash = worldSeed;
    hash ^= static_cast<uint32_t>(worldPosition.x * 374761393.0f);
    hash ^= static_cast<uint32_t>(worldPosition.z * 668265263.0f);
    hash ^= hash >> 13;
    hash *= 1103515245u;
    hash ^= hash >> 16;
    
    float randValue = (hash & 0xFFFF) / 65535.0f;  // 0.0 to 1.0
    
    // Weighted random biome distribution
    if (randValue < 0.05f) return BiomeType::CRYSTAL;      // 5%
    if (randValue < 0.15f) return BiomeType::VOLCANIC;     // 10%
    if (randValue < 0.25f) return BiomeType::SNOW;         // 10%
    if (randValue < 0.35f) return BiomeType::DESERT;       // 10%
    if (randValue < 0.50f) return BiomeType::TROPICAL;     // 15%
    if (randValue < 0.60f) return BiomeType::BARREN;       // 10%
    if (randValue < 0.80f) return BiomeType::FOREST;       // 20%
    return BiomeType::GRASSLAND;                            // 20%
}

BiomePalette BiomeSystem::getPalette(BiomeType biome) const
{
    int index = static_cast<int>(biome);
    if (index < 0 || index >= 8)
    {
        return m_palettes[0];  // Fallback to grassland
    }
    return m_palettes[index];
}

const char* BiomeSystem::getBiomeName(BiomeType biome) const
{
    switch (biome)
    {
        case BiomeType::GRASSLAND: return "Grassland";
        case BiomeType::FOREST: return "Forest";
        case BiomeType::DESERT: return "Desert";
        case BiomeType::SNOW: return "Snow";
        case BiomeType::VOLCANIC: return "Volcanic";
        case BiomeType::CRYSTAL: return "Crystal";
        case BiomeType::TROPICAL: return "Tropical";
        case BiomeType::BARREN: return "Barren";
        default: return "Unknown";
    }
}
