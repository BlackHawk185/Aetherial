// BlockType.cpp - Implementation of ID-based block type system
#include "BlockType.h"
#include <iostream>

const std::string BlockTypeRegistry::UNKNOWN_BLOCK_NAME = "unknown";

void BlockTypeRegistry::registerBlockType(uint8_t id, const std::string& name, BlockRenderType renderType, 
                                         const std::string& assetPath, const BlockProperties& properties) {
    // Ensure we have enough space in the vector
    if (id >= m_blockTypes.size()) {
        m_blockTypes.resize(id + 1, BlockTypeInfo(0, "", BlockRenderType::VOXEL));
    }
    
    m_blockTypes[id] = BlockTypeInfo(id, name, renderType, assetPath, properties);
    // std::cout << "Registered block type: " << name << " (ID: " << (int)id << ")" << std::endl;
}

const BlockTypeInfo* BlockTypeRegistry::getBlockType(uint8_t id) const {
    if (id < m_blockTypes.size() && !m_blockTypes[id].name.empty()) {
        return &m_blockTypes[id];
    }
    return nullptr;
}

const std::string& BlockTypeRegistry::getBlockName(uint8_t id) const {
    if (id < m_blockTypes.size() && !m_blockTypes[id].name.empty()) {
        return m_blockTypes[id].name;
    }
    return UNKNOWN_BLOCK_NAME;
}

bool BlockTypeRegistry::hasBlockType(uint8_t id) const {
    return id < m_blockTypes.size() && !m_blockTypes[id].name.empty();
}

BlockTypeRegistry::BlockTypeRegistry() {
    m_blockTypes.reserve(BlockID::MAX_BLOCK_TYPES);
    initializeDefaultBlocks();
}

void BlockTypeRegistry::initializeDefaultBlocks() {
    // Register default block types with properties
    registerBlockType(BlockID::AIR, "air", BlockRenderType::VOXEL, "", BlockProperties::Air());
    registerBlockType(BlockID::STONE, "stone", BlockRenderType::VOXEL, "", BlockProperties::Solid(1.5f));
    registerBlockType(BlockID::DIRT, "dirt", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.5f));
    registerBlockType(BlockID::GRASS, "grass", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.6f));
    
    // Decorative/OBJ blocks (disabled - no models available)
    // registerBlockType(BlockID::TREE, "tree", BlockRenderType::OBJ, "assets/models/tree.glb", 
    //                  BlockProperties::Solid(2.0f));
    // registerBlockType(BlockID::LAMP, "lamp", BlockRenderType::OBJ, "assets/models/lamp.glb", 
    //                  BlockProperties::LightSource(14, 0.5f));
    // registerBlockType(BlockID::ROCK, "rock", BlockRenderType::OBJ, "assets/models/rock.glb", 
    //                  BlockProperties::Solid(3.0f));
    
    // Decorative grass tuft (transparent, requires support, fragile)
    BlockProperties grassProps = BlockProperties::Transparent(0.1f);
    grassProps.requiresSupport = true;
    registerBlockType(BlockID::DECOR_GRASS, "decor_grass", BlockRenderType::OBJ, 
                     "assets/models/grass.glb", grassProps);
    
    // Quantum Field Generator - Core faction mechanic
    registerBlockType(BlockID::QUANTUM_FIELD_GENERATOR, "quantum_field_generator", 
                     BlockRenderType::OBJ, "assets/models/quantumFieldGenerator.glb", 
                     BlockProperties::QuantumFieldGenerator());
    
    // Elemental/crafted blocks (voxel blocks for now until textures are ready)
    registerBlockType(BlockID::COAL, "coal", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.7f));
    registerBlockType(BlockID::IRON_BLOCK, "iron_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(5.0f));
    registerBlockType(BlockID::GOLD_BLOCK, "gold_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(3.0f));
    registerBlockType(BlockID::COPPER_BLOCK, "copper_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(3.5f));
    registerBlockType(BlockID::WATER, "water", BlockRenderType::VOXEL, "", BlockProperties::Transparent(0.1f));
    registerBlockType(BlockID::SAND, "sand", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.5f));
    registerBlockType(BlockID::SALT_BLOCK, "salt_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.3f));
    registerBlockType(BlockID::LIMESTONE, "limestone", BlockRenderType::VOXEL, "", BlockProperties::Solid(1.5f));
    registerBlockType(BlockID::ICE, "ice", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.9f));
    registerBlockType(BlockID::DIAMOND_BLOCK, "diamond_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(10.0f));
    
    std::cout << "BlockTypeRegistry initialized with " << m_blockTypes.size() << " block types" << std::endl;
}
