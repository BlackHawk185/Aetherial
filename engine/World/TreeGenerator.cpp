// TreeGenerator.cpp - Implementation of procedural tree generation
#include "TreeGenerator.h"
#include "IslandChunkSystem.h"
#include "BlockType.h"
#include <cstdlib>
#include <cmath>

void TreeGenerator::generateTree(IslandChunkSystem* chunkSystem, uint32_t islandID,
                                 const Vec3& basePos, uint32_t seed, float biomeVegetationDensity)
{
    if (!chunkSystem) return;
    
    // Select tree type based on seed and biome density
    // High density biomes (forests) get more variety
    int treeTypeRoll = seed % 100;
    
    if (biomeVegetationDensity > 0.8f) {
        // Dense forest - mix of oak, pine, and jungle trees
        if (treeTypeRoll < 40) generateOakTree(chunkSystem, islandID, basePos, seed);
        else if (treeTypeRoll < 70) generatePineTree(chunkSystem, islandID, basePos, seed);
        else generateJungleTree(chunkSystem, islandID, basePos, seed);
    }
    else if (biomeVegetationDensity > 0.5f) {
        // Moderate - oak and birch
        if (treeTypeRoll < 60) generateOakTree(chunkSystem, islandID, basePos, seed);
        else generateBirchTree(chunkSystem, islandID, basePos, seed);
    }
    else if (biomeVegetationDensity > 0.15f) {
        // Sparse - mix with dead trees
        if (treeTypeRoll < 70) generateOakTree(chunkSystem, islandID, basePos, seed);
        else generateDeadTree(chunkSystem, islandID, basePos, seed);
    }
    else {
        // Very sparse - mostly dead or palm
        if (treeTypeRoll < 50) generateDeadTree(chunkSystem, islandID, basePos, seed);
        else generatePalmTree(chunkSystem, islandID, basePos, seed);
    }
}

void TreeGenerator::generateOakTree(IslandChunkSystem* chunkSystem, uint32_t islandID,
                                    const Vec3& basePos, uint32_t seed)
{
    // Massive oak: 12-18 blocks tall with wide, layered canopy
    int trunkHeight = 12 + (seed % 7);
    
    // Build thick trunk (2x2 for larger trees)
    bool isLarge = trunkHeight > 14;
    if (isLarge) {
        for (int y = 0; y < trunkHeight; ++y) {
            chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(0, y, 0), BlockID::WOOD_OAK);
            chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(1, y, 0), BlockID::WOOD_OAK);
            chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(0, y, 1), BlockID::WOOD_OAK);
            chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(1, y, 1), BlockID::WOOD_OAK);
        }
    } else {
        for (int y = 0; y < trunkHeight; ++y) {
            chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(0, y, 0), BlockID::WOOD_OAK);
        }
    }
    
    // Multi-layered wide canopy
    Vec3 canopyBase = basePos + Vec3(0, trunkHeight - 4, 0);
    placeSphere(chunkSystem, islandID, canopyBase, 5, BlockID::LEAVES_GREEN);
    placeSphere(chunkSystem, islandID, canopyBase + Vec3(0, 2, 0), 4, BlockID::LEAVES_GREEN);
    placeSphere(chunkSystem, islandID, canopyBase + Vec3(0, 4, 0), 3, BlockID::LEAVES_GREEN);
    
    // Add some branch extensions
    for (int i = 0; i < 4; ++i) {
        int xOff = (i % 2 == 0) ? 3 : -3;
        int zOff = (i / 2 == 0) ? 3 : -3;
        Vec3 branchPos = canopyBase + Vec3(xOff, 1, zOff);
        chunkSystem->setBlockIDWithAutoChunk(islandID, branchPos, BlockID::WOOD_OAK);
        placeSphere(chunkSystem, islandID, branchPos, 2, BlockID::LEAVES_GREEN);
    }
}

void TreeGenerator::generatePineTree(IslandChunkSystem* chunkSystem, uint32_t islandID,
                                     const Vec3& basePos, uint32_t seed)
{
    // Towering pine: 18-28 blocks tall with massive conical shape
    int trunkHeight = 18 + (seed % 11);
    
    // Build trunk
    for (int y = 0; y < trunkHeight; ++y) {
        chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(0, y, 0), BlockID::WOOD_PINE);
    }
    
    // Conical canopy - decreasing radius as we go up, many layers
    int canopyStart = trunkHeight / 2;
    int numLayers = trunkHeight - canopyStart + 2;
    
    for (int layer = 0; layer < numLayers; ++layer) {
        int y = canopyStart + layer;
        // Radius decreases from 5 at base to 1 at top
        float normalizedHeight = (float)layer / numLayers;
        int radius = 5 - (int)(normalizedHeight * 4);
        
        if (radius > 0) {
            placeSphere(chunkSystem, islandID, basePos + Vec3(0, y, 0), radius, BlockID::LEAVES_DARK);
        }
    }
    
    // Top point
    chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(0, trunkHeight, 0), BlockID::LEAVES_DARK);
    chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(0, trunkHeight + 1, 0), BlockID::LEAVES_DARK);
}

