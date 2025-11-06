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
        0.6f,                    // vegetationDensity
        0.3f                     // oreSpawnChance
    };
    
    // FOREST - Dense forested biome with moss and rich soil
    m_palettes[static_cast<int>(BiomeType::FOREST)] = {
        BlockID::MOSS,           // surfaceBlock (mossy forest floor)
        BlockID::DIRT,           // subsurfaceBlock
        BlockID::GRANITE,        // deepBlock (hard bedrock)
        BlockID::EMERALD_BLOCK,  // oreBlock (rare emeralds in ancient forests)
        0.95f,                   // vegetationDensity (very high - dense forest)
        0.2f                     // oreSpawnChance (less ore, more nature)
    };
    
    // DESERT - Sandy and dry with sandstone layers
    m_palettes[static_cast<int>(BiomeType::DESERT)] = {
        BlockID::SAND,           // surfaceBlock
        BlockID::SANDSTONE,      // subsurfaceBlock (compressed sand)
        BlockID::LIMESTONE,      // deepBlock
        BlockID::GOLD_BLOCK,     // oreBlock (desert gold deposits)
        0.1f,                    // vegetationDensity (sparse)
        0.4f                     // oreSpawnChance (gold in deserts)
    };
    
    // SNOW - Frozen tundra with packed ice
    m_palettes[static_cast<int>(BiomeType::SNOW)] = {
        BlockID::SNOW,           // surfaceBlock
        BlockID::PACKED_ICE,     // subsurfaceBlock (permafrost)
        BlockID::MARBLE,         // deepBlock (metamorphic rock)
        BlockID::SAPPHIRE_BLOCK, // oreBlock (icy blue gems)
        0.2f,                    // vegetationDensity
        0.35f                    // oreSpawnChance
    };
    
    // VOLCANIC - Dark basalt with glowing magma
    m_palettes[static_cast<int>(BiomeType::VOLCANIC)] = {
        BlockID::LAVA_ROCK,      // surfaceBlock (cooled lava)
        BlockID::BASALT,         // subsurfaceBlock (volcanic rock)
        BlockID::OBSIDIAN,       // deepBlock (volcanic glass)
        BlockID::RUBY_BLOCK,     // oreBlock (fire gems)
        0.05f,                   // vegetationDensity (almost none)
        0.7f                     // oreSpawnChance (very rich in minerals)
    };
    
    // CRYSTAL - Magical rare biome with crystal formations
    m_palettes[static_cast<int>(BiomeType::CRYSTAL)] = {
        BlockID::CRYSTAL_PURPLE, // surfaceBlock (shimmering crystals)
        BlockID::QUARTZ,         // subsurfaceBlock
        BlockID::AMETHYST,       // deepBlock
        BlockID::DIAMOND_BLOCK,  // oreBlock
        0.3f,                    // vegetationDensity
        0.9f                     // oreSpawnChance (extremely valuable)
    };
    
    // TROPICAL - Beach paradise with coral and sand
    m_palettes[static_cast<int>(BiomeType::TROPICAL)] = {
        BlockID::DIRT,           // surfaceBlock
        BlockID::SAND,           // subsurfaceBlock (sandy beach)
        BlockID::CORAL,          // deepBlock (coral reef base)
        BlockID::COPPER_BLOCK,   // oreBlock
        0.8f,                    // vegetationDensity (lush palm trees)
        0.25f                    // oreSpawnChance
    };
    
    // BARREN - Rocky wasteland with gravel
    m_palettes[static_cast<int>(BiomeType::BARREN)] = {
        BlockID::GRAVEL,         // surfaceBlock (loose rocks)
        BlockID::STONE,          // subsurfaceBlock
        BlockID::GRANITE,        // deepBlock (hard granite bedrock)
        BlockID::IRON_BLOCK,     // oreBlock
        0.0f,                    // vegetationDensity (none)
        0.5f                     // oreSpawnChance (decent ore)
    };
}

BiomeType BiomeSystem::getBiomeForPosition(const Vec3& worldPosition, uint32_t worldSeed) const
{
    // Use two layers of noise for interesting biome distribution
    FastNoiseLite biomeNoise;
    biomeNoise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    biomeNoise.SetSeed(worldSeed + 5000);  // Offset seed for biome layer
    biomeNoise.SetFrequency(0.001f);  // Very low frequency for large biome regions
    
    FastNoiseLite temperatureNoise;
    temperatureNoise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    temperatureNoise.SetSeed(worldSeed + 6000);
    temperatureNoise.SetFrequency(0.0015f);
    
    // Sample noise at island position (using X-Z plane, ignore Y)
    float biomeValue = biomeNoise.GetNoise(worldPosition.x, worldPosition.z);
    float temperature = temperatureNoise.GetNoise(worldPosition.x, worldPosition.z);
    
    // Use altitude (Y position) as a factor - higher islands are colder
    float altitudeFactor = worldPosition.y / 200.0f;  // -1.0 to 1.0 range
    temperature -= altitudeFactor * 0.5f;  // Higher = colder
    
    // Map noise values to biomes
    // Crystal biomes are extremely rare (only at specific noise intersections)
    if (std::abs(biomeValue) > 0.85f && std::abs(temperature) > 0.85f)
    {
        return BiomeType::CRYSTAL;  // Very rare
    }
    
    // Temperature-based biomes
    if (temperature < -0.5f)
    {
        return BiomeType::SNOW;  // Cold regions
    }
    else if (temperature > 0.6f)
    {
        // Hot regions - desert or volcanic
        return (biomeValue > 0.3f) ? BiomeType::VOLCANIC : BiomeType::DESERT;
    }
    else if (temperature > 0.2f && biomeValue < -0.2f)
    {
        return BiomeType::TROPICAL;  // Warm and humid
    }
    else if (biomeValue > 0.7f)
    {
        return BiomeType::BARREN;  // Rocky regions
    }
    else if (biomeValue < -0.4f && temperature > -0.2f && temperature < 0.4f)
    {
        return BiomeType::FOREST;  // Dense forested temperate regions
    }
    else
    {
        return BiomeType::GRASSLAND;  // Default temperate biome
    }
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
