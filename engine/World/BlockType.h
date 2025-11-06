// BlockType.h - ID-based block type system
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "BlockProperties.h"

// Block type IDs - simple, efficient, and network-friendly
namespace BlockID {
    constexpr uint8_t AIR = 0;
    
    // === NATURAL TERRAIN BLOCKS ===
    constexpr uint8_t STONE = 1;
    constexpr uint8_t DIRT = 2;
    constexpr uint8_t GRAVEL = 3;
    constexpr uint8_t CLAY = 4;
    constexpr uint8_t MOSS = 5;
    constexpr uint8_t SAND = 6;
    
    // === WOOD/TREE BLOCKS ===
    constexpr uint8_t WOOD_OAK = 7;
    constexpr uint8_t WOOD_BIRCH = 8;
    constexpr uint8_t WOOD_PINE = 9;
    constexpr uint8_t WOOD_JUNGLE = 10;
    constexpr uint8_t WOOD_PALM = 11;
    constexpr uint8_t LEAVES_GREEN = 12;
    constexpr uint8_t LEAVES_DARK = 13;
    constexpr uint8_t LEAVES_PALM = 14;
    
    // === ICE & SNOW BLOCKS ===
    constexpr uint8_t ICE = 15;
    constexpr uint8_t PACKED_ICE = 16;
    constexpr uint8_t SNOW = 17;
    
    // === STONE VARIANTS ===
    constexpr uint8_t SANDSTONE = 18;
    constexpr uint8_t GRANITE = 19;
    constexpr uint8_t BASALT = 20;
    constexpr uint8_t LIMESTONE = 21;
    constexpr uint8_t MARBLE = 22;
    constexpr uint8_t OBSIDIAN = 23;
    
    // === VOLCANIC BLOCKS ===
    constexpr uint8_t LAVA_ROCK = 24;
    constexpr uint8_t VOLCANIC_ASH = 25;
    constexpr uint8_t MAGMA = 26;
    constexpr uint8_t LAVA = 27;
    
    // === BASE ORES ===
    constexpr uint8_t COAL = 28;
    constexpr uint8_t IRON_BLOCK = 29;
    constexpr uint8_t COPPER_BLOCK = 30;
    constexpr uint8_t GOLD_BLOCK = 31;
    
    // === PRECIOUS GEMS ===
    constexpr uint8_t DIAMOND_BLOCK = 32;
    constexpr uint8_t EMERALD_BLOCK = 33;
    constexpr uint8_t RUBY_BLOCK = 34;
    constexpr uint8_t SAPPHIRE_BLOCK = 35;
    constexpr uint8_t AMETHYST = 36;
    constexpr uint8_t QUARTZ = 37;
    
    // === CRYSTAL BLOCKS ===
    constexpr uint8_t CRYSTAL_BLUE = 38;
    constexpr uint8_t CRYSTAL_GREEN = 39;
    constexpr uint8_t CRYSTAL_PURPLE = 40;
    constexpr uint8_t CRYSTAL_PINK = 41;
    
    // === SPECIAL MATERIALS ===
    constexpr uint8_t SALT_BLOCK = 42;
    constexpr uint8_t MUSHROOM_BLOCK = 43;
    constexpr uint8_t CORAL = 44;
    
    // === FLUIDS ===
    constexpr uint8_t WATER = 45;
    
    // === SPECIAL/OBJ BLOCKS (100+) ===
    constexpr uint8_t LAMP = 100;
    constexpr uint8_t ROCK = 101;
    constexpr uint8_t DECOR_GRASS = 102;
    constexpr uint8_t QUANTUM_FIELD_GENERATOR = 103;
    
    constexpr uint8_t MAX_BLOCK_TYPES = 255;
}

enum class BlockRenderType {
    VOXEL,    // Traditional meshed voxel blocks
    OBJ       // GPU instanced OBJ models
};

struct BlockTypeInfo {
    uint8_t id;
    std::string name;           // For debugging/display only
    BlockRenderType renderType;
    std::string assetPath;      // For OBJ blocks, path to the model file
    BlockProperties properties; // Block metadata and behavior
    
    BlockTypeInfo(uint8_t blockID, const std::string& blockName, BlockRenderType type, 
                  const std::string& asset = "", const BlockProperties& props = BlockProperties())
        : id(blockID), name(blockName), renderType(type), assetPath(asset), properties(props) {}
};

class BlockTypeRegistry {
public:
    static BlockTypeRegistry& getInstance() {
        static BlockTypeRegistry instance;
        return instance;
    }
    
    // Register a new block type
    void registerBlockType(uint8_t id, const std::string& name, BlockRenderType renderType, 
                          const std::string& assetPath = "", const BlockProperties& properties = BlockProperties());
    
    // Get block type info by ID (primary method)
    const BlockTypeInfo* getBlockType(uint8_t id) const;
    
    // Get block name for debugging (should rarely be used)
    const std::string& getBlockName(uint8_t id) const;
    
    // Check if a block type exists
    bool hasBlockType(uint8_t id) const;
    
    // Get all registered block types
    const std::vector<BlockTypeInfo>& getAllBlockTypes() const { return m_blockTypes; }

private:
    BlockTypeRegistry();
    void initializeDefaultBlocks();
    
    std::vector<BlockTypeInfo> m_blockTypes;  // Simple array indexed by block ID
    static const std::string UNKNOWN_BLOCK_NAME;
};


