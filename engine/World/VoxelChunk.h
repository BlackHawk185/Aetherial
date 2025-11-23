// VoxelChunk.h - Dynamic physics-enabled voxel chunks with light mapping
#pragma once

#include <glm/glm.hpp>
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
#include <future>
#include <mutex>

// Forward declaration for OpenGL types
using GLuint = uint32_t;

// Unified quad/face representation (used for both render and collision)
// QuadFace struct for vertex pulling - 32 bytes bit-packed (matches GPU shader std430 layout exactly)
// CRITICAL: position is island-relative and NEVER packed (islands move, need precision)
// Bit packing: width/height in 16-bit fixed-point (1/256 precision), normal 10:10:10, IDs in 16-bit
struct alignas(16) QuadFace
{
    glm::vec3 position;   // Island-relative CORNER position (12 bytes, offset 0) - NEVER PACK
    uint32_t packed0;     // width(16) | height(16) - dimensions in 8.8 fixed-point (4 bytes, offset 12)
    uint32_t packed1;     // normal(30) | faceDir(3) - packed normal + face direction (4 bytes, offset 16) [1 bit unused]
    uint32_t packed2;     // blockType(16) | islandID(16) - packed IDs (4 bytes, offset 20)
    uint32_t _padding0;   // Align to 32 bytes (4 bytes, offset 24)
    uint32_t _padding1;   // Align to 32 bytes (4 bytes, offset 28)
    
    // Unpacking helpers
    inline float getWidth() const { return float(packed0 & 0xFFFF) / 256.0f; }
    inline float getHeight() const { return float((packed0 >> 16) & 0xFFFF) / 256.0f; }
    inline uint32_t getFaceDir() const { return packed1 & 0x7; }  // 3 bits for 0-7 range (6 faces use 0-5)
    inline uint32_t getBlockType() const { return packed2 & 0xFFFF; }
    inline uint32_t getIslandID() const { return (packed2 >> 16) & 0xFFFF; }
    
    // Setters for convenience
    inline void setWidth(float w) { packed0 = (packed0 & 0xFFFF0000) | (uint32_t(w * 256.0f) & 0xFFFF); }
    inline void setHeight(float h) { packed0 = (packed0 & 0x0000FFFF) | ((uint32_t(h * 256.0f) & 0xFFFF) << 16); }
    inline void setIslandID(uint32_t id) { packed2 = (packed2 & 0x0000FFFF) | ((id & 0xFFFF) << 16); }
};
static_assert(sizeof(QuadFace) == 32, "QuadFace must be 32 bytes with 16-byte alignment for std430 array");
static_assert(alignof(QuadFace) == 16, "QuadFace must have 16-byte alignment for std430 array");

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
    // Async mesh generation (eliminates 100+ms frame drops)
    void generateMeshAsync(bool generateLighting = true);
    bool tryUploadPendingMesh();  // Non-blocking upload check
    
    // Explosion system for direct quad manipulation
    void explodeQuad(uint16_t quadIndex);
    void addSimpleFacesForVoxel(int x, int y, int z);
    void uploadMeshToGPU();
    
    // Direct quad manipulation for block changes (no full remesh)
    void setVoxelWithQuadManipulation(int x, int y, int z, uint8_t type);
    void removeVoxelQuads(int x, int y, int z);
    void addVoxelQuads(int x, int y, int z);

    // **LOD AND CULLING SUPPORT**
    int calculateLOD(const glm::vec3& cameraPos) const;
    bool shouldRender(const glm::vec3& cameraPos, float maxDistance = 1024.0f) const;
    
    // Cached world-space AABB for frustum culling
    struct WorldAABB {
        glm::vec3 min, max;
        bool valid = false;
    };
    void setCachedWorldAABB(const glm::vec3& min, const glm::vec3& max) {
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
    const std::vector<glm::vec3>& getModelInstances(uint8_t blockID) const;
    
    // NOTE: For raycasting, use VoxelRaycaster::raycast() - it's DDA-based and handles rotated islands

    // Island context for inter-chunk culling
    void setIslandContext(uint32_t islandID, const glm::vec3& chunkCoord);

   public:
    // Island context for inter-chunk culling
    uint32_t m_islandID = 0;
    glm::vec3 m_chunkCoord{0, 0, 0};
    
    // Generate full chunk mesh (island-relative positions)
    std::vector<QuadFace> generateFullChunkMesh();
    
   private:
    std::array<uint8_t, VOLUME> voxels;
    std::shared_ptr<VoxelMesh> renderMesh;  // Direct access (no atomic overhead)
    
    // Cached world-space AABB for frustum culling optimization
    WorldAABB m_cachedWorldAABB;

    // NEW: Per-block-type model instance positions (for BlockRenderType::OBJ blocks)
    // Key: BlockID, Value: list of instance positions within this chunk
    std::unordered_map<uint8_t, std::vector<glm::vec3>> m_modelInstances;
    
    // Client/Server flag - only client chunks upload to GPU
    bool m_isClientChunk = false;
    
    // Async mesh generation state
    std::mutex m_meshMutex;
    std::future<std::shared_ptr<VoxelMesh>> m_pendingMeshFuture;

    // Quad generation helper (island-relative positions)
    void addQuad(std::vector<QuadFace>& quads, float x, float y, float z, int face, int width, int height, uint8_t blockType);
    
    // Greedy meshing per face direction (island-relative)
    void greedyMeshFace(std::vector<QuadFace>& quads, int face);
    
    bool isVoxelSolid(int x, int y, int z) const;
    
    // Interchunk culling enabled (queries neighboring chunks for proper face culling)
    bool isFaceExposed(int x, int y, int z, int face) const;
};

