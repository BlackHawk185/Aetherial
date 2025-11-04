// VoxelChunk.cpp - 256x256x256 dynamic physics-enabled voxel chunks
#include "VoxelChunk.h"
#include "BlockType.h"

#include "../Time/DayNightController.h"  // For dynamic sun direction

#include <algorithm>
#include <cmath>
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
    
    uint8_t oldType = voxels[x + y * SIZE + z * SIZE * SIZE];
    if (oldType == type) return; // No change
    
    voxels[x + y * SIZE + z * SIZE * SIZE] = type;
    
    // Only use incremental updates if enabled (disabled during world generation)
    if (m_incrementalUpdatesEnabled) {
        // Use incremental updates for runtime block changes
        if (oldType == BlockID::AIR && type != BlockID::AIR) {
            // Block placed - add its quads and update neighbors
            addBlockQuads(x, y, z, type);
            updateNeighborQuads(x, y, z, true);
        } else if (oldType != BlockID::AIR && type == BlockID::AIR) {
            // Block removed - remove its quads and update neighbors
            removeBlockQuads(x, y, z);
            updateNeighborQuads(x, y, z, false);
        } else {
            // Block type changed (rare) - remove old, add new
            removeBlockQuads(x, y, z);
            addBlockQuads(x, y, z, type);
            // No neighbor update needed - faces stay the same
        }
        
        // EVENT-DRIVEN: Immediately notify renderer of mesh changes (zero latency)
        if (m_meshUpdateCallback) {
            m_meshUpdateCallback(this);
        }
    }
    
    meshDirty = true; // Keep for backwards compatibility with generateMesh()
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
    
    auto t_start = std::chrono::high_resolution_clock::now();
    
    (void)generateLighting; // Parameter kept for API compatibility but unused (real-time lighting only)
    
    // Build into a new mesh (no locks needed - parallel-safe!)
    auto newMesh = std::make_shared<VoxelMesh>();
    auto newCollisionMesh = std::make_shared<CollisionMesh>();
    
    // Temporary storage for model instances during mesh generation
    std::unordered_map<uint8_t, std::vector<Vec3>> tempModelInstances;
    
    auto t_prescan_start = std::chrono::high_resolution_clock::now();
    
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
    
    auto t_prescan_end = std::chrono::high_resolution_clock::now();
    auto t_mesh_start = std::chrono::high_resolution_clock::now();

    // Generate quads into the new mesh (core implementation in generateSimpleMeshInto)
    generateSimpleMeshInto(newMesh->quads);
    
    auto t_mesh_end = std::chrono::high_resolution_clock::now();
    auto t_collision_start = std::chrono::high_resolution_clock::now();
    
    // Build collision mesh from generated quads (trivial copy)
    for (const auto& quad : newMesh->quads)
    {
        newCollisionMesh->faces.push_back({
            quad.position,
            quad.normal,
            quad.width,
            quad.height
        });
    }
    
    auto t_collision_end = std::chrono::high_resolution_clock::now();
    
    // Build quad lookup map for incremental updates
    m_quadLookup.clear();
    for (size_t i = 0; i < newMesh->quads.size(); ++i) {
        const auto& quad = newMesh->quads[i];
        // Reverse-engineer voxel position from quad center position
        int x = static_cast<int>(quad.position.x);
        int y = static_cast<int>(quad.position.y);
        int z = static_cast<int>(quad.position.z);
        int face = quad.faceDir;
        
        // Adjust for face offset (quad center is offset from voxel corner)
        if (face == 1) y--;       // Top face (+Y)
        else if (face == 3) z--;  // Front face (+Z)
        else if (face == 5) x--;  // Right face (+X)
        
        uint64_t key = makeQuadKey(x, y, z, face);
        m_quadLookup[key] = i;
    }
    
    // DIRECT ASSIGNMENT - fast, no atomic overhead!
    renderMesh = newMesh;
    collisionMesh = newCollisionMesh;
    
    // Update model instances (protected by implicit synchronization)
    m_modelInstances = std::move(tempModelInstances);
    
    meshDirty = false;
    
    // Enable incremental updates after first full mesh generation
    m_incrementalUpdatesEnabled = true;
    
    auto t_end = std::chrono::high_resolution_clock::now();
    
    // Performance tracking
    auto prescan_ms = std::chrono::duration<double, std::milli>(t_prescan_end - t_prescan_start).count();
    auto mesh_ms = std::chrono::duration<double, std::milli>(t_mesh_end - t_mesh_start).count();
    auto collision_ms = std::chrono::duration<double, std::milli>(t_collision_end - t_collision_start).count();
    auto total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    
    std::cout << "🔧 MESH GEN: Total=" << total_ms << "ms (Prescan=" << prescan_ms 
              << "ms, Mesh=" << mesh_ms << "ms, Collision=" << collision_ms 
              << "ms) Quads=" << newMesh->quads.size() << std::endl;
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

