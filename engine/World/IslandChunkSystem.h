// IslandChunkSystem.h - Floating island chunking system
#pragma once
#include <memory>
#include <unordered_map>
#include <map>
#include <vector>
#include <cmath>
#include <mutex>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "../Math/Vec3.h"
#include "VoxelChunk.h"
#include "BlockType.h"
#include "BiomeSystem.h"

// Sleeping fluid voxel data for tug system
struct SleepingFluidVoxel {
    Vec3 islandRelativePos;
    float tugStrength = 1.0f;
    float volume = 1.0f;              // Amount of fluid in this voxel (for partial filling)
};

// An Island is a collection of chunks that move together as one physics body
struct FloatingIsland
{
    Vec3 physicsCenter{0, 0, 0};                                     // Center of mass for physics
    Vec3 velocity{0, 0, 0};                                          // Island velocity for physics simulation
    Vec3 acceleration{0, 0, 0};                                      // Island acceleration (gravity, wind, etc.)
    Vec3 rotation{0, 0, 0};                                          // Euler angles (pitch, yaw, roll) in radians
    Vec3 angularVelocity{0, 0, 0};                                   // Rotation speed (radians per second)
    std::map<Vec3, std::unique_ptr<VoxelChunk>> chunks;              // Multi-chunk support: chunkCoord -> VoxelChunk
    uint32_t islandID;                                               // Unique island identifier
    bool needsPhysicsUpdate = false;
    bool isPiloted = false;                                          // Is a player currently piloting this entity?
    uint32_t pilotPlayerID = 0;                                      // Which player is piloting (0 = none)
    
    // Fluid system: Water voxels that have been "noticed" by particles and can be tugged awake
    std::unordered_map<uint64_t, SleepingFluidVoxel> sleepingFluidVoxels;  // Position hash -> voxel data

    // Helper functions for chunk coordinate conversion (operates on island-relative coordinates)
    static Vec3 islandPosToChunkCoord(const Vec3& islandRelativePos) {
        return Vec3(
            static_cast<int>(std::floor(islandRelativePos.x / VoxelChunk::SIZE)),
            static_cast<int>(std::floor(islandRelativePos.y / VoxelChunk::SIZE)),
            static_cast<int>(std::floor(islandRelativePos.z / VoxelChunk::SIZE))
        );
    }

    static Vec3 islandPosToLocalPos(const Vec3& islandRelativePos) {
        Vec3 chunkCoord = islandPosToChunkCoord(islandRelativePos);
        // Proper modulo for negative numbers
        int x = static_cast<int>(islandRelativePos.x) - (chunkCoord.x * VoxelChunk::SIZE);
        int y = static_cast<int>(islandRelativePos.y) - (chunkCoord.y * VoxelChunk::SIZE);
        int z = static_cast<int>(islandRelativePos.z) - (chunkCoord.z * VoxelChunk::SIZE);
        
        // Ensure result is always 0-15 for valid chunk-local coordinates
        if (x < 0) x += VoxelChunk::SIZE;
        if (y < 0) y += VoxelChunk::SIZE;
        if (z < 0) z += VoxelChunk::SIZE;
        
        return Vec3(x, y, z);
    }

    static Vec3 chunkCoordToWorldPos(const Vec3& chunkCoord) {
        return Vec3(
            chunkCoord.x * VoxelChunk::SIZE,
            chunkCoord.y * VoxelChunk::SIZE,
            chunkCoord.z * VoxelChunk::SIZE
        );
    }
    
    // Helper: Get chunk transform matrix (island transform + chunk offset)
    glm::mat4 getChunkTransform(const Vec3& chunkCoord) const {
        Vec3 chunkLocalPos = chunkCoordToWorldPos(chunkCoord);
        return getTransformMatrix() * 
            glm::translate(glm::mat4(1.0f), glm::vec3(chunkLocalPos.x, chunkLocalPos.y, chunkLocalPos.z));
    }
    
