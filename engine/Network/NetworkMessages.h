#pragma once

#include <cstdint>
#include "../Math/Vec3.h"
#include "../World/ChunkConstants.h"

// Cross-platform structure packing
#ifdef _MSC_VER
    #pragma pack(push, 1)
    #define PACKED
#else
    #define PACKED __attribute__((packed))
#endif

// Network message types - use simple enum instead of enum class for network compatibility
enum NetworkMessageType : uint8_t {
    HELLO_WORLD = 1,
    PLAYER_MOVEMENT_REQUEST = 2,
    PLAYER_POSITION_UPDATE = 3,
    CHAT_MESSAGE = 4,
    WORLD_STATE = 5,
    COMPRESSED_ISLAND_DATA = 6,        // Legacy: Single chunk per island
    COMPRESSED_CHUNK_DATA = 7,         // NEW: Individual chunk with coordinates
    VOXEL_CHANGE_REQUEST = 8,          // Updated numbering
    VOXEL_CHANGE_UPDATE = 9,
    ENTITY_STATE_UPDATE = 10,
    PILOTING_INPUT = 11,
    FLUID_PARTICLE_SPAWN = 12,         // Server->Client: Spawn fluid particle
    FLUID_PARTICLE_UPDATE = 13,        // Server->Client: Bulk fluid particle state
    FLUID_PARTICLE_DESPAWN = 14        // Server->Client: Remove fluid particle
};

// Simple hello world message
struct PACKED HelloWorldMessage {
    uint8_t type = HELLO_WORLD;
    char message[32] = "Hello from server!";
};

// Player movement request from client to server
struct PACKED PlayerMovementRequest {
    uint8_t type = PLAYER_MOVEMENT_REQUEST;
    uint32_t sequenceNumber;
    Vec3 intendedPosition;
    Vec3 velocity;
    float deltaTime;
};

// Player position update from server to clients
struct PACKED PlayerPositionUpdate {
    uint8_t type = PLAYER_POSITION_UPDATE;
    uint32_t playerId;
    uint32_t sequenceNumber;
    Vec3 position;
    Vec3 velocity;
};

// Simple chat message
struct PACKED ChatMessage {
    uint8_t type = CHAT_MESSAGE;
    char message[256];
};

// Basic world state - simplified for initial implementation
struct PACKED WorldStateMessage {
    uint8_t type = WORLD_STATE;
    uint32_t numIslands;
    // For simplicity, include positions of first 3 islands
    Vec3 islandPositions[3];
    Vec3 playerSpawnPosition;
};

// Compressed island chunk data for efficient transmission
struct PACKED CompressedIslandHeader {
    uint8_t type = COMPRESSED_ISLAND_DATA;
    uint32_t islandID;
    Vec3 position;
    uint32_t originalSize;      // Uncompressed voxel data size (VoxelChunk::VOLUME bytes)
    uint32_t compressedSize;    // Size of the compressed data that follows
    // Compressed voxel data follows this header (variable length)
};

// NEW: Individual chunk data with coordinates for multi-chunk islands
struct PACKED CompressedChunkHeader {
    uint8_t type = COMPRESSED_CHUNK_DATA;
    uint32_t islandID;              // Which island this chunk belongs to
    Vec3 chunkCoord;                // Chunk coordinate within the island (0,0,0), (1,0,0), etc.
    Vec3 islandPosition;            // Island's physics center for positioning
    uint32_t originalSize;          // Uncompressed voxel data size (VoxelChunk::VOLUME bytes)
    uint32_t compressedSize;        // Size of the compressed data that follows
    // Compressed voxel data follows this header (variable length)
};

// Maximum size for compressed data (dynamically scales with CHUNK_SIZE)
// Worst case: ChunkConfig::MAX_CHUNK_DATA_SIZE uncompressed, LZ4 achieves 60-80% compression
// For 512³ = 134MB uncompressed, we reserve 32MB max for compressed data
constexpr uint32_t MAX_COMPRESSED_ISLAND_SIZE = 33554432; // 32MB max (scales with chunk size)
constexpr uint32_t MAX_COMPRESSED_CHUNK_SIZE = 33554432;  // 32MB max (scales with chunk size)

