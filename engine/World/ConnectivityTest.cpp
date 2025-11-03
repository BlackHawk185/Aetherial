// ConnectivityTest.cpp - Test implementations
#include "ConnectivityTest.h"
#include "ConnectivityAnalyzer.h"
#include "IslandChunkSystem.h"
#include "BlockType.h"
#include <iostream>

namespace ConnectivityTest {

void testIslandConnectivity(uint32_t islandID) {
    std::cout << "\n====== CONNECTIVITY TEST ======" << std::endl;
    
    FloatingIsland* island = g_islandSystem.getIsland(islandID);
    if (!island) {
        std::cout << "❌ Island " << islandID << " not found!" << std::endl;
        return;
    }
    
    std::cout << "🔍 Analyzing Island " << islandID << "..." << std::endl;
    
    // Count total voxels
    size_t totalVoxels = 0;
    for (const auto& [chunkCoord, chunk] : island->chunks) {
        if (!chunk) continue;
        for (int x = 0; x < VoxelChunk::SIZE; x++) {
            for (int y = 0; y < VoxelChunk::SIZE; y++) {
                for (int z = 0; z < VoxelChunk::SIZE; z++) {
                    if (chunk->getVoxel(x, y, z) != 0) {
                        totalVoxels++;
                    }
                }
            }
        }
    }
    
    std::cout << "   Chunks: " << island->chunks.size() << std::endl;
    std::cout << "   Total Solid Voxels: " << totalVoxels << std::endl;
    
    // Analyze connectivity
    auto groups = ConnectivityAnalyzer::analyzeIsland(island);
    
    std::cout << "   Connected Groups: " << groups.size() << std::endl;
    
    if (groups.size() == 1) {
        std::cout << "✅ Island is fully connected (1 group)" << std::endl;
    } else {
        std::cout << "⚠️ Island has " << groups.size() << " separate groups:" << std::endl;
        for (size_t i = 0; i < groups.size(); i++) {
            float percentage = (groups[i].voxelCount * 100.0f) / totalVoxels;
            std::cout << "   Group " << (i+1) << ": " << groups[i].voxelCount 
                      << " voxels (" << static_cast<int>(percentage) << "%)" << std::endl;
            std::cout << "      Center: (" << groups[i].centerOfMass.x 
                      << ", " << groups[i].centerOfMass.y 
                      << ", " << groups[i].centerOfMass.z << ")" << std::endl;
        }
    }
    
    std::cout << "============================\n" << std::endl;
}

uint32_t createDisconnectedTestIsland() {
    std::cout << "\n====== CREATING TEST ISLAND ======" << std::endl;
    std::cout << "Creating island with intentionally disconnected groups..." << std::endl;
    
    // Create island at origin
    uint32_t islandID = g_islandSystem.createIsland(Vec3(0, 0, 0));
    
    // Create main cube (10x10x10 at origin)
    std::cout << "Building main cube..." << std::endl;
    for (int x = -5; x < 5; x++) {
        for (int y = -5; y < 5; y++) {
            for (int z = -5; z < 5; z++) {
                g_islandSystem.setBlockIDWithAutoChunk(
                    islandID, 
                    Vec3(x, y, z), 
                    BlockID::STONE
                );
            }
        }
    }
    
    // Create separate floating cube (5x5x5 far away)
    std::cout << "Building disconnected satellite cube..." << std::endl;
    for (int x = 20; x < 25; x++) {
        for (int y = 0; y < 5; y++) {
            for (int z = 0; z < 5; z++) {
                g_islandSystem.setBlockIDWithAutoChunk(
                    islandID, 
                    Vec3(x, y, z), 
                    BlockID::DIRT
                );
            }
        }
    }
    
    // Create another tiny floating piece
    std::cout << "Building tiny debris piece..." << std::endl;
    for (int x = -20; x < -18; x++) {
        for (int y = 10; y < 12; y++) {
            for (int z = -5; z < -3; z++) {
                g_islandSystem.setBlockIDWithAutoChunk(
                    islandID, 
                    Vec3(x, y, z), 
                    BlockID::DIRT
                );
            }
        }
    }
    
    // Generate meshes
    std::cout << "Generating meshes..." << std::endl;
    FloatingIsland* island = g_islandSystem.getIsland(islandID);
    if (island) {
        for (auto& [chunkCoord, chunk] : island->chunks) {
            if (chunk) {
                chunk->generateMesh();
                chunk->buildCollisionMesh();
            }
        }
    }
    
    std::cout << "✅ Test island created (ID: " << islandID << ")" << std::endl;
    std::cout << "   This island should have 3 separate groups" << std::endl;
    std::cout << "==================================\n" << std::endl;
    
    return islandID;
}

void testBlockBreakSplit(uint32_t islandID) {
    std::cout << "\n====== BLOCK BREAK SPLIT TEST ======" << std::endl;
    
    FloatingIsland* island = g_islandSystem.getIsland(islandID);
    if (!island) {
        std::cout << "❌ Island " << islandID << " not found!" << std::endl;
        return;
    }
    
    std::cout << "Creating narrow bridge structure for split testing..." << std::endl;
    
    // Clear island first
    for (auto& [chunkCoord, chunk] : island->chunks) {
        if (chunk) {
            for (int x = 0; x < VoxelChunk::SIZE; x++) {
                for (int y = 0; y < VoxelChunk::SIZE; y++) {
                    for (int z = 0; z < VoxelChunk::SIZE; z++) {
                        chunk->setVoxel(x, y, z, 0);
                    }
                }
            }
        }
    }
    
    // Create two cubes connected by a single-block bridge
    // Left cube
    for (int x = -10; x < -5; x++) {
        for (int y = -2; y < 3; y++) {
            for (int z = -2; z < 3; z++) {
                g_islandSystem.setBlockIDWithAutoChunk(islandID, Vec3(x, y, z), BlockID::STONE);
            }
        }
    }
    
    // Bridge (critical single block)
    Vec3 criticalBlock(0, 0, 0);
    g_islandSystem.setBlockIDWithAutoChunk(islandID, criticalBlock, BlockID::DIRT);
    
    // Right cube
    for (int x = 5; x < 10; x++) {
        for (int y = -2; y < 3; y++) {
            for (int z = -2; z < 3; z++) {
                g_islandSystem.setBlockIDWithAutoChunk(islandID, Vec3(x, y, z), BlockID::STONE);
            }
        }
    }
    
    // Generate meshes
    for (auto& [chunkCoord, chunk] : island->chunks) {
        if (chunk) {
            chunk->generateMesh();
        }
    }
    
    std::cout << "✅ Created two cubes connected by single block at (0,0,0)" << std::endl;
    
    // Test if breaking the bridge block would split using new fast method
    std::cout << "Testing if breaking bridge block would cause split..." << std::endl;
    Vec3 fragmentAnchor;
    bool wouldSplit = ConnectivityAnalyzer::wouldBreakingCauseSplit(island, criticalBlock, fragmentAnchor);
    
    if (wouldSplit) {
        std::cout << "✅ Correctly detected: Breaking block WOULD split island!" << std::endl;
        
        std::cout << "Breaking the block now..." << std::endl;
        g_islandSystem.setVoxelInIsland(islandID, criticalBlock, 0);
        
        std::cout << "Analyzing split..." << std::endl;
        auto newIslands = ConnectivityAnalyzer::splitIslandByConnectivity(&g_islandSystem, islandID);
        
        if (!newIslands.empty()) {
            std::cout << "💥 SUCCESS! Island split into " << (newIslands.size() + 1) 
                      << " separate islands!" << std::endl;
            std::cout << "   Original Island ID: " << islandID << std::endl;
            for (size_t i = 0; i < newIslands.size(); i++) {
                std::cout << "   New Island " << (i+1) << " ID: " << newIslands[i] << std::endl;
            }
        } else {
            std::cout << "⚠️ Split detection succeeded but no new islands created" << std::endl;
        }
    } else {
        std::cout << "❌ ERROR: Should have detected split but didn't!" << std::endl;
    }
    
    std::cout << "====================================\n" << std::endl;
}

} // namespace ConnectivityTest
