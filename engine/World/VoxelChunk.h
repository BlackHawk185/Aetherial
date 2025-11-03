// VoxelChunk.h - 16x16x16 dynamic physics-enabled voxel chunks with light mapping
#pragma once

#include "../Math/Vec3.h"
#include "BlockType.h"
#include <array>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <mutex>
#include <string>
#include <memory>
#include <atomic>

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
    float lightmapU; // Lightmap U coordinate (4 bytes, offset 32)
    float lightmapV; // Lightmap V coordinate (4 bytes, offset 36)
    uint8_t blockType; // Block type (1 byte, offset 40)
    uint8_t faceDir;   // Face direction 0-5 (1 byte, offset 41)
    uint16_t padding;  // Padding (2 bytes, offset 42) - Total: 44 bytes
};
#pragma pack(pop)

struct VoxelMesh
{
    // Quad-based mesh for instanced rendering
    std::vector<QuadFace> quads;
    GLuint instanceVBO = 0;  // Instance buffer for QuadFace data
    bool needsUpdate = true;
};

// Per-face light mapping data for the chunk
struct FaceLightMap 
{
    static constexpr int LIGHTMAP_SIZE = 32; // 32x32 per face
    uint32_t textureHandle = 0;  // OpenGL texture handle
    std::vector<uint8_t> data;  // RGB data
    bool needsUpdate = true;
    
    FaceLightMap() {
        // Initialize with RGB data
        data.resize(LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3);
    }
    
    ~FaceLightMap() {
        if (textureHandle != 0) {
            // Note: glDeleteTextures should only be called from OpenGL context
            // This will be handled by proper cleanup in VoxelChunk destructor
        }
    }
};

// Light mapping data for the chunk - one light map per face direction
struct ChunkLightMaps 
{
    // 6 face directions: +X, -X, +Y, -Y, +Z, -Z (matches face indices 4, 5, 2, 3, 0, 1)
    std::array<FaceLightMap, 6> faceMaps;
    
    FaceLightMap& getFaceMap(int faceDirection) {
        return faceMaps[faceDirection];
    }
    
    const FaceLightMap& getFaceMap(int faceDirection) const {
        return faceMaps[faceDirection];
    }
};

struct CollisionFace
{
    Vec3 position;  // Center position of the face
    Vec3 normal;    // Normal vector of the face
    float width;    // Width of the face (for greedy merged quads)
    float height;   // Height of the face (for greedy merged quads)
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
    static constexpr int SIZE = 16;  // 16x16x16 chunks
    static constexpr int VOLUME = SIZE * SIZE * SIZE;
    
    // Static island system pointer for inter-chunk queries (must be public for IslandChunkSystem to access)
    static IslandChunkSystem* s_islandSystem;
    static void setIslandSystem(IslandChunkSystem* system) { s_islandSystem = system; }

    VoxelChunk();
    ~VoxelChunk();

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

    // Mesh state
    bool isDirty() const
    {
        return meshDirty;
    }

    // **LOD AND CULLING SUPPORT**
    int calculateLOD(const Vec3& cameraPos) const;
    bool shouldRender(const Vec3& cameraPos, float maxDistance = 1024.0f) const;

    // Collision detection methods - thread-safe atomic access
    std::shared_ptr<const CollisionMesh> getCollisionMesh() const
    {
        return std::atomic_load(&collisionMesh);
    }
    
    void setCollisionMesh(std::shared_ptr<CollisionMesh> newMesh)
    {
        std::atomic_store(&collisionMesh, newMesh);
    }
    
    // Mesh access for VBO rendering
    VoxelMesh& getMesh() { return mesh; }
    const VoxelMesh& getMesh() const { return mesh; }
    std::mutex& getMeshMutex() const { return meshMutex; }

    // Decorative/model instance positions (generic per block type)
    const std::vector<Vec3>& getModelInstances(uint8_t blockID) const;
    void addModelInstance(uint8_t blockID, const Vec3& position);
    void clearModelInstances(uint8_t blockID);
    void clearAllModelInstances();
    
    // Light mapping access
    ChunkLightMaps& getLightMaps() { return lightMaps; }
    const ChunkLightMaps& getLightMaps() const { return lightMaps; }
    void updateLightMapTextures();  // Create/update OpenGL textures from light map data
    void markLightMapsDirty();      // Mark light maps as needing GPU texture update
    bool hasValidLightMaps() const; // Check if all lightmap textures are created
    bool hasLightMapData() const;   // Check if lightmap data exists (before texture creation)
    
    // NEW: Lighting dirty state management
    bool needsLightingUpdate() const { return lightingDirty; }
    void markLightingDirty() { lightingDirty = true; }
    void markLightingClean() { lightingDirty = false; }
    
    // Light mapping utilities - public for GlobalLightingManager
    Vec3 calculateWorldPositionFromLightMapUV(int faceIndex, float u, float v) const;  // Convert UV to world pos
    
    void buildCollisionMesh();
    bool checkRayCollision(const Vec3& rayOrigin, const Vec3& rayDirection, float maxDistance,
                           Vec3& hitPoint, Vec3& hitNormal) const;

   public:
    // Island context for inter-chunk culling
    void setIslandContext(uint32_t islandID, const Vec3& chunkCoord);
    

   private:
    std::array<uint8_t, VOLUME> voxels;
    VoxelMesh mesh;
    mutable std::mutex meshMutex;
    std::shared_ptr<CollisionMesh> collisionMesh;  // Thread-safe atomic access via getCollisionMesh/setCollisionMesh
    ChunkLightMaps lightMaps;  // NEW: Per-face light mapping data
    bool meshDirty = true;
    bool lightingDirty = true;  // NEW: Lighting needs recalculation
    
    // Island context for inter-chunk culling
    uint32_t m_islandID = 0;
    Vec3 m_chunkCoord{0, 0, 0};

    // NEW: Per-block-type model instance positions (for BlockRenderType::OBJ blocks)
    // Key: BlockID, Value: list of instance positions within this chunk
    std::unordered_map<uint8_t, std::vector<Vec3>> m_modelInstances;

    // Greedy meshing helpers
    void addGreedyQuad(float x, float y, float z, int face, int width, int height, uint8_t blockType);
    
    // Collision mesh building
    void buildCollisionMeshFromVertices();
    
    bool isVoxelSolid(int x, int y, int z) const;
    
    // Unified culling - works for intra-chunk AND inter-chunk
    bool isFaceExposed(int x, int y, int z, int face) const;
    
    // Simple meshing implementation
    void generateSimpleMesh();
    
    // Light mapping utilities
    float computeAmbientOcclusion(int x, int y, int z, int face) const;
    void generatePerFaceLightMaps();  // Generate separate light map per face direction
    bool performSunRaycast(const Vec3& rayStart, const Vec3& sunDirection, float maxDistance) const;  // Raycast for occlusion
    bool performLocalSunRaycast(const Vec3& rayStart, const Vec3& sunDirection, float maxDistance) const;  // Local chunk raycast for floating islands
    bool performInterIslandSunRaycast(const Vec3& rayStart, const Vec3& sunDirection, float maxDistance) const;  // Inter-island raycast for lighting
};
