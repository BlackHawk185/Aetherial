// VoxelChunk.cpp - 32x32x32 dynamic physics-enabled voxel chunks
#include "VoxelChunk.h"
#include "BlockType.h"

#include "../Time/DayNightController.h"  // For dynamic sun direction

#include <algorithm>
#include <cmath>
#include <cstring>  // For std::memset (greedy meshing optimization)
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <glad/gl.h>  // For OpenGL light map texture functions

#include "../Profiling/Profiler.h"
#include "IslandChunkSystem.h"  // For inter-island raycast queries

// Static island system pointer for inter-chunk queries
IslandChunkSystem* VoxelChunk::s_islandSystem = nullptr;

VoxelChunk::VoxelChunk()
{
    // Initialize voxel data to empty (0 = air)
    std::fill(voxels.begin(), voxels.end(), 0);
    meshDirty = true;
    
    // Initialize render mesh and collision mesh with empty shared_ptrs
    renderMesh = std::make_shared<VoxelMesh>();
    collisionMesh = std::make_shared<CollisionMesh>();
}

VoxelChunk::~VoxelChunk()
{
    // Clean up VBO resources if they exist
    // We'll handle this through the VBORenderer to avoid OpenGL context issues
}

uint8_t VoxelChunk::getVoxel(int x, int y, int z) const
{
    if (x < 0 || x >= SIZE || y < 0 || y >= SIZE || z < 0 || z >= SIZE)
        return 0;  // Out of bounds = air
    return voxels[x + y * SIZE + z * SIZE * SIZE];
}

void VoxelChunk::setVoxel(int x, int y, int z, uint8_t type)
{
    if (x < 0 || x >= SIZE || y < 0 || y >= SIZE || z < 0 || z >= SIZE)
        return;
    voxels[x + y * SIZE + z * SIZE * SIZE] = type;
    meshDirty = true;
}

void VoxelChunk::setRawVoxelData(const uint8_t* data, uint32_t size)
{
    if (size != VOLUME)
    {
        std::cerr << "⚠️  VoxelChunk::setRawVoxelData: Size mismatch! Expected " << VOLUME 
                  << " but got " << size << std::endl;
        return;
    }
    std::copy(data, data + size, voxels.begin());
    meshDirty = true;
}

void VoxelChunk::setIslandContext(uint32_t islandID, const Vec3& chunkCoord)
{
    m_islandID = islandID;
    m_chunkCoord = chunkCoord;
}

bool VoxelChunk::isVoxelSolid(int x, int y, int z) const
{
    uint8_t blockID = getVoxel(x, y, z);
    if (blockID == BlockID::AIR) return false;
    
    // Check if this is an OBJ-type block (instanced models, not meshed voxels)
    auto& registry = BlockTypeRegistry::getInstance();
    const BlockTypeInfo* blockInfo = registry.getBlockType(blockID);
    if (blockInfo && blockInfo->renderType == BlockRenderType::OBJ) {
        return false;  // OBJ blocks are not solid for meshing/collision purposes
    }
    
    return true;
}

void VoxelChunk::generateMesh(bool generateLighting)
{
    PROFILE_SCOPE("VoxelChunk::generateMesh");
    
    (void)generateLighting; // Parameter kept for API compatibility but unused (real-time lighting only)
    
    // Build into a new mesh (no locks needed - parallel-safe!)
    auto newMesh = std::make_shared<VoxelMesh>();
    auto newCollisionMesh = std::make_shared<CollisionMesh>();
    
    // Temporary storage for model instances during mesh generation
    std::unordered_map<uint8_t, std::vector<Vec3>> tempModelInstances;
    
    // Pre-scan for all OBJ-type blocks to create instance anchors (and ensure they are not meshed)
    auto& registry = BlockTypeRegistry::getInstance();
    for (int z = 0; z < SIZE; ++z) {
        for (int y = 0; y < SIZE; ++y) {
            for (int x = 0; x < SIZE; ++x) {
                uint8_t blockID = getVoxel(x, y, z);
                if (blockID == BlockID::AIR) continue;
                
                // Check if this is an OBJ-type block
                const BlockTypeInfo* blockInfo = registry.getBlockType(blockID);
                if (blockInfo && blockInfo->renderType == BlockRenderType::OBJ) {
                    // Position at block corner with X/Z centering, Y at ground level
                    Vec3 instancePos((float)x + 0.5f, (float)y, (float)z + 0.5f);
                    tempModelInstances[blockID].push_back(instancePos);
                }
            }
        }
    }

    // Simple mesh generation - one quad per exposed face
    generateSimpleMeshInto(newMesh->quads);
    
    // Build collision mesh from generated quads
    for (const auto& quad : newMesh->quads)
    {
        newCollisionMesh->faces.push_back({
            quad.position,
            quad.normal,
            quad.width,
            quad.height
        });
    }
    
    newMesh->needsUpdate = true;
    
    // ATOMIC SWAP - instant switch, no locks needed!
    setRenderMesh(newMesh);
    setCollisionMesh(newCollisionMesh);
    
    // Update model instances (protected by implicit synchronization)
    m_modelInstances = std::move(tempModelInstances);
    
    meshDirty = false;
}

