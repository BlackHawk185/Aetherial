// BlockDamageTracker.h - Track damage state for multi-hit block breaking
#pragma once

#include <unordered_map>
#include <cstdint>
#include "../Math/Vec3.h"

/**
 * Tracks accumulated damage on blocks for multi-hit breaking system.
 * Damage persists between clicks (no Minecraft-style reset on interrupt).
 * 
 * Design:
 * - Each block has durability (hits required to break)
 * - LMB click = 1 hit damage
 * - Fast clicking = fast mining (skill-based)
 * - Damage state persists until block breaks or is placed over
 */
class BlockDamageTracker {
public:
    /**
     * Apply 1 hit of damage to a block
     * @return true if block should break (damage >= durability)
     */
    bool applyHit(uint32_t islandID, const Vec3& blockPos, uint8_t blockDurability);
    
    /**
     * Get current damage on a block (0 = no damage)
     */
    uint8_t getDamage(uint32_t islandID, const Vec3& blockPos) const;
    
    /**
     * Get damage percentage (0.0 - 1.0) for visual feedback
     */
    float getDamagePercent(uint32_t islandID, const Vec3& blockPos, uint8_t blockDurability) const;
    
    /**
     * Clear damage state for a block (called when block breaks or is replaced)
     */
    void clearDamage(uint32_t islandID, const Vec3& blockPos);
    
    /**
     * Clear all damage state (e.g., on world unload)
     */
    void clearAll();

private:
    // Key = (islandID << 32) | hash(blockPos), Value = accumulated damage
    struct BlockKey {
        uint32_t islandID;
        Vec3 blockPos;
        
        bool operator==(const BlockKey& other) const {
            return islandID == other.islandID && 
                   blockPos.x == other.blockPos.x && 
                   blockPos.y == other.blockPos.y && 
                   blockPos.z == other.blockPos.z;
        }
    };
    
    struct BlockKeyHash {
        std::size_t operator()(const BlockKey& key) const {
            // Simple hash combining island ID and block position
            std::size_t h1 = std::hash<uint32_t>{}(key.islandID);
            std::size_t h2 = std::hash<float>{}(key.blockPos.x);
            std::size_t h3 = std::hash<float>{}(key.blockPos.y);
            std::size_t h4 = std::hash<float>{}(key.blockPos.z);
            return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
        }
    };
    
    std::unordered_map<BlockKey, uint8_t, BlockKeyHash> m_damageMap;
};