// Voxel change request from client to server
struct PACKED VoxelChangeRequest {
    uint8_t type = VOXEL_CHANGE_REQUEST;
    uint32_t sequenceNumber;
    uint32_t islandID;
    Vec3 localPos;
    uint8_t voxelType; // 0 = air (break), 1+ = place block
};

// Voxel change update from server to all clients
struct PACKED VoxelChangeUpdate {
    uint8_t type = VOXEL_CHANGE_UPDATE;
    uint32_t sequenceNumber;
    uint32_t islandID;
    Vec3 localPos;
    uint8_t voxelType;
    uint32_t authorPlayerId; // Player who made the change
};

// Unified entity state update (works for players, islands, NPCs, etc.)
struct PACKED EntityStateUpdate {
    uint8_t type = ENTITY_STATE_UPDATE;
    uint32_t sequenceNumber;
    uint32_t entityID;           // Unique entity identifier
    uint8_t entityType;          // 0=Player, 1=Island, 2=NPC, etc.
    Vec3 position;               // Current position
    Vec3 velocity;               // Current velocity
    Vec3 acceleration;           // For smooth prediction/interpolation
    Vec3 rotation;               // Current rotation (Euler angles in radians)
    Vec3 angularVelocity;        // Rotation speed (radians per second)
    uint32_t serverTimestamp;    // Server time for lag compensation
    uint8_t flags;               // Bit flags (isGrounded, needsCorrection, etc.)
};

// Piloting input from client to server
struct PACKED PilotingInputMessage {
    uint8_t type = PILOTING_INPUT;
    uint32_t sequenceNumber;
    uint32_t islandID;          // Which island the player is piloting
    float thrustY;              // Vertical thrust input (-1.0 to 1.0)
    float rotationPitch;        // Pitch rotation input (-1.0 to 1.0)
    float rotationYaw;          // Yaw rotation input (-1.0 to 1.0)
    float rotationRoll;         // Roll rotation input (-1.0 to 1.0)
};

// Fluid particle spawn notification from server to client
struct PACKED FluidParticleSpawnMessage {
    uint8_t type = FLUID_PARTICLE_SPAWN;
    uint32_t entityID;          // ECS entity ID for the particle
    uint32_t islandID;          // Source island
    Vec3 worldPosition;         // World-space position
    Vec3 velocity;              // Initial velocity
    Vec3 originalVoxelPos;      // Island-relative sleeping position
};

// Bulk fluid particle update (for many particles)
struct PACKED FluidParticleUpdateHeader {
    uint8_t type = FLUID_PARTICLE_UPDATE;
    uint32_t particleCount;     // Number of particles in this update
    // Followed by particleCount * FluidParticleState structs
};

struct PACKED FluidParticleState {
    uint32_t entityID;          // ECS entity ID
    Vec3 worldPosition;         // Current position
    Vec3 velocity;              // Current velocity
    uint8_t state;              // FluidState enum value (0=SLEEPING, 1=ACTIVE, 2=SETTLING)
};

// Fluid particle despawn (particle went back to sleep or was destroyed)
struct PACKED FluidParticleDespawnMessage {
    uint8_t type = FLUID_PARTICLE_DESPAWN;
    uint32_t entityID;          // ECS entity ID to remove
    uint32_t islandID;          // Island where it settled (if applicable)
    Vec3 settledVoxelPos;       // Final voxel position (if settled, otherwise 0,0,0)
    uint8_t shouldCreateVoxel;  // 1 if client should place water voxel, 0 otherwise
};

// Maximum particles per update message
constexpr uint32_t MAX_FLUID_PARTICLES_PER_UPDATE = 64;

// Restore packing
#ifdef _MSC_VER
    #pragma pack(pop)
#endif

#undef PACKED
