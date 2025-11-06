// BlockTextureMapping.h - Maps block IDs to texture filenames
#pragma once

#include "BlockType.h"
#include <string>

inline std::string getBlockTextureName(uint8_t blockID) {
    switch (blockID) {
        // Natural terrain
        case BlockID::STONE: return "stone.png";
        case BlockID::DIRT: return "dirt.png";
        case BlockID::GRAVEL: return "gravel.png";
        case BlockID::CLAY: return "clay.png";
        case BlockID::MOSS: return "moss.png";
        case BlockID::SAND: return "sand.png";
        
        // Wood/Tree blocks
        case BlockID::WOOD_OAK: return "wood_oak.png";
        case BlockID::WOOD_BIRCH: return "wood_birch.png";
        case BlockID::WOOD_PINE: return "wood_pine.png";
        case BlockID::WOOD_JUNGLE: return "wood_jungle.png";
        case BlockID::WOOD_PALM: return "wood_palm.png";
        case BlockID::LEAVES_GREEN: return "leaves_green.png";
        case BlockID::LEAVES_DARK: return "leaves_dark.png";
        case BlockID::LEAVES_PALM: return "leaves_palm.png";
        
        // Ice & Snow
        case BlockID::ICE: return "ice.png";
        case BlockID::PACKED_ICE: return "packed_ice.png";
        case BlockID::SNOW: return "snow.png";
        
        // Stone Variants
        case BlockID::SANDSTONE: return "sandstone.png";
        case BlockID::GRANITE: return "granite.png";
        case BlockID::BASALT: return "basalt.png";
        case BlockID::LIMESTONE: return "limestone.png";
        case BlockID::MARBLE: return "marble.png";
        case BlockID::OBSIDIAN: return "obsidian.png";
        
        // Volcanic
        case BlockID::LAVA_ROCK: return "lava_rock.png";
        case BlockID::VOLCANIC_ASH: return "volcanic_ash.png";
        case BlockID::MAGMA: return "magma.png";
        case BlockID::LAVA: return "lava.png";
        
        // Base Ores
        case BlockID::COAL: return "coal.png";
        case BlockID::IRON_BLOCK: return "iron_block.png";
        case BlockID::COPPER_BLOCK: return "copper_block.png";
        case BlockID::GOLD_BLOCK: return "gold_block.png";
        
        // Precious Gems
        case BlockID::DIAMOND_BLOCK: return "diamond_block.png";
        case BlockID::EMERALD_BLOCK: return "emerald_block.png";
        case BlockID::RUBY_BLOCK: return "ruby_block.png";
        case BlockID::SAPPHIRE_BLOCK: return "sapphire_block.png";
        case BlockID::AMETHYST: return "amethyst.png";
        case BlockID::QUARTZ: return "quartz.png";
        
        // Crystals
        case BlockID::CRYSTAL_BLUE: return "crystal_blue.png";
        case BlockID::CRYSTAL_GREEN: return "crystal_green.png";
        case BlockID::CRYSTAL_PURPLE: return "crystal_purple.png";
        case BlockID::CRYSTAL_PINK: return "crystal_pink.png";
        
        // Special Materials
        case BlockID::SALT_BLOCK: return "salt_block.png";
        case BlockID::MUSHROOM_BLOCK: return "mushroom_block.png";
        case BlockID::CORAL: return "coral.png";
        
        // Fluids
        case BlockID::WATER: return "water.png";
        
        // Default fallback
        default: return "stone.png";
    }
}