    // Get the complete transformation matrix for this island (position + rotation)
    // This is the single source of truth for how island-space transforms to world-space
    glm::mat4 getTransformMatrix() const {
        glm::mat4 transform = glm::mat4(1.0f);
        
        // Apply translation (position)
        transform = glm::translate(transform, glm::vec3(physicsCenter.x, physicsCenter.y, physicsCenter.z));
        
        // Apply rotation (Euler angles: yaw, pitch, roll)
        // Order matters: Y (yaw) -> X (pitch) -> Z (roll) for typical ship-like rotation
        transform = glm::rotate(transform, rotation.y, glm::vec3(0.0f, 1.0f, 0.0f)); // Yaw
        transform = glm::rotate(transform, rotation.x, glm::vec3(1.0f, 0.0f, 0.0f)); // Pitch
        transform = glm::rotate(transform, rotation.z, glm::vec3(0.0f, 0.0f, 1.0f)); // Roll
        
        return transform;
    }
    
    // Get the inverse transform matrix (world-space → island-local space)
    // Used for raycasting and collision detection against rotated islands
    glm::mat4 getInverseTransformMatrix() const {
        return glm::inverse(getTransformMatrix());
    }
    
    // Transform a world-space position to island-local space
    Vec3 worldToLocal(const Vec3& worldPos) const {
        glm::vec4 worldPoint(worldPos.x, worldPos.y, worldPos.z, 1.0f);
        glm::vec4 localPoint = getInverseTransformMatrix() * worldPoint;
        return Vec3(localPoint.x, localPoint.y, localPoint.z);
    }
    
    // Transform a world-space direction (no translation) to island-local space
    Vec3 worldDirToLocal(const Vec3& worldDir) const {
        glm::vec4 worldDirection(worldDir.x, worldDir.y, worldDir.z, 0.0f); // w=0 for direction
        glm::vec4 localDirection = getInverseTransformMatrix() * worldDirection;
        return Vec3(localDirection.x, localDirection.y, localDirection.z);
    }
    
    // Transform an island-local position to world space
    Vec3 localToWorld(const Vec3& localPos) const {
        glm::vec4 localPoint(localPos.x, localPos.y, localPos.z, 1.0f);
        glm::vec4 worldPoint = getTransformMatrix() * localPoint;
        return Vec3(worldPoint.x, worldPoint.y, worldPoint.z);
    }
    
    // Transform an island-local direction to world space
    Vec3 localDirToWorld(const Vec3& localDir) const {
        glm::vec4 localDirection(localDir.x, localDir.y, localDir.z, 0.0f); // w=0 for direction
        glm::vec4 worldDirection = getTransformMatrix() * localDirection;
        return Vec3(worldDirection.x, worldDirection.y, worldDirection.z);
    }
};

// A chunk within an island - has LOCAL coordinates relative to island center
struct IslandChunk
{
    Vec3 localPosition{0, 0, 0};   // Position relative to island center
    uint32_t islandID;             // Which island this chunk belongs to
    uint8_t* voxelData = nullptr;  // 32x32x32 voxel data
    bool needsRemesh = true;
    uint32_t meshVertexCount = 0;
    float* meshVertices = nullptr;

    // Get world position by combining island physics position + local offset
    Vec3 getWorldPosition(const FloatingIsland& /*island*/, const Vec3& islandPhysicsPos) const
    {
        return islandPhysicsPos + localPosition;
    }
};

// This system manages islands that can move through space
class IslandChunkSystem
{
   public:
    IslandChunkSystem();
    ~IslandChunkSystem();
    
    // Set whether this is a client-side system (chunks need GPU upload)
    void setIsClient(bool isClient) { m_isClient = isClient; }
    bool isClient() const { return m_isClient; }

    // Island management
    uint32_t createIsland(const Vec3& physicsCenter);
    uint32_t createIsland(const Vec3& physicsCenter, uint32_t forceIslandID);  // For network sync: force specific ID
    void destroyIsland(uint32_t islandID);
    FloatingIsland* getIsland(uint32_t islandID);
    const FloatingIsland* getIsland(uint32_t islandID) const;