void VoxelChunk::buildCollisionMesh()
{
    // Wrapper for backwards compatibility - regenerate entire mesh
    generateMesh(true);
}

bool VoxelChunk::checkRayCollision(const Vec3& rayOrigin, const Vec3& rayDirection,
                                   float maxDistance, Vec3& hitPoint, Vec3& hitNormal) const
{
    // Get current collision mesh (thread-safe atomic load)
    auto mesh = getCollisionMesh();
    if (!mesh)
        return false;
    
    // Simple ray-triangle intersection with collision faces
    float closestDistance = maxDistance;
    bool hit = false;

    for (const auto& face : mesh->faces)
    {
        // Ray-plane intersection
        float denom = rayDirection.dot(face.normal);
        if (abs(denom) < 1e-6f)
            continue;  // Ray parallel to plane

        Vec3 planeToRay = face.position - rayOrigin;
        float t = planeToRay.dot(face.normal) / denom;

        if (t < 0 || t > closestDistance)
            continue;

        // Check if intersection point is within face bounds (simple AABB check)
        Vec3 intersection = rayOrigin + rayDirection * t;
        Vec3 localPoint = intersection - face.position;

        // Determine which axes to check based on face normal
        bool withinBounds = true;
        if (abs(face.normal.x) > 0.5f)
        {  // X-facing face
            withinBounds = abs(localPoint.y) <= 0.5f && abs(localPoint.z) <= 0.5f;
        }
        else if (abs(face.normal.y) > 0.5f)
        {  // Y-facing face
            withinBounds = abs(localPoint.x) <= 0.5f && abs(localPoint.z) <= 0.5f;
        }
        else
        {  // Z-facing face
            withinBounds = abs(localPoint.x) <= 0.5f && abs(localPoint.y) <= 0.5f;
        }

        if (withinBounds)
        {
            closestDistance = t;
            hitPoint = intersection;
            hitNormal = face.normal;
            hit = true;
        }
    }

    return hit;
}

int VoxelChunk::calculateLOD(const Vec3& cameraPos) const
{
    // Simple distance-based LOD calculation
    Vec3 chunkCenter(SIZE * 0.5f, SIZE * 0.5f, SIZE * 0.5f);
    Vec3 distance = cameraPos - chunkCenter;
    float dist =
        std::sqrt(distance.x * distance.x + distance.y * distance.y + distance.z * distance.z);

    // LOD distances scale with chunk size (half-chunk and full-chunk)
    if (dist < SIZE * 0.5f)
        return 0;  // High detail (within half chunk)
    else if (dist < SIZE * 1.0f)
        return 1;  // Medium detail (within full chunk)
    else
        return 2;  // Low detail (beyond chunk)
}

bool VoxelChunk::shouldRender(const Vec3& cameraPos, float maxDistance) const
{
    Vec3 chunkCenter(SIZE * 0.5f, SIZE * 0.5f, SIZE * 0.5f);
    Vec3 distance = cameraPos - chunkCenter;
    float dist =
        std::sqrt(distance.x * distance.x + distance.y * distance.y + distance.z * distance.z);
    return dist <= maxDistance;
}

// ========================================
// UNIFIED CULLING - Works for intra-chunk AND inter-chunk
// ========================================

