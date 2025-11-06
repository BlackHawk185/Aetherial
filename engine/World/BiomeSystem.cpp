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
        BlockID::GRASS,          // surfaceBlock
        BlockID::DIRT,           // subsurfaceBlock
        BlockID::STONE,          // deepBlock
        BlockID::COAL,           // oreBlock
        0.6f,                    // vegetationDensity
        0.3f                     // oreSpawnChance
    };
    
    // FOREST - Dense forested biome
    m_palettes[static_cast<int>(BiomeType::FOREST)] = {
        BlockID::GRASS,          // surfaceBlock
        BlockID::DIRT,           // subsurfaceBlock
        BlockID::STONE,          // deepBlock
        BlockID::COAL,           // oreBlock
        0.95f,                   // vegetationDensity (very high - dense forest)
        0.25f                    // oreSpawnChance
    };
    
    // DESERT - Sandy and dry
    m_palettes[static_cast<int>(BiomeType::DESERT)] = {
        BlockID::SAND,           // surfaceBlock
        BlockID::SAND,           // subsurfaceBlock (more sand)
        BlockID::LIMESTONE,      // deepBlock
        BlockID::GOLD_BLOCK,     // oreBlock
        0.1f,                    // vegetationDensity (sparse)
        0.4f                     // oreSpawnChance (gold in deserts)
    };
    
    // SNOW - Frozen tundra
    m_palettes[static_cast<int>(BiomeType::SNOW)] = {
        BlockID::ICE,            // surfaceBlock
        BlockID::DIRT,           // subsurfaceBlock
        BlockID::STONE,          // deepBlock
        BlockID::IRON_BLOCK,     // oreBlock
        0.2f,                    // vegetationDensity
        0.35f                    // oreSpawnChance
    };
    
    // VOLCANIC - Dark and mineral-rich
    m_palettes[static_cast<int>(BiomeType::VOLCANIC)] = {
        BlockID::STONE,          // surfaceBlock (dark rock)
        BlockID::STONE,          // subsurfaceBlock
        BlockID::STONE,          // deepBlock
        BlockID::COAL,           // oreBlock (abundant coal)
        0.05f,                   // vegetationDensity (almost none)
        0.7f                     // oreSpawnChance (very rich)
    };
    
    // CRYSTAL - Rare and valuable
    m_palettes[static_cast<int>(BiomeType::CRYSTAL)] = {
        BlockID::DIAMOND_BLOCK,  // surfaceBlock (shimmering)
        BlockID::STONE,          // subsurfaceBlock
        BlockID::STONE,          // deepBlock
        BlockID::DIAMOND_BLOCK,  // oreBlock
        0.3f,                    // vegetationDensity
        0.9f                     // oreSpawnChance (very valuable)
    };
    
    // TROPICAL - Beach paradise
    m_palettes[static_cast<int>(BiomeType::TROPICAL)] = {
        BlockID::GRASS,          // surfaceBlock
        BlockID::SAND,           // subsurfaceBlock (sandy underneath)
        BlockID::LIMESTONE,      // deepBlock
        BlockID::COPPER_BLOCK,   // oreBlock
        0.8f,                    // vegetationDensity (lush)
        0.25f                    // oreSpawnChance
    };
    
    // BARREN - Rocky wasteland
    m_palettes[static_cast<int>(BiomeType::BARREN)] = {
        BlockID::STONE,          // surfaceBlock
        BlockID::STONE,          // subsurfaceBlock
        BlockID::STONE,          // deepBlock
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
