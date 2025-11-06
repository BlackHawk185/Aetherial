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
    // Quad-based mesh for instanced rendering
    std::vector<QuadFace> quads;
    GLuint instanceVBO = 0;  // Instance buffer for QuadFace data
};

struct CollisionFace
{
    Vec3 position;  // Center position of the face
    Vec3 normal;    // Normal vector of the face
    float width;    // Width of the quad
    float height;   // Height of the quad
};

struct CollisionMesh
{
    std::vector<CollisionFace> faces;
    
    CollisionMesh() = default;
    CollisionMesh(const CollisionMesh& other) : faces(other.faces) {}
    CollisionMesh& operator=(const CollisionMesh& other) {
        if (this != &other) {
            faces = other.faces;
        }
        return *this;
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
    
    // Event-driven GPU update callback - called immediately when mesh changes
    using MeshUpdateCallback = std::function<void(VoxelChunk*)>;
    void setMeshUpdateCallback(MeshUpdateCallback callback) { m_meshUpdateCallback = callback; }

    // Voxel data access (ID-based - clean and efficient)
    uint8_t getVoxel(int x, int y, int z) const;
    void setVoxel(int x, int y, int z, uint8_t type);
    
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
    void generateMesh(bool generateLighting = true);
    
    // Incremental quad manipulation (for direct block modifications)
    void addBlockQuads(int x, int y, int z, uint8_t blockType);
    void removeBlockQuads(int x, int y, int z);
    void updateNeighborQuads(int x, int y, int z, bool blockWasAdded);
    
    // Control incremental updates (disable during world generation)
    void enableIncrementalUpdates() { m_incrementalUpdatesEnabled = true; }
    void disableIncrementalUpdates() { m_incrementalUpdatesEnabled = false; }
    bool areIncrementalUpdatesEnabled() const { return m_incrementalUpdatesEnabled; }

    // Mesh state
    bool isDirty() const
    {
        return meshDirty;
    }

    // **LOD AND CULLING SUPPORT**
    int calculateLOD(const Vec3& cameraPos) const;
    bool shouldRender(const Vec3& cameraPos, float maxDistance = 1024.0f) const;

    // Direct mesh access (fast - no atomic overhead)
    std::shared_ptr<CollisionMesh> getCollisionMesh() const { return collisionMesh; }
    void setCollisionMesh(std::shared_ptr<CollisionMesh> newMesh) { collisionMesh = newMesh; }
    
    std::shared_ptr<VoxelMesh> getRenderMesh() const { return renderMesh; }
    void setRenderMesh(std::shared_ptr<VoxelMesh> newMesh) { renderMesh = newMesh; }
    
    // Lazy mesh generation - generates mesh on first access if dirty
    std::shared_ptr<VoxelMesh> getRenderMeshLazy()
    {
        if (!renderMesh || meshDirty) {
            generateMesh(false);  // Generate without lighting (real-time lighting)
        }
        return renderMesh;
    }

    // Decorative/model instance positions (generic per block type)
    const std::vector<Vec3>& getModelInstances(uint8_t blockID) const;
    
    // NOTE: For raycasting, use VoxelRaycaster::raycast() - it's DDA-based and handles rotated islands

   public:
    // Island context for inter-chunk culling
    void setIslandContext(uint32_t islandID, const Vec3& chunkCoord);
    
    // Friend class for async mesh generation
    friend class AsyncMeshGenerator;

   private:
    std::array<uint8_t, VOLUME> voxels;
    std::shared_ptr<VoxelMesh> renderMesh;  // Direct access (no atomic overhead)
    std::shared_ptr<CollisionMesh> collisionMesh;  // Direct access (no atomic overhead)
    bool meshDirty = true;
    
    // Island context for inter-chunk culling
    uint32_t m_islandID = 0;
    Vec3 m_chunkCoord{0, 0, 0};

    // NEW: Per-block-type model instance positions (for BlockRenderType::OBJ blocks)
    // Key: BlockID, Value: list of instance positions within this chunk
    std::unordered_map<uint8_t, std::vector<Vec3>> m_modelInstances;
    
    // Fast quad lookup for incremental updates: (x, y, z, face) -> quad index
    std::unordered_map<uint64_t, size_t> m_quadLookup;
    
    // Disable incremental updates during bulk operations (world generation)
    bool m_incrementalUpdatesEnabled = false;
    
    // Client/Server flag - only client chunks upload to GPU
    bool m_isClientChunk = false;
    
    // Event-driven GPU update callback
    MeshUpdateCallback m_meshUpdateCallback;
    
    // Helper: Pack voxel coord + face direction into lookup key
    static inline uint64_t makeQuadKey(int x, int y, int z, int face) {
        return ((uint64_t)x << 48) | ((uint64_t)y << 32) | ((uint64_t)z << 16) | (uint64_t)face;
    }

    // Quad generation helper
    void addQuad(std::vector<QuadFace>& quads, float x, float y, float z, int face, int width, int height, uint8_t blockType);
    
    bool isVoxelSolid(int x, int y, int z) const;
    
    // Intra-chunk culling only (inter-chunk culling removed for performance)
    bool isFaceExposed(int x, int y, int z, int face) const;
    
    // Simple meshing implementation
    void generateSimpleMeshInto(std::vector<QuadFace>& quads);
};