    // Chunk management within islands
    void addChunkToIsland(uint32_t islandID, const Vec3& chunkCoord);
    void removeChunkFromIsland(uint32_t islandID, const Vec3& chunkCoord);
    VoxelChunk* getChunkFromIsland(uint32_t islandID, const Vec3& chunkCoord);

    // **ISLAND-CENTRIC VOXEL ACCESS** (Only way to access voxels)
    // Uses world coordinates - automatically converts to chunk + local coordinates
    // Get a specific voxel from an island using island-relative coordinates (for raycasting and collision detection)
    uint8_t getVoxelFromIsland(uint32_t islandID, const Vec3& islandRelativePosition) const;
    
    // **CLIENT-SIDE VOXEL MODIFICATION** (Triggers mesh generation)
    // Used by GameClient to modify voxels and update rendering meshes
    void setVoxelWithMesh(uint32_t islandID, const Vec3& islandRelativePosition, uint8_t voxelType);
    
    // **SERVER-ONLY VOXEL DATA MODIFICATION** (No mesh operations)
    // Used by GameServer to modify voxel data without triggering any rendering/mesh code
    // This directly modifies the voxel array and marks the chunk dirty, but never calls chunk->setVoxel()
    void setVoxelServerOnly(uint32_t islandID, const Vec3& islandRelativePosition, uint8_t voxelType);
    
    // **DYNAMIC VOXEL PLACEMENT** (Creates chunks as needed)
    // Uses island-relative coordinates - automatically creates chunks on grid-aligned boundaries
    void setVoxelWithAutoChunk(uint32_t islandID, const Vec3& islandRelativePos, uint8_t voxelType);
    
    // ID-based block methods (clean and efficient)
    void setBlockIDWithAutoChunk(uint32_t islandID, const Vec3& islandRelativePos, uint8_t blockID);
    uint8_t getBlockIDInIsland(uint32_t islandID, const Vec3& islandRelativePosition) const;

    // Physics integration
    void updateIslandPhysics(float deltaTime);

    // Player-relative chunk loading (for infinite worlds)
    void updatePlayerChunks(const Vec3& playerPosition);
    void setRenderDistance(int chunks)
    {
        m_renderDistance = chunks;
    }

    // Rendering interface
    void getAllChunks(std::vector<VoxelChunk*>& outChunks);
    void getVisibleChunks(const Vec3& viewPosition, std::vector<VoxelChunk*>& outChunks);
    void getVisibleChunksFrustum(const class Frustum& frustum, std::vector<VoxelChunk*>& outChunks);

    // **ORGANIC ISLAND GENERATION** (Creates chunks dynamically based on island shape)
    // This is now the primary and only island generation method
    void generateFloatingIslandOrganic(uint32_t islandID, uint32_t seed, float radius = 48.0f, BiomeType biome = BiomeType::GRASSLAND);

    // Water basin generation (called during island generation)
    void placeWaterBasins(uint32_t islandID, const BiomePalette& palette, uint32_t seed);
    void cullExposedWater(uint32_t islandID);

    // Island queries
    Vec3 getIslandCenter(uint32_t islandID) const;    // Get current physics center of island
    Vec3 getIslandVelocity(uint32_t islandID) const;  // Get current velocity of island
    const std::unordered_map<uint32_t, FloatingIsland>& getIslands() const { return m_islands; }

   private:
    std::unordered_map<uint32_t, FloatingIsland> m_islands;
    uint32_t m_nextIslandID = 1;
    int m_renderDistance = 8;
    mutable std::mutex m_islandsMutex;
    bool m_isClient = false;  // Whether this system is client-side (chunks need GPU upload)

    // Generate chunks around a center point (for infinite worlds)
    void generateChunksAroundPoint(const Vec3& center);
};

// Global island system
extern IslandChunkSystem g_islandSystem;