void TreeGenerator::generateJungleTree(IslandChunkSystem* chunkSystem, uint32_t islandID,
                                       const Vec3& basePos, uint32_t seed)
{
    // Colossal jungle tree: 20-32 blocks tall with 3x3 trunk
    int trunkHeight = 20 + (seed % 13);
    
    // Build 3x3 thick trunk for ultimate presence
    for (int y = 0; y < trunkHeight; ++y) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dz = -1; dz <= 1; ++dz) {
                chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(dx, y, dz), BlockID::WOOD_JUNGLE);
            }
        }
    }
    
    // Massive multi-layer canopy with spreading branches
    Vec3 canopyBase = basePos + Vec3(0, trunkHeight - 8, 0);
    
    // Bottom canopy layer - huge
    placeSphere(chunkSystem, islandID, canopyBase, 7, BlockID::LEAVES_DARK);
    
    // Middle layers
    placeSphere(chunkSystem, islandID, canopyBase + Vec3(0, 3, 0), 6, BlockID::LEAVES_DARK);
    placeSphere(chunkSystem, islandID, canopyBase + Vec3(0, 5, 0), 5, BlockID::LEAVES_DARK);
    
    // Top layers
    placeSphere(chunkSystem, islandID, canopyBase + Vec3(0, 7, 0), 4, BlockID::LEAVES_DARK);
    placeSphere(chunkSystem, islandID, canopyBase + Vec3(0, 9, 0), 3, BlockID::LEAVES_DARK);
    
    // Add branch extensions in cardinal directions
    for (int dir = 0; dir < 4; ++dir) {
        int xOff = (dir == 0) ? 5 : (dir == 1) ? -5 : 0;
        int zOff = (dir == 2) ? 5 : (dir == 3) ? -5 : 0;
        
        Vec3 branchPos = canopyBase + Vec3(xOff, 2, zOff);
        // Branch trunk
        for (int i = 0; i < 3; ++i) {
            chunkSystem->setBlockIDWithAutoChunk(islandID, branchPos + Vec3(-i * xOff/5, 0, -i * zOff/5), BlockID::WOOD_JUNGLE);
        }
        placeSphere(chunkSystem, islandID, branchPos, 3, BlockID::LEAVES_DARK);
    }
}

void TreeGenerator::generateBirchTree(IslandChunkSystem* chunkSystem, uint32_t islandID,
                                      const Vec3& basePos, uint32_t seed)
{
    // Tall and elegant: 14-20 blocks tall, narrow but full canopy
    int trunkHeight = 14 + (seed % 7);
    
    // Build trunk
    for (int y = 0; y < trunkHeight; ++y) {
        chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(0, y, 0), BlockID::WOOD_BIRCH);
    }
    
    // Elongated canopy - narrow but tall
    Vec3 canopyBase = basePos + Vec3(0, trunkHeight - 5, 0);
    for (int layer = 0; layer < 6; ++layer) {
        int radius = (layer < 2 || layer > 4) ? 2 : 3;
        placeSphere(chunkSystem, islandID, canopyBase + Vec3(0, layer, 0), radius, BlockID::LEAVES_GREEN);
    }
}

void TreeGenerator::generateWillowTree(IslandChunkSystem* chunkSystem, uint32_t islandID,
                                       const Vec3& basePos, uint32_t seed)
{
    // Majestic weeping willow: 12-16 blocks tall with dramatic hanging branches
    int trunkHeight = 12 + (seed % 5);
    
    // Build trunk (2x2 for stability)
    for (int y = 0; y < trunkHeight; ++y) {
        chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(0, y, 0), BlockID::WOOD_OAK);
        if (y < trunkHeight - 3) {
            chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(1, y, 0), BlockID::WOOD_OAK);
            chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(0, y, 1), BlockID::WOOD_OAK);
            chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(1, y, 1), BlockID::WOOD_OAK);
        }
    }
    
    // Wide flat canopy
    Vec3 canopyBase = basePos + Vec3(0, trunkHeight - 4, 0);
    placeSphere(chunkSystem, islandID, canopyBase, 5, BlockID::LEAVES_GREEN);
    
    // Dramatic drooping branches - vertical columns of leaves at various distances
    for (int ring = 2; ring <= 5; ring += 1) {
        for (int angle = 0; angle < 8; ++angle) {
            int xOff = (int)(ring * cos(angle * 3.14159f / 4));
            int zOff = (int)(ring * sin(angle * 3.14159f / 4));
            
            int dropLength = 6 + (seed % 4);
            for (int dy = 0; dy < dropLength; ++dy) {
                chunkSystem->setBlockIDWithAutoChunk(islandID, canopyBase + Vec3(xOff, -dy, zOff), BlockID::LEAVES_GREEN);
            }
        }
    }
}

