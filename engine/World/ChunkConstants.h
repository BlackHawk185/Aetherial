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
    
    // Region subdivision for partial mesh updates
    // Each chunk is subdivided into REGIONS_PER_AXIS^3 regions for granular remeshing
    static constexpr int REGION_SIZE = 64;  // 64³ voxels per region
    static constexpr int REGIONS_PER_AXIS = CHUNK_SIZE / REGION_SIZE;  // 4 regions per axis
    static constexpr int TOTAL_REGIONS = REGIONS_PER_AXIS * REGIONS_PER_AXIS * REGIONS_PER_AXIS;  // 64 total regions
    
    // Convert voxel coordinates to region index
    static constexpr inline int voxelToRegionCoord(int voxelCoord) {
        return voxelCoord / REGION_SIZE;
    }
    
    // Convert region coordinates to linear region index
    static constexpr inline int regionCoordsToIndex(int rx, int ry, int rz) {
        return rx + ry * REGIONS_PER_AXIS + rz * REGIONS_PER_AXIS * REGIONS_PER_AXIS;
    }
    
    // Convert voxel coordinates directly to region index
    static constexpr inline int voxelToRegionIndex(int x, int y, int z) {
        return regionCoordsToIndex(
            voxelToRegionCoord(x),
            voxelToRegionCoord(y),
            voxelToRegionCoord(z)
        );
    }
}