bool VoxelChunk::isFaceExposed(int x, int y, int z, int face) const
{
    // STANDARD OpenGL/Minecraft face ordering for sanity:
    // 0=-Y(bottom), 1=+Y(top), 2=-Z(back), 3=+Z(front), 4=-X(left), 5=+X(right)
    static const int dx[6] = { 0,  0,  0,  0, -1,  1};
    static const int dy[6] = {-1,  1,  0,  0,  0,  0};
    static const int dz[6] = { 0,  0, -1,  1,  0,  0};
    
    // Calculate neighbor position in LOCAL chunk space
    int nx = x + dx[face];
    int ny = y + dy[face];
    int nz = z + dz[face];
    
    // Fast path: neighbor is within this chunk
    if (nx >= 0 && nx < SIZE && ny >= 0 && ny < SIZE && nz >= 0 && nz < SIZE)
    {
        return !isVoxelSolid(nx, ny, nz);
    }
    
    // Slow path: neighbor is in adjacent chunk - query island system
    if (m_islandID == 0 || !s_islandSystem) return true;  // No island context = always exposed
    
    // Calculate which neighbor chunk we need
    Vec3 neighborChunkCoord = m_chunkCoord;
    int localX = nx, localY = ny, localZ = nz;
    
    if (nx < 0) { neighborChunkCoord.x -= 1; localX = SIZE - 1; }
    else if (nx >= SIZE) { neighborChunkCoord.x += 1; localX = 0; }
    
    if (ny < 0) { neighborChunkCoord.y -= 1; localY = SIZE - 1; }
    else if (ny >= SIZE) { neighborChunkCoord.y += 1; localY = 0; }
    
    if (nz < 0) { neighborChunkCoord.z -= 1; localZ = SIZE - 1; }
    else if (nz >= SIZE) { neighborChunkCoord.z += 1; localZ = 0; }
    
    // Query neighbor chunk
    VoxelChunk* neighborChunk = s_islandSystem->getChunkFromIsland(m_islandID, neighborChunkCoord);
    if (!neighborChunk) return true;  // Neighbor doesn't exist yet = exposed
    
    return !neighborChunk->isVoxelSolid(localX, localY, localZ);
}

// ========================================
// GREEDY MESHING IMPLEMENTATION
// ========================================
// Merges adjacent quads of the same block type into larger rectangles
// Reduces vertex count by 70-90% compared to simple meshing

void VoxelChunk::generateSimpleMeshInto(std::vector<QuadFace>& quads)
{
    PROFILE_SCOPE("VoxelChunk::generateSimpleMesh");
    
    // Stack-allocated mask buffer (reused 6×SIZE times per chunk)
    // SIZE×SIZE bytes (256×256 = 64 KB for 256³ chunks - safe for stack)
    // Eliminates 6×SIZE heap allocations + deallocations per chunk!
    uint8_t maskBuffer[SIZE * SIZE];
    
    // For each of the 6 face directions, perform greedy meshing
    for (int faceDir = 0; faceDir < 6; ++faceDir)
    {
        // Determine the axes based on face direction
        // Face 0=-Y, 1=+Y, 2=-Z, 3=+Z, 4=-X, 5=+X
        int du, dv, dn;  // Axis indices: u = width, v = height, n = depth (normal direction)
        int nu, nv, nn;  // Dimensions along each axis
        
        switch (faceDir)
        {
            case 0: case 1: // Y faces (top/bottom)
                du = 0; dv = 2; dn = 1;  // u=X, v=Z, n=Y
                nu = SIZE; nv = SIZE; nn = SIZE;
                break;
            case 2: case 3: // Z faces (front/back)
                du = 0; dv = 1; dn = 2;  // u=X, v=Y, n=Z
                nu = SIZE; nv = SIZE; nn = SIZE;
                break;
            case 4: case 5: // X faces (left/right)
                du = 2; dv = 1; dn = 0;  // u=Z, v=Y, n=X
                nu = SIZE; nv = SIZE; nn = SIZE;
                break;
            default:
                continue;
        }
        
        (void)du; (void)dv; (void)dn;  // Used indirectly via coordinate mapping
        
        // For each slice perpendicular to the normal direction
        for (int n = 0; n < nn; ++n)
        {
            // Clear the mask for this slice (SIMD-optimized memset)
            std::memset(maskBuffer, 0, nu * nv);
            
            for (int v = 0; v < nv; ++v)
            {
                for (int u = 0; u < nu; ++u)
                {
                    // Convert (u, v, n) to (x, y, z)
                    int x = 0, y = 0, z = 0;
                    if (faceDir == 0 || faceDir == 1) {      // Y faces
                        x = u; z = v; y = n;
                    } else if (faceDir == 2 || faceDir == 3) { // Z faces
                        x = u; y = v; z = n;
                    } else {                                  // X faces
                        z = u; y = v; x = n;
                    }
                    
                    // Check if voxel is solid and face is exposed
                    if (isVoxelSolid(x, y, z) && isFaceExposed(x, y, z, faceDir))
                    {
                        maskBuffer[u + v * nu] = getVoxel(x, y, z);
                    }
                }
            }
            
            // Greedy meshing: merge adjacent quads into rectangles
            for (int v = 0; v < nv; ++v)
            {
                for (int u = 0; u < nu; )
                {
                    uint8_t blockType = maskBuffer[u + v * nu];
                    if (blockType == 0)
                    {
                        ++u;
                        continue;
                    }
                    
                    // Find the width (u direction) of this quad
                    int width = 1;
                    while (u + width < nu && maskBuffer[u + width + v * nu] == blockType)
                    {
                        ++width;
                    }
                    
                    // Find the height (v direction) of this quad
                    int height = 1;
                    bool done = false;
                    while (v + height < nv && !done)
                    {
                        // Check if the entire row matches
                        for (int k = 0; k < width; ++k)
                        {
                            if (maskBuffer[u + k + (v + height) * nu] != blockType)
                            {
                                done = true;
                                break;
                            }
                        }
                        if (!done)
                        {
                            ++height;
                        }
                    }
                    
                    // Create a merged quad at (u, v) with dimensions (width x height)
                    // Convert back to (x, y, z) coordinates
                    int x = 0, y = 0, z = 0;
                    if (faceDir == 0 || faceDir == 1) {      // Y faces
                        x = u; z = v; y = n;
                    } else if (faceDir == 2 || faceDir == 3) { // Z faces
                        x = u; y = v; z = n;
                    } else {                                  // X faces
                        z = u; y = v; x = n;
                    }
                    
                    // Add the greedy quad (used for both rendering and collision)
                    addGreedyQuadTo(quads, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
                                  faceDir, width, height, blockType);
                    
                    // Clear the mask for the merged area
                    for (int h = 0; h < height; ++h)
                    {
                        for (int w = 0; w < width; ++w)
                        {
                            maskBuffer[u + w + (v + h) * nu] = 0;
                        }
                    }
                    
                    // Move to the next unprocessed position
                    u += width;
                }
            }
        }
    }
}