void TreeGenerator::generateDeadTree(IslandChunkSystem* chunkSystem, uint32_t islandID,
                                     const Vec3& basePos, uint32_t seed)
{
    // Dead/barren tree: 4-7 blocks tall, no leaves, twisted branches
    int trunkHeight = 4 + (seed % 4);
    
    // Build trunk
    for (int y = 0; y < trunkHeight; ++y) {
        chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(0, y, 0), BlockID::WOOD_OAK);
    }
    
    // Add bare branches at top
    Vec3 top = basePos + Vec3(0, trunkHeight - 1, 0);
    chunkSystem->setBlockIDWithAutoChunk(islandID, top + Vec3(1, 0, 0), BlockID::WOOD_OAK);
    chunkSystem->setBlockIDWithAutoChunk(islandID, top + Vec3(-1, 0, 0), BlockID::WOOD_OAK);
    chunkSystem->setBlockIDWithAutoChunk(islandID, top + Vec3(0, 1, 0), BlockID::WOOD_OAK);
    
    if (seed % 2 == 0) {
        chunkSystem->setBlockIDWithAutoChunk(islandID, top + Vec3(1, 1, 0), BlockID::WOOD_OAK);
        chunkSystem->setBlockIDWithAutoChunk(islandID, top + Vec3(-1, 1, 0), BlockID::WOOD_OAK);
    }
}

void TreeGenerator::generatePalmTree(IslandChunkSystem* chunkSystem, uint32_t islandID,
                                     const Vec3& basePos, uint32_t seed)
{
    // Towering tropical palm: 12-18 blocks tall with dramatic curved trunk
    int trunkHeight = 12 + (seed % 7);
    
    // Build curved trunk with lean
    int leanDir = seed % 4;
    for (int y = 0; y < trunkHeight; ++y) {
        float leanAmount = (float)y / trunkHeight;
        int xOffset = 0;
        int zOffset = 0;
        
        // Progressive lean
        if (y > 4) {
            int lean = (int)(leanAmount * 3);
            switch(leanDir) {
                case 0: xOffset = lean; break;
                case 1: xOffset = -lean; break;
                case 2: zOffset = lean; break;
                case 3: zOffset = -lean; break;
            }
        }
        
        chunkSystem->setBlockIDWithAutoChunk(islandID, basePos + Vec3(xOffset, y, zOffset), BlockID::WOOD_PALM);
    }
    
    // Palm fronds - large radiating pattern from top
    Vec3 top = basePos + Vec3(0, trunkHeight, 0);
    
    // 8 directions of long fronds
    for (int angle = 0; angle < 8; ++angle) {
        int xDir = (int)(cos(angle * 3.14159f / 4) * 6);
        int zDir = (int)(sin(angle * 3.14159f / 4) * 6);
        
        // Create long frond extending outward
        for (int i = 1; i <= 6; ++i) {
            float dropAmount = (float)(i * i) / 36.0f; // Parabolic droop
            int yOffset = -(int)(dropAmount * 3);
            
            Vec3 frondPos = top + Vec3(xDir * i / 6, yOffset, zDir * i / 6);
            chunkSystem->setBlockIDWithAutoChunk(islandID, frondPos, BlockID::LEAVES_PALM);
        }
    }
    
    // Center coconut cluster
    placeSphere(chunkSystem, islandID, top, 2, BlockID::LEAVES_PALM);
}

void TreeGenerator::placeSphere(IslandChunkSystem* chunkSystem, uint32_t islandID,
                                const Vec3& center, int radius, uint8_t blockType)
{
    for (int dx = -radius; dx <= radius; ++dx) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dz = -radius; dz <= radius; ++dz) {
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (dist <= radius && !(dx == 0 && dy == 0 && dz == 0)) {
                    chunkSystem->setBlockIDWithAutoChunk(islandID, center + Vec3(dx, dy, dz), blockType);
                }
            }
        }
    }
}
