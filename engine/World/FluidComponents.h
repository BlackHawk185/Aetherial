// FluidComponents.h - Shared fluid particle component definitions
// Used by both server (simulation) and client (rendering)
#pragma once

#include "../Math/Vec3.h"
#include <vector>

// Fluid particle states
enum class FluidState {
    SLEEPING,    // Stored as voxel in island
    ACTIVE,      // Simulated as world-space particle
    SETTLING     // Transitioning from active to sleeping
};

// Fluid particle component for ECS
// Server: Full simulation data
// Client: Render-only (position/velocity updated from network)
struct FluidParticleComponent {
    FluidState state = FluidState::ACTIVE;
    Vec3 velocity{0, 0, 0};
    Vec3 targetGridPos{0, 0, 0};      // Where particle wants to settle
    Vec3 pathfindingTarget{0, 0, 0};  // Final pathfinding target voxel center (island-relative)
    bool hasPathfindingTarget = false; // Whether we have a committed target
    float aliveTimer = 0.0f;          // How long particle has been active (for tracking/debugging)
    uint32_t sourceIslandID = 0;      // Island this particle came from
    Vec3 originalVoxelPos{0, 0, 0};   // Original sleeping position
    float tugStrength = 1.0f;         // Force needed to wake this particle
    int chainDepth = 0;                // Tug chain depth to prevent infinite cascades
    
    // Path following: Waypoints from floodfill BFS (island-relative voxel positions)
    std::vector<Vec3> pathWaypoints;   // Path to follow to reach target (empty = direct path)
    int currentWaypointIndex = 0;      // Current waypoint we're moving toward
    
    // Tug system: Water voxels this particle is watching (will activate if we move too far away)
    std::vector<Vec3> watchedWaterVoxels;  // Island-relative positions of nearby water voxels
};