// Model instance management (for BlockRenderType::OBJ blocks)
const std::vector<Vec3>& VoxelChunk::getModelInstances(uint8_t blockID) const
{
    static const std::vector<Vec3> empty;
    auto it = m_modelInstances.find(blockID);
    if (it != m_modelInstances.end())
        return it->second;
    return empty;
}

// Greedy quad generation for merged rectangles
void VoxelChunk::addGreedyQuadTo(std::vector<QuadFace>& quads, float x, float y, float z, int face, int width, int height, uint8_t blockType)
{
    // Quad vertices based on face direction
    // Face ordering: 0=-Y, 1=+Y, 2=-Z, 3=+Z, 4=-X, 5=+X
    
    static const Vec3 normals[6] = {
        Vec3(0, -1, 0),  // -Y (bottom)
        Vec3(0, 1, 0),   // +Y (top)
        Vec3(0, 0, -1),  // -Z (back)
        Vec3(0, 0, 1),   // +Z (front)
        Vec3(-1, 0, 0),  // -X (left)
        Vec3(1, 0, 0)    // +X (right)
    };
    
    Vec3 normal = normals[face];
    float w = static_cast<float>(width);
    float h = static_cast<float>(height);
    
    // Add to QuadFace array for instanced rendering
    QuadFace quadFace;
    
    // Calculate center position based on face direction
    switch (face)
    {
        case 0: // -Y (bottom)
            quadFace.position = Vec3(x + w * 0.5f, y, z + h * 0.5f);
            break;
        case 1: // +Y (top)
            quadFace.position = Vec3(x + w * 0.5f, y + 1, z + h * 0.5f);
            break;
        case 2: // -Z (back)
            quadFace.position = Vec3(x + w * 0.5f, y + h * 0.5f, z);
            break;
        case 3: // +Z (front)
            quadFace.position = Vec3(x + w * 0.5f, y + h * 0.5f, z + 1);
            break;
        case 4: // -X (left)
            quadFace.position = Vec3(x, y + h * 0.5f, z + w * 0.5f);
            break;
        case 5: // +X (right)
            quadFace.position = Vec3(x + 1, y + h * 0.5f, z + w * 0.5f);
            break;
    }
    
    quadFace.normal = normal;
    quadFace.width = w;
    quadFace.height = h;
    quadFace.blockType = blockType;
    quadFace.faceDir = static_cast<uint8_t>(face);
    quadFace.padding = 0;
    
    quads.push_back(quadFace);
}

