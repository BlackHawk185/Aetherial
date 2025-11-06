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
    // === AIR ===
    registerBlockType(BlockID::AIR, "air", BlockRenderType::VOXEL, "", BlockProperties::Air());
    
    // === NATURAL TERRAIN BLOCKS ===
    registerBlockType(BlockID::STONE, "stone", BlockRenderType::VOXEL, "", BlockProperties::Solid(1.5f));
    registerBlockType(BlockID::DIRT, "dirt", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.5f));
    registerBlockType(BlockID::GRAVEL, "gravel", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.6f));
    registerBlockType(BlockID::CLAY, "clay", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.6f));
    registerBlockType(BlockID::MOSS, "moss", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.3f));
    registerBlockType(BlockID::SAND, "sand", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.5f));
    
    // === WOOD/TREE BLOCKS ===
    registerBlockType(BlockID::WOOD_OAK, "wood_oak", BlockRenderType::VOXEL, "", BlockProperties::Solid(2.0f));
    registerBlockType(BlockID::WOOD_BIRCH, "wood_birch", BlockRenderType::VOXEL, "", BlockProperties::Solid(2.0f));
    registerBlockType(BlockID::WOOD_PINE, "wood_pine", BlockRenderType::VOXEL, "", BlockProperties::Solid(2.0f));
    registerBlockType(BlockID::WOOD_JUNGLE, "wood_jungle", BlockRenderType::VOXEL, "", BlockProperties::Solid(2.0f));
    registerBlockType(BlockID::WOOD_PALM, "wood_palm", BlockRenderType::VOXEL, "", BlockProperties::Solid(2.0f));
    
    BlockProperties leavesProps = BlockProperties::Transparent(0.3f);
    registerBlockType(BlockID::LEAVES_GREEN, "leaves_green", BlockRenderType::VOXEL, "", leavesProps);
    registerBlockType(BlockID::LEAVES_DARK, "leaves_dark", BlockRenderType::VOXEL, "", leavesProps);
    registerBlockType(BlockID::LEAVES_PALM, "leaves_palm", BlockRenderType::VOXEL, "", leavesProps);
    
    // === DECORATIVE/OBJ BLOCKS ===
    BlockProperties grassProps = BlockProperties::Transparent(0.1f);
    grassProps.requiresSupport = true;
    registerBlockType(BlockID::DECOR_GRASS, "decor_grass", BlockRenderType::OBJ, 
                     "assets/models/grass.glb", grassProps);
    
    registerBlockType(BlockID::QUANTUM_FIELD_GENERATOR, "quantum_field_generator", 
                     BlockRenderType::OBJ, "assets/models/quantumFieldGenerator.glb", 
                     BlockProperties::QuantumFieldGenerator());
    
    // === ICE & SNOW BLOCKS ===
    registerBlockType(BlockID::ICE, "ice", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.5f));
    registerBlockType(BlockID::PACKED_ICE, "packed_ice", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.9f));
    registerBlockType(BlockID::SNOW, "snow", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.2f));
    
    // === STONE VARIANTS ===
    // Sandstone: Formed from compressed sand (Sand recipe)
    registerBlockType(BlockID::SANDSTONE, "sandstone", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.8f));
    
    // Granite: Igneous rock, harder than stone
    registerBlockType(BlockID::GRANITE, "granite", BlockRenderType::VOXEL, "", BlockProperties::Solid(2.0f));
    
    // Basalt: Volcanic rock (Lava Rock + Stone recipe)
    registerBlockType(BlockID::BASALT, "basalt", BlockRenderType::VOXEL, "", BlockProperties::Solid(1.8f));
    
    // Limestone: Sedimentary rock (Clay + Stone recipe)
    registerBlockType(BlockID::LIMESTONE, "limestone", BlockRenderType::VOXEL, "", BlockProperties::Solid(1.3f));
    
    // Marble: Metamorphic limestone (Limestone + Heat recipe)
    registerBlockType(BlockID::MARBLE, "marble", BlockRenderType::VOXEL, "", BlockProperties::Solid(1.7f));
    
    // Obsidian: Volcanic glass - extremely hard (Lava + Water recipe)
    registerBlockType(BlockID::OBSIDIAN, "obsidian", BlockRenderType::VOXEL, "", BlockProperties::Solid(50.0f));
    
    // === VOLCANIC BLOCKS ===
    registerBlockType(BlockID::LAVA_ROCK, "lava_rock", BlockRenderType::VOXEL, "", BlockProperties::Solid(1.2f));
    registerBlockType(BlockID::VOLCANIC_ASH, "volcanic_ash", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.3f));
    
    // Magma: Glowing molten rock (Lava Rock + Heat recipe)
    BlockProperties magmaProps = BlockProperties::LightSource(12, 1.5f);
    registerBlockType(BlockID::MAGMA, "magma", BlockRenderType::VOXEL, "", magmaProps);
    
    // Lava: Flowing molten rock (Magma + Heat recipe)
    BlockProperties lavaProps = BlockProperties::LightSource(15, 0.1f);
    lavaProps.isTransparent = true;
    registerBlockType(BlockID::LAVA, "lava", BlockRenderType::VOXEL, "", lavaProps);
    
    // === BASE ORES ===
    // Coal: Basic fuel (Stone + Carbon recipe)
    registerBlockType(BlockID::COAL, "coal", BlockRenderType::VOXEL, "", BlockProperties::Solid(3.0f));
    
    // Iron: Common metal (Stone + Iron Ore recipe)
    registerBlockType(BlockID::IRON_BLOCK, "iron_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(5.0f));
    
    // Copper: Conductive metal (Stone + Copper Ore recipe)
    registerBlockType(BlockID::COPPER_BLOCK, "copper_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(3.5f));
    
    // Gold: Precious metal (Stone + Gold Ore recipe)
    registerBlockType(BlockID::GOLD_BLOCK, "gold_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(3.0f));
    
    // === PRECIOUS GEMS ===
    // Diamond: Hardest natural material (Coal + Extreme Pressure recipe)
    registerBlockType(BlockID::DIAMOND_BLOCK, "diamond_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(10.0f));
    
    // Emerald: Rare green gem (Stone + Beryllium recipe)
    registerBlockType(BlockID::EMERALD_BLOCK, "emerald_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(8.0f));
    
    // Ruby: Red gem (Limestone + Chromium recipe)
    registerBlockType(BlockID::RUBY_BLOCK, "ruby_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(9.0f));
    
    // Sapphire: Blue gem (Limestone + Titanium recipe)
    registerBlockType(BlockID::SAPPHIRE_BLOCK, "sapphire_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(9.0f));
    
    // Amethyst: Purple crystal (Quartz + Iron recipe)
    registerBlockType(BlockID::AMETHYST, "amethyst", BlockRenderType::VOXEL, "", BlockProperties::Solid(7.0f));
    
    // Quartz: Clear crystal (Sand + Pressure recipe)
    registerBlockType(BlockID::QUARTZ, "quartz", BlockRenderType::VOXEL, "", BlockProperties::Solid(7.0f));
    
    // === CRYSTAL BLOCKS (Magical/Elemental) ===
    // Blue Crystal: Water-attuned (Water + Quartz recipe)
    BlockProperties blueCrystalProps = BlockProperties::LightSource(8, 6.0f);
    blueCrystalProps.isTransparent = true;
    registerBlockType(BlockID::CRYSTAL_BLUE, "crystal_blue", BlockRenderType::VOXEL, "", blueCrystalProps);
    
    // Green Crystal: Nature-attuned (Moss + Quartz recipe)
    BlockProperties greenCrystalProps = BlockProperties::LightSource(8, 6.0f);
    greenCrystalProps.isTransparent = true;
    registerBlockType(BlockID::CRYSTAL_GREEN, "crystal_green", BlockRenderType::VOXEL, "", greenCrystalProps);
    
    // Purple Crystal: Arcane-attuned (Amethyst + Quartz recipe)
    BlockProperties purpleCrystalProps = BlockProperties::LightSource(8, 6.0f);
    purpleCrystalProps.isTransparent = true;
    registerBlockType(BlockID::CRYSTAL_PURPLE, "crystal_purple", BlockRenderType::VOXEL, "", purpleCrystalProps);
    
    // Pink Crystal: Life-attuned (Ruby + Quartz recipe)
    BlockProperties pinkCrystalProps = BlockProperties::LightSource(8, 6.0f);
    pinkCrystalProps.isTransparent = true;
    registerBlockType(BlockID::CRYSTAL_PINK, "crystal_pink", BlockRenderType::VOXEL, "", pinkCrystalProps);
    
    // === SPECIAL MATERIALS ===
    // Salt: Preservative and seasoning (Water evaporation recipe)
    registerBlockType(BlockID::SALT_BLOCK, "salt_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.5f));
    
    // Mushroom Block: Organic material (Moss + Darkness recipe)
    registerBlockType(BlockID::MUSHROOM_BLOCK, "mushroom_block", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.2f));
    
    // Coral: Marine structure (Water + Limestone recipe)
    registerBlockType(BlockID::CORAL, "coral", BlockRenderType::VOXEL, "", BlockProperties::Solid(0.4f));
    
    // === FLUIDS ===
    // Water: Essential liquid
    BlockProperties waterProps = BlockProperties::Transparent(0.1f);
    waterProps.isTransparent = true;
    registerBlockType(BlockID::WATER, "water", BlockRenderType::VOXEL, "", waterProps);
    
    std::cout << "BlockTypeRegistry initialized with " << m_blockTypes.size() << " block types" << std::endl;
}
