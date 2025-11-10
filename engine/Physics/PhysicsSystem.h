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
    
    // Unified movement resolver with aggressive anti-stuck logic
    // Attempts to move entity from currentPos by velocity*deltaTime
    // Returns final position after collision resolution, step-up, and unstuck
    // Updates velocity to reflect actual movement (stops velocity on collision)
    // stepHeightRatio: fraction of entity height that can be climbed (default 0.4 = 40% of height)
    Vec3 resolveCapsuleMovement(const Vec3& currentPos, Vec3& velocity, float deltaTime, 
                                 float radius, float height, float stepHeightRatio = 0.4f);
    
    // Sphere collision detection (for simple objects like particles)
    // Sphere is defined by center position and radius
    bool checkSphereCollision(const Vec3& sphereCenter, float radius, Vec3& outNormal, const FloatingIsland** outIsland = nullptr);
    GroundInfo detectGroundSphere(const Vec3& sphereCenter, float radius, float rayMargin = 0.1f);
    
    // Sphere movement resolver (simpler than capsule, better for small particles)
    Vec3 resolveSphereMovement(const Vec3& currentPos, Vec3& velocity, float deltaTime, 
                               float radius, float stepHeightRatio = 0.4f);
    
    // Fluid-specific movement - allows phasing through terrain toward target (no blocking)
    Vec3 resolveFluidMovement(const Vec3& currentPos, Vec3& velocity, float deltaTime, float radius);
    
    // NOTE: For raycasting, use VoxelRaycaster::raycast() directly - it's faster and already handles rotated islands
    
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
    
    // Helper methods for sphere collision
    bool checkChunkSphereCollision(const VoxelChunk* chunk, const Vec3& sphereCenter, const Vec3& chunkWorldPos,
                                  Vec3& outNormal, float radius);
};

// Global g_physics removed - GameClient and GameServer now have separate physics instances
