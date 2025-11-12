// VoxelChunk.h - Dynamic physics-enabled voxel chunks with light mapping
#pragma once

#include "../Math/Vec3.h"
#include "BlockType.h"
#include "ChunkConstants.h"
#include <array>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <string>
#include <memory>
#include <atomic>
#include <functional>

// Forward declaration for OpenGL types
using GLuint = uint32_t;

// Unified quad/face representation (used for both render and collision)
// Pack tightly to match GPU buffer layout
#pragma pack(push, 1)
struct QuadFace
{
    Vec3 position;   // Center position (12 bytes, offset 0)
    Vec3 normal;     // Face normal (12 bytes, offset 12)
    float width;     // Width of the quad (4 bytes, offset 24)
    float height;    // Height of the quad (4 bytes, offset 28)
    uint8_t blockType; // Block type (1 byte, offset 32)
    uint8_t faceDir;   // Face direction 0-5 (1 byte, offset 33)
    uint16_t padding;  // Padding (2 bytes, offset 34) - Total: 36 bytes (was 44)
};
#pragma pack(pop)

struct VoxelMesh
{
    // Direct quad storage - entire chunk meshed as one unit
    std::vector<QuadFace> quads;
    bool needsGPUUpload = false;
    
    // Voxel-to-quad reverse mapping for explosion system
    // Maps (voxelIndex * 6 + faceDir) -> quad index
    // This allows storing up to 6 quad indices per voxel (one per face direction)
    std::unordered_map<uint32_t, uint16_t> voxelFaceToQuadIndex;
    
    // Track which voxels have been exploded (non-greedy)
    // Using vector<bool> for space efficiency (1 bit per voxel = 2MB/chunk)
    std::vector<bool> isExploded;
    
    GLuint instanceVBO = 0;  // Per-chunk instance buffer
    
    VoxelMesh() {
        isExploded.resize(ChunkConfig::CHUNK_VOLUME, false);
    }
};

class IslandChunkSystem;  // Forward declaration

class VoxelChunk
{
   public:
    static constexpr int SIZE = ChunkConfig::CHUNK_SIZE;  // Use global chunk size
    static constexpr int VOLUME = ChunkConfig::CHUNK_VOLUME;
    
    // Static island system pointer for inter-chunk queries (must be public for IslandChunkSystem to access)
    static IslandChunkSystem* s_islandSystem;
    static void setIslandSystem(IslandChunkSystem* system) { s_islandSystem = system; }

    VoxelChunk();
    ~VoxelChunk();
    
    // Set whether this chunk is on the client (needs GPU upload) or server (CPU only)
    void setIsClient(bool isClient) { m_isClientChunk = isClient; }
    bool isClient() const { return m_isClientChunk; }

    // Voxel data access (ID-based - clean and efficient)
    uint8_t getVoxel(int x, int y, int z) const;
    void setVoxel(int x, int y, int z, uint8_t type);
    
    // SERVER-ONLY: Direct voxel data modification without triggering mesh updates
    void setVoxelDataDirect(int x, int y, int z, uint8_t type);
    
    // Block type access using IDs (for debugging only)
    uint8_t getBlockID(int x, int y, int z) const { return getVoxel(x, y, z); }
    void setBlockID(int x, int y, int z, uint8_t blockID) { setVoxel(x, y, z, blockID); }
    bool hasBlockID(int x, int y, int z, uint8_t blockID) const { return getVoxel(x, y, z) == blockID; }

    // Network serialization - get raw voxel data for transmission
    const uint8_t* getRawVoxelData() const
    {
        return voxels.data();
    }
    
    // Network serialization helpers
    void setRawVoxelData(const uint8_t* data, uint32_t size);
    uint32_t getVoxelDataSize() const { return VOLUME; }

    // Mesh generation and management
    // Generate mesh for entire chunk as single unit
    void generateMesh(bool generateLighting = true);
    std::vector<QuadFace> generateFullChunkMesh();
    
    // Explosion system for direct quad manipulation
    void explodeQuad(uint16_t quadIndex);
    void addSimpleFacesForVoxel(int x, int y, int z);
    void uploadMeshToGPU();

    // **LOD AND CULLING SUPPORT**
    int calculateLOD(const Vec3& cameraPos) const;
    bool shouldRender(const Vec3& cameraPos, float maxDistance = 1024.0f) const;
    
    // Cached world-space AABB
    struct WorldAABB {
        Vec3 min, max;
        bool valid = false;
    };
    void setCachedWorldAABB(const Vec3& min, const Vec3& max) {
        m_cachedWorldAABB.min = min;
        m_cachedWorldAABB.max = max;
        m_cachedWorldAABB.valid = true;
    }
    const WorldAABB& getCachedWorldAABB() const { return m_cachedWorldAABB; }
    void invalidateCachedWorldAABB() { m_cachedWorldAABB.valid = false; }

    // Direct mesh access (fast - no atomic overhead)
    std::shared_ptr<VoxelMesh> getRenderMesh() const { return renderMesh; }
    void setRenderMesh(std::shared_ptr<VoxelMesh> newMesh) { renderMesh = newMesh; }

    // Decorative/model instance positions (generic per block type)
    const std::vector<Vec3>& getModelInstances(uint8_t blockID) const;
    
    // NOTE: For raycasting, use VoxelRaycaster::raycast() - it's DDA-based and handles rotated islands

    // Island context for inter-chunk culling
    void setIslandContext(uint32_t islandID, const Vec3& chunkCoord);

   private:
    std::array<uint8_t, VOLUME> voxels;
    std::shared_ptr<VoxelMesh> renderMesh;  // Direct access (no atomic overhead)
    
    // Island context for inter-chunk culling
    uint32_t m_islandID = 0;
    Vec3 m_chunkCoord{0, 0, 0};
    
    // Cached world-space AABB
    WorldAABB m_cachedWorldAABB;

    // NEW: Per-block-type model instance positions (for BlockRenderType::OBJ blocks)
    // Key: BlockID, Value: list of instance positions within this chunk
    std::unordered_map<uint8_t, std::vector<Vec3>> m_modelInstances;
    
    // Client/Server flag - only client chunks upload to GPU
    bool m_isClientChunk = false;

    // Quad generation helper
    void addQuad(std::vector<QuadFace>& quads, float x, float y, float z, int face, int width, int height, uint8_t blockType);
    
    // Greedy meshing per face direction
    void greedyMeshFace(std::vector<QuadFace>& quads, int face);
    
    bool isVoxelSolid(int x, int y, int z) const;
    
    // Intra-chunk culling only (inter-chunk culling removed for performance)
    bool isFaceExposed(int x, int y, int z, int face) const;
};

