// BlockDamageTracker.cpp - Track damage state for multi-hit block breaking
#include "BlockDamageTracker.h"

bool BlockDamageTracker::applyHit(uint32_t islandID, const Vec3& blockPos, uint8_t blockDurability) {
    if (blockDurability == 0) {
        return true; // Air or instant-break blocks
    }
    
    BlockKey key{islandID, blockPos};
    uint8_t& damage = m_damageMap[key];
    damage++;
    
    if (damage >= blockDurability) {
        m_damageMap.erase(key); // Clear damage state on break
        return true;
    }
    
    return false;
}

uint8_t BlockDamageTracker::getDamage(uint32_t islandID, const Vec3& blockPos) const {
    BlockKey key{islandID, blockPos};
    auto it = m_damageMap.find(key);
    return (it != m_damageMap.end()) ? it->second : 0;
}

float BlockDamageTracker::getDamagePercent(uint32_t islandID, const Vec3& blockPos, uint8_t blockDurability) const {
    if (blockDurability == 0) return 0.0f;
    
    uint8_t damage = getDamage(islandID, blockPos);
    return static_cast<float>(damage) / static_cast<float>(blockDurability);
}

void BlockDamageTracker::clearDamage(uint32_t islandID, const Vec3& blockPos) {
    BlockKey key{islandID, blockPos};
    m_damageMap.erase(key);
}

void BlockDamageTracker::clearAll() {
    m_damageMap.clear();
}