// INTRA-CHUNK CULLING ONLY - Inter-chunk culling removed for performance
// Boundary faces are always rendered (negligible visual difference, massive speed gain)
bool VoxelChunk::isFaceExposed(int x, int y, int z, int face) const
{
    static const int dx[6] = { 0,  0,  0,  0, -1,  1};
    static const int dy[6] = {-1,  1,  0,  0,  0,  0};
    static const int dz[6] = { 0,  0, -1,  1,  0,  0};
    
    int nx = x + dx[face];
    int ny = y + dy[face];
    int nz = z + dz[face];
    
    // Only check within this chunk - out of bounds = exposed
    if (nx < 0 || nx >= SIZE || ny < 0 || ny >= SIZE || nz < 0 || nz >= SIZE)
    {
        return true;  // Chunk boundary = always render face
    }
    
    return !isVoxelSolid(nx, ny, nz);
}

// ========================================
// SIMPLE MESHING - One quad per exposed face
// ========================================

void VoxelChunk::generateSimpleMeshInto(std::vector<QuadFace>& quads)
{
    PROFILE_SCOPE("VoxelChunk::generateSimpleMesh");
    
    auto t_start = std::chrono::high_resolution_clock::now();
    
    size_t totalQuadsGenerated = 0;
    
    // Iterate through all voxels once
    for (int z = 0; z < SIZE; ++z)
    {
        for (int y = 0; y < SIZE; ++y)
        {
            for (int x = 0; x < SIZE; ++x)
            {
                if (!isVoxelSolid(x, y, z))
                    continue;
                
                uint8_t blockType = getVoxel(x, y, z);
                
                // Check all 6 faces and add 1x1 quads for exposed faces
                for (int face = 0; face < 6; ++face)
                {
                    if (isFaceExposed(x, y, z, face))
                    {
                        addQuad(quads, static_cast<float>(x), static_cast<float>(y), 
                                static_cast<float>(z), face, 1, 1, blockType);
                        totalQuadsGenerated++;
                    }
                }
            }
        }
    }
    
    auto t_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    const char* side = m_isClientChunk ? "CLIENT" : "SERVER";
    std::cout << "[" << side << "] SIMPLE MESH: " << total_ms << "ms, " << totalQuadsGenerated << " quads" << std::endl;
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

// Add a single quad to the mesh (width/height support for future optimizations)
void VoxelChunk::addQuad(std::vector<QuadFace>& quads, float x, float y, float z, int face, int width, int height, uint8_t blockType)
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

// ========================================
// INCREMENTAL QUAD MANIPULATION
// ========================================

// Add quads for a single block's exposed faces
void VoxelChunk::addBlockQuads(int x, int y, int z, uint8_t blockType)
{
    // Skip air blocks
    if (blockType == BlockID::AIR) return;
    
    // Skip OBJ-type blocks (they use instanced models, not voxel quads)
    auto& registry = BlockTypeRegistry::getInstance();
    const BlockTypeInfo* blockInfo = registry.getBlockType(blockType);
    if (blockInfo && blockInfo->renderType == BlockRenderType::OBJ) {
        return;
    }
    
    // Get mutable mesh (create if needed)
    if (!renderMesh) {
        renderMesh = std::make_shared<VoxelMesh>();
    }
    
    // Add quads for all exposed faces
    for (int face = 0; face < 6; ++face)
    {
        if (isFaceExposed(x, y, z, face))
        {
            // Store quad index in lookup map
            uint64_t key = makeQuadKey(x, y, z, face);
            size_t quadIndex = renderMesh->quads.size();
            m_quadLookup[key] = quadIndex;
            
            // Add quad to mesh
            addQuad(renderMesh->quads, static_cast<float>(x), static_cast<float>(y), 
                   static_cast<float>(z), face, 1, 1, blockType);
        }
    }
    
    // Update collision mesh (simple copy)
    auto collMesh = std::make_shared<CollisionMesh>();
    for (const auto& quad : renderMesh->quads) {
        collMesh->faces.push_back({quad.position, quad.normal, quad.width, quad.height});
    }
    collisionMesh = collMesh;
}

// Remove all quads for a block
void VoxelChunk::removeBlockQuads(int x, int y, int z)
{
    if (!renderMesh || renderMesh->quads.empty()) return;
    
    // Find and remove all 6 possible face quads for this block
    std::vector<size_t> indicesToRemove;
    for (int face = 0; face < 6; ++face)
    {
        uint64_t key = makeQuadKey(x, y, z, face);
        auto it = m_quadLookup.find(key);
        if (it != m_quadLookup.end()) {
            indicesToRemove.push_back(it->second);
            m_quadLookup.erase(it);
        }
    }
    
    // Sort in reverse order to avoid index invalidation during removal
    std::sort(indicesToRemove.rbegin(), indicesToRemove.rend());
    
    // Remove quads (swap-and-pop for efficiency)
    for (size_t idx : indicesToRemove) {
        if (idx < renderMesh->quads.size()) {
            // Swap with last element
            if (idx != renderMesh->quads.size() - 1) {
                renderMesh->quads[idx] = renderMesh->quads.back();
                // Update lookup map for the swapped quad
                // TODO: Need to reverse-lookup the swapped quad to update its index
            }
            renderMesh->quads.pop_back();
        }
    }
    
    // Rebuild lookup map (safer than tracking swaps)
    m_quadLookup.clear();
    for (size_t i = 0; i < renderMesh->quads.size(); ++i) {
        const auto& quad = renderMesh->quads[i];
        // Reverse-engineer position from quad data
        int qx = static_cast<int>(quad.position.x);
        int qy = static_cast<int>(quad.position.y);
        int qz = static_cast<int>(quad.position.z);
        int qface = quad.faceDir;
        
        // Adjust for face offset
        if (qface == 1) qy--;       // Top face
        else if (qface == 3) qz--;  // Front face
        else if (qface == 5) qx--;  // Right face
        
        uint64_t key = makeQuadKey(qx, qy, qz, qface);
        m_quadLookup[key] = i;
    }
    
    // Update collision mesh
    auto collMesh = std::make_shared<CollisionMesh>();
    for (const auto& quad : renderMesh->quads) {
        collMesh->faces.push_back({quad.position, quad.normal, quad.width, quad.height});
    }
    collisionMesh = collMesh;
}

// Update neighbor quads when a block is added or removed
void VoxelChunk::updateNeighborQuads(int x, int y, int z, bool blockWasAdded)
{
    // Face offsets and their opposite faces
    static const int dx[6] = { 0,  0,  0,  0, -1,  1};
    static const int dy[6] = {-1,  1,  0,  0,  0,  0};
    static const int dz[6] = { 0,  0, -1,  1,  0,  0};
    static const int oppositeFace[6] = {1, 0, 3, 2, 5, 4};
    
    for (int face = 0; face < 6; ++face)
    {
        int nx = x + dx[face];
        int ny = y + dy[face];
        int nz = z + dz[face];
        
        // Check if neighbor is within chunk bounds
        if (nx < 0 || nx >= SIZE || ny < 0 || ny >= SIZE || nz < 0 || nz >= SIZE) {
            // TODO: Cross-chunk updates - notify neighboring chunk
            continue;
        }
        
        uint8_t neighborBlock = getVoxel(nx, ny, nz);
        if (neighborBlock == BlockID::AIR) continue;
        
        // Skip OBJ blocks
        auto& registry = BlockTypeRegistry::getInstance();
        const BlockTypeInfo* blockInfo = registry.getBlockType(neighborBlock);
        if (blockInfo && blockInfo->renderType == BlockRenderType::OBJ) {
            continue;
        }
        
        int neighborFace = oppositeFace[face];
        uint64_t key = makeQuadKey(nx, ny, nz, neighborFace);
        
        if (blockWasAdded) {
            // A block was placed - neighbor face is now hidden, remove it
            auto it = m_quadLookup.find(key);
            if (it != m_quadLookup.end()) {
                // Remove this specific quad (inefficient, but correct)
                removeBlockQuads(nx, ny, nz);
                addBlockQuads(nx, ny, nz, neighborBlock);
            }
        } else {
            // A block was removed - neighbor face is now exposed, add it
            if (m_quadLookup.find(key) == m_quadLookup.end()) {
                // Quad doesn't exist - add it
                if (renderMesh) {
                    size_t quadIndex = renderMesh->quads.size();
                    m_quadLookup[key] = quadIndex;
                    addQuad(renderMesh->quads, static_cast<float>(nx), static_cast<float>(ny),
                           static_cast<float>(nz), neighborFace, 1, 1, neighborBlock);
                }
            }
        }
    }
    
    // Update collision mesh after all neighbor updates
    if (renderMesh) {
        auto collMesh = std::make_shared<CollisionMesh>();
        for (const auto& quad : renderMesh->quads) {
            collMesh->faces.push_back({quad.position, quad.normal, quad.width, quad.height});
        }
        collisionMesh = collMesh;
    }
}




