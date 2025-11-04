// PhysicsSystem.h - Basic collision detection system
#pragma once
#include "ECS/ECS.h"
#include "Math/Vec3.h"
#include <vector>

// Forward declarations
class IslandChunkSystem;
class VoxelChunk;
struct FloatingIsland;

// Ground detection information for player physics
struct GroundInfo
{
    bool isGrounded = false;              // Is the player standing on solid ground?
    uint32_t standingOnIslandID = 0;      // Which island is the player standing on?
    Vec3 groundNormal = Vec3(0, 1, 0);    // Surface normal of the ground
    Vec3 groundVelocity = Vec3(0, 0, 0);  // Velocity of the ground (for moving platforms)
    Vec3 groundContactPoint = Vec3(0, 0, 0); // Where exactly we're touching the ground
    float distanceToGround = 999.0f;      // Distance to ground (for coyote time, etc.)
};

// Simple collision detection system using voxel face culling
class PhysicsSystem
{
   public:
    PhysicsSystem();
    ~PhysicsSystem();

    bool initialize();
    void update(float deltaTime);
    void updateEntities(float deltaTime);
    void shutdown();

    // Capsule collision detection (for humanoid characters)
    // Capsule is defined by center position, radius, and height
    // Height is total height including hemispherical caps
    bool checkCapsuleCollision(const Vec3& capsuleCenter, float radius, float height, Vec3& outNormal, const FloatingIsland** outIsland = nullptr);
    GroundInfo detectGroundCapsule(const Vec3& capsuleCenter, float radius, float height, float rayMargin = 0.1f);
    
    // Raycasting
    bool checkRayCollision(const Vec3& rayOrigin, const Vec3& rayDirection, float maxDistance, Vec3& hitPoint, Vec3& hitNormal);
    
    // Island system integration
    void setIslandSystem(IslandChunkSystem* islandSystem) { m_islandSystem = islandSystem; }

    // Debug and testing methods
    void debugCollisionInfo(const Vec3& playerPos, float playerRadius = 0.5f);
    int getTotalCollisionFaces() const;

   private:
    IslandChunkSystem* m_islandSystem = nullptr;
    
    // Helper methods for capsule collision
    bool checkChunkCapsuleCollision(const VoxelChunk* chunk, const Vec3& capsuleCenter, const Vec3& chunkWorldPos,
                                   Vec3& outNormal, float radius, float height);
};

// Global g_physics removed - GameClient and GameServer now have separate physics instances
