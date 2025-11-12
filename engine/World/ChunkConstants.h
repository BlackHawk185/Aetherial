// ChunkConstants.h - Centralized chunk configuration for the entire engine
// Modify CHUNK_SIZE here to change chunk dimensions globally
#pragma once

#include <cstdint>

namespace ChunkConfig
{
    // Global chunk size - adjust this value to prototype different chunk dimensions
    // All systems (rendering, physics, networking, collision) derive from this value
    static constexpr int CHUNK_SIZE = 256;
    
    // Derived constants (automatically updated when CHUNK_SIZE changes)
    static constexpr int CHUNK_VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;
    static constexpr float CHUNK_SIZE_F = static_cast<float>(CHUNK_SIZE);
    
    // Network serialization buffer size (for worst-case uncompressed data)
    static constexpr size_t MAX_CHUNK_DATA_SIZE = CHUNK_VOLUME * sizeof(uint8_t);
}
