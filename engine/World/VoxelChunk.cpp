// VoxelChunk.cpp - 256x256x256 dynamic physics-enabled voxel chunks
#include "VoxelChunk.h"
#include "BlockType.h"

#include "../Time/DayNightController.h"  // For dynamic sun direction
#include "../Rendering/GPUMeshQueue.h"  // For region-based remeshing queue (greedy meshing)
#include "../Rendering/InstancedQuadRenderer.h"  // For GPU upload

#include <algorithm>
#include <cmath>
#include <iostream>
#include <chrono>
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
    
    // Initialize render mesh with empty shared_ptr
    renderMesh = std::make_shared<VoxelMesh>();
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
    
    int voxelIdx = x + y * SIZE + z * SIZE * SIZE;
    uint8_t oldType = voxels[voxelIdx];
    if (oldType == type) return; // No change
    
    voxels[voxelIdx] = type;
    
    // Track OBJ-type blocks (instanced models like grass, water)
    auto& registry = BlockTypeRegistry::getInstance();
    const BlockTypeInfo* oldBlockInfo = registry.getBlockType(oldType);
    const BlockTypeInfo* newBlockInfo = registry.getBlockType(type);
    
    // Remove old OBJ instance if it was an OBJ block
    if (oldBlockInfo && oldBlockInfo->renderType == BlockRenderType::OBJ) {
        auto it = m_modelInstances.find(oldType);
        if (it != m_modelInstances.end()) {
            auto& instances = it->second;
            Vec3 pos(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
            instances.erase(std::remove(instances.begin(), instances.end(), pos), instances.end());
        }
    }
    
    // Add new OBJ instance if it's an OBJ block
    if (newBlockInfo && newBlockInfo->renderType == BlockRenderType::OBJ) {
        Vec3 pos(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        m_modelInstances[type].push_back(pos);
    }
    
    // CLIENT ONLY: Use explosion system for instant mesh updates
    if (m_isClientChunk)
    {
        auto mesh = getRenderMesh();
        if (!mesh)
        {
            // No mesh yet - queue for initial generation
            if (g_greedyMeshQueue) {
                g_greedyMeshQueue->queueChunkMesh(this);
            }
            return;
        }
        
        // Find and explode ALL quads covering this voxel (check all 6 face directions)
        for (int face = 0; face < 6; ++face)
        {
            uint32_t key = voxelIdx * 6 + face;
            auto it = mesh->voxelFaceToQuadIndex.find(key);
            if (it != mesh->voxelFaceToQuadIndex.end())
            {
                explodeQuad(it->second);
                mesh->voxelFaceToQuadIndex.erase(it);
            }
        }
        
        // If placing a block, add new faces
        if (type != 0)  // 0 = air
        {
            addSimpleFacesForVoxel(x, y, z);
        }
        
        // CRITICAL: Update neighboring voxels - they may now have exposed faces
        // When we break/place a block, the 6 adjacent voxels may need new faces added
        static const int dx[6] = { 0,  0,  0,  0, -1,  1};
        static const int dy[6] = {-1,  1,  0,  0,  0,  0};
        static const int dz[6] = { 0,  0, -1,  1,  0,  0};
        
        for (int face = 0; face < 6; ++face)
        {
            int nx = x + dx[face];
            int ny = y + dy[face];
            int nz = z + dz[face];
            
            // Skip out of bounds
            if (nx < 0 || nx >= SIZE || ny < 0 || ny >= SIZE || nz < 0 || nz >= SIZE)
                continue;
            
            // If neighbor is solid, it might need a new face toward this voxel
            if (isVoxelSolid(nx, ny, nz))
            {
                // Opposite face direction (face toward the modified voxel)
                int oppositeFace = face ^ 1; // 0<->1, 2<->3, 4<->5
                
                // Check if that face is now exposed
                if (isFaceExposed(nx, ny, nz, oppositeFace))
                {
                    int neighborIdx = nx + ny * SIZE + nz * SIZE * SIZE;
                    
                    // Explode the quad on this specific face direction for the neighbor
                    if (!mesh->isExploded[neighborIdx])
                    {
                        uint32_t neighborKey = neighborIdx * 6 + oppositeFace;
                        auto it = mesh->voxelFaceToQuadIndex.find(neighborKey);
                        if (it != mesh->voxelFaceToQuadIndex.end())
                        {
                            explodeQuad(it->second);
                            mesh->voxelFaceToQuadIndex.erase(it);
                        }
                    }
                    
                    // Add a 1x1 face for this exposed direction
                    uint16_t newQuadIdx = static_cast<uint16_t>(mesh->quads.size());
                    uint8_t neighborBlockType = getVoxel(nx, ny, nz);
                    addQuad(mesh->quads, static_cast<float>(nx), static_cast<float>(ny),
                           static_cast<float>(nz), oppositeFace, 1, 1, neighborBlockType);
                    
                    // Add to tracking map (voxelIdx * 6 + faceDir)
                    uint32_t neighborKey = neighborIdx * 6 + oppositeFace;
                    mesh->voxelFaceToQuadIndex[neighborKey] = newQuadIdx;
                    mesh->isExploded[neighborIdx] = true;
                }
            }
        }
        
        mesh->needsGPUUpload = true;
        
        // Upload to GPU immediately
        uploadMeshToGPU();
    }
}

void VoxelChunk::setVoxelDataDirect(int x, int y, int z, uint8_t type)
{
    // SERVER-ONLY: Direct voxel data modification without any mesh operations
    if (x < 0 || x >= SIZE || y < 0 || y >= SIZE || z < 0 || z >= SIZE)
        return;
    
    voxels[x + y * SIZE + z * SIZE * SIZE] = type;
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
    
    // Scan for OBJ-type blocks (instanced models) and populate m_modelInstances
    m_modelInstances.clear();
    auto& registry = BlockTypeRegistry::getInstance();
    
    for (int z = 0; z < SIZE; ++z) {
        for (int y = 0; y < SIZE; ++y) {
            for (int x = 0; x < SIZE; ++x) {
                uint8_t blockType = getVoxel(x, y, z);
                if (blockType == BlockID::AIR) continue;
                
                const BlockTypeInfo* blockInfo = registry.getBlockType(blockType);
                if (blockInfo && blockInfo->renderType == BlockRenderType::OBJ) {
                    Vec3 pos(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                    m_modelInstances[blockType].push_back(pos);
                }
            }
        }
    }
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
    
    // Initialize mesh if needed
    if (!renderMesh) {
        renderMesh = std::make_shared<VoxelMesh>();
    }
    
    // Queue entire chunk for meshing
    if (g_greedyMeshQueue) {
        g_greedyMeshQueue->queueChunkMesh(this);
    }
}

// Greedy meshing for a single face direction
void VoxelChunk::greedyMeshFace(std::vector<QuadFace>& quads, int face)
{
    // Face directions: 0=-Y, 1=+Y, 2=-Z, 3=+Z, 4=-X, 5=+X
    // Process each face direction with proper axis mapping
    
    // Simpler approach: iterate through each voxel position and build greedy rectangles
    // Use a visited mask to track which faces have been merged
    std::vector<bool> visited(SIZE * SIZE * SIZE, false);
    
    for (int z = 0; z < SIZE; ++z)
    {
        for (int y = 0; y < SIZE; ++y)
        {
            for (int x = 0; x < SIZE; ++x)
            {
                if (!isVoxelSolid(x, y, z)) continue;
                if (!isFaceExposed(x, y, z, face)) continue;
                
                int idx = x + y * SIZE + z * SIZE * SIZE;
                if (visited[idx]) continue;
                
                uint8_t blockType = getVoxel(x, y, z);
                
                // Determine merge dimensions based on face direction
                int width = 1;
                int height = 1;
                
                // Expand width (horizontal for this face)
                if (face == 0 || face == 1) // Y faces: expand in X direction
                {
                    while (x + width < SIZE)
                    {
                        int checkIdx = (x + width) + y * SIZE + z * SIZE * SIZE;
                        if (visited[checkIdx]) break;
                        if (!isVoxelSolid(x + width, y, z)) break;
                        if (getVoxel(x + width, y, z) != blockType) break;
                        if (!isFaceExposed(x + width, y, z, face)) break;
                        ++width;
                    }
                    
                    // Expand height (Z direction)
                    bool canExpand = true;
                    while (z + height < SIZE && canExpand)
                    {
                        for (int dx = 0; dx < width; ++dx)
                        {
                            int checkIdx = (x + dx) + y * SIZE + (z + height) * SIZE * SIZE;
                            if (visited[checkIdx] ||
                                !isVoxelSolid(x + dx, y, z + height) ||
                                getVoxel(x + dx, y, z + height) != blockType ||
                                !isFaceExposed(x + dx, y, z + height, face))
                            {
                                canExpand = false;
                                break;
                            }
                        }
                        if (canExpand) ++height;
                    }
                }
                else if (face == 2 || face == 3) // Z faces: expand in X direction
                {
                    while (x + width < SIZE)
                    {
                        int checkIdx = (x + width) + y * SIZE + z * SIZE * SIZE;
                        if (visited[checkIdx]) break;
                        if (!isVoxelSolid(x + width, y, z)) break;
                        if (getVoxel(x + width, y, z) != blockType) break;
                        if (!isFaceExposed(x + width, y, z, face)) break;
                        ++width;
                    }
                    
                    // Expand height (Y direction)
                    bool canExpand = true;
                    while (y + height < SIZE && canExpand)
                    {
                        for (int dx = 0; dx < width; ++dx)
                        {
                            int checkIdx = (x + dx) + (y + height) * SIZE + z * SIZE * SIZE;
                            if (visited[checkIdx] ||
                                !isVoxelSolid(x + dx, y + height, z) ||
                                getVoxel(x + dx, y + height, z) != blockType ||
                                !isFaceExposed(x + dx, y + height, z, face))
                            {
                                canExpand = false;
                                break;
                            }
                        }
                        if (canExpand) ++height;
                    }
                }
                else // X faces: expand in Z direction
                {
                    while (z + width < SIZE)
                    {
                        int checkIdx = x + y * SIZE + (z + width) * SIZE * SIZE;
                        if (visited[checkIdx]) break;
                        if (!isVoxelSolid(x, y, z + width)) break;
                        if (getVoxel(x, y, z + width) != blockType) break;
                        if (!isFaceExposed(x, y, z + width, face)) break;
                        ++width;
                    }
                    
                    // Expand height (Y direction)
                    bool canExpand = true;
                    while (y + height < SIZE && canExpand)
                    {
                        for (int dz = 0; dz < width; ++dz)
                        {
                            int checkIdx = x + (y + height) * SIZE + (z + dz) * SIZE * SIZE;
                            if (visited[checkIdx] ||
                                !isVoxelSolid(x, y + height, z + dz) ||
                                getVoxel(x, y + height, z + dz) != blockType ||
                                !isFaceExposed(x, y + height, z + dz, face))
                            {
                                canExpand = false;
                                break;
                            }
                        }
                        if (canExpand) ++height;
                    }
                }
                
                // Mark merged area as visited
                if (face == 0 || face == 1) // Y faces
                {
                    for (int dz = 0; dz < height; ++dz)
                        for (int dx = 0; dx < width; ++dx)
                            visited[(x + dx) + y * SIZE + (z + dz) * SIZE * SIZE] = true;
                }
                else if (face == 2 || face == 3) // Z faces
                {
                    for (int dy = 0; dy < height; ++dy)
                        for (int dx = 0; dx < width; ++dx)
                            visited[(x + dx) + (y + dy) * SIZE + z * SIZE * SIZE] = true;
                }
                else // X faces
                {
                    for (int dy = 0; dy < height; ++dy)
                        for (int dz = 0; dz < width; ++dz)
                            visited[x + (y + dy) * SIZE + (z + dz) * SIZE * SIZE] = true;
                }
                
                // Add merged quad (quad index will be assigned after all faces are generated)
                addQuad(quads, static_cast<float>(x), static_cast<float>(y),
                       static_cast<float>(z), face, width, height, blockType);
            }
        }
    }
}

// Generate mesh for entire chunk - GREEDY MESHING with voxel-to-quad tracking
std::vector<QuadFace> VoxelChunk::generateFullChunkMesh()
{
    PROFILE_SCOPE("VoxelChunk::generateFullChunkMesh");
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    std::vector<QuadFace> quads;
    quads.reserve(15000); // Pre-allocate for greedy meshing
    
    // Quick check: is chunk completely empty?
    bool hasAnyVoxel = false;
    for (int i = 0; i < VOLUME && !hasAnyVoxel; ++i) {
        if (voxels[i] != 0) hasAnyVoxel = true;
    }
    
    if (!hasAnyVoxel) {
        return quads;
    }
    
    // GREEDY MESHING - Process each face direction separately
    for (int face = 0; face < 6; ++face)
    {
        greedyMeshFace(quads, face);
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    (void)duration; // Suppress unused warning

    return quads;
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
// EXPLOSION SYSTEM - Direct quad manipulation
// ========================================

// Explode a greedy quad into individual 1x1 faces
void VoxelChunk::explodeQuad(uint16_t quadIndex)
{
    auto mesh = getRenderMesh();
    if (!mesh || quadIndex >= mesh->quads.size()) return;
    
    QuadFace& quad = mesh->quads[quadIndex];
    
    int width = static_cast<int>(quad.width);
    int height = static_cast<int>(quad.height);
    int face = quad.faceDir;
    uint8_t blockType = quad.blockType;
    
    // Zero out the old quad immediately so it doesn't render
    quad.width = 0;
    quad.height = 0;
    
    // Calculate base corner position from center position (reverse of addQuad)
    int baseX, baseY, baseZ;
    
    switch (face)
    {
        case 0: // -Y (bottom): width=X, height=Z
            baseX = static_cast<int>(quad.position.x - width * 0.5f);
            baseY = static_cast<int>(quad.position.y);
            baseZ = static_cast<int>(quad.position.z - height * 0.5f);
            break;
        case 1: // +Y (top): width=X, height=Z
            baseX = static_cast<int>(quad.position.x - width * 0.5f);
            baseY = static_cast<int>(quad.position.y - 1);
            baseZ = static_cast<int>(quad.position.z - height * 0.5f);
            break;
        case 2: // -Z (back): width=X, height=Y
            baseX = static_cast<int>(quad.position.x - width * 0.5f);
            baseY = static_cast<int>(quad.position.y - height * 0.5f);
            baseZ = static_cast<int>(quad.position.z);
            break;
        case 3: // +Z (front): width=X, height=Y
            baseX = static_cast<int>(quad.position.x - width * 0.5f);
            baseY = static_cast<int>(quad.position.y - height * 0.5f);
            baseZ = static_cast<int>(quad.position.z - 1);
            break;
        case 4: // -X (left): width=Z, height=Y
            baseX = static_cast<int>(quad.position.x);
            baseY = static_cast<int>(quad.position.y - height * 0.5f);
            baseZ = static_cast<int>(quad.position.z - width * 0.5f);
            break;
        case 5: // +X (right): width=Z, height=Y
            baseX = static_cast<int>(quad.position.x - 1);
            baseY = static_cast<int>(quad.position.y - height * 0.5f);
            baseZ = static_cast<int>(quad.position.z - width * 0.5f);
            break;
        default:
            return;
    }
    
    // Mark original quad as deleted (set width/height to 0)
    quad.width = 0;
    quad.height = 0;
    
    // Create 1x1 replacement quads for each voxel that still exists
    // Iterate based on face direction
    if (face == 0 || face == 1) // Y faces: width=X, height=Z
    {
        for (int dz = 0; dz < height; ++dz)
        {
            for (int dx = 0; dx < width; ++dx)
            {
                int vx = baseX + dx;
                int vy = baseY;
                int vz = baseZ + dz;
                
                if (vx >= 0 && vx < SIZE && vy >= 0 && vy < SIZE && vz >= 0 && vz < SIZE)
                {
                    if (isVoxelSolid(vx, vy, vz) && isFaceExposed(vx, vy, vz, face))
                    {
                        uint16_t newQuadIdx = static_cast<uint16_t>(mesh->quads.size());
                        addQuad(mesh->quads, static_cast<float>(vx), static_cast<float>(vy),
                               static_cast<float>(vz), face, 1, 1, blockType);
                        
                        int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                        uint32_t key = voxelIdx * 6 + face;
                        mesh->voxelFaceToQuadIndex[key] = newQuadIdx;
                        mesh->isExploded[voxelIdx] = true;
                    }
                }
            }
        }
    }
    else if (face == 2 || face == 3) // Z faces: width=X, height=Y
    {
        for (int dy = 0; dy < height; ++dy)
        {
            for (int dx = 0; dx < width; ++dx)
            {
                int vx = baseX + dx;
                int vy = baseY + dy;
                int vz = baseZ;
                
                if (vx >= 0 && vx < SIZE && vy >= 0 && vy < SIZE && vz >= 0 && vz < SIZE)
                {
                    if (isVoxelSolid(vx, vy, vz) && isFaceExposed(vx, vy, vz, face))
                    {
                        uint16_t newQuadIdx = static_cast<uint16_t>(mesh->quads.size());
                        addQuad(mesh->quads, static_cast<float>(vx), static_cast<float>(vy),
                               static_cast<float>(vz), face, 1, 1, blockType);
                        
                        int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                        uint32_t key = voxelIdx * 6 + face;
                        mesh->voxelFaceToQuadIndex[key] = newQuadIdx;
                        mesh->isExploded[voxelIdx] = true;
                    }
                }
            }
        }
    }
    else // X faces: width=Z, height=Y
    {
        for (int dy = 0; dy < height; ++dy)
        {
            for (int dz = 0; dz < width; ++dz)
            {
                int vx = baseX;
                int vy = baseY + dy;
                int vz = baseZ + dz;
                
                if (vx >= 0 && vx < SIZE && vy >= 0 && vy < SIZE && vz >= 0 && vz < SIZE)
                {
                    if (isVoxelSolid(vx, vy, vz) && isFaceExposed(vx, vy, vz, face))
                    {
                        uint16_t newQuadIdx = static_cast<uint16_t>(mesh->quads.size());
                        addQuad(mesh->quads, static_cast<float>(vx), static_cast<float>(vy),
                               static_cast<float>(vz), face, 1, 1, blockType);
                        
                        int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                        uint32_t key = voxelIdx * 6 + face;
                        mesh->voxelFaceToQuadIndex[key] = newQuadIdx;
                        mesh->isExploded[voxelIdx] = true;
                    }
                }
            }
        }
    }
    
    mesh->needsGPUUpload = true;
}

// Add simple 1x1 faces for a newly placed voxel
void VoxelChunk::addSimpleFacesForVoxel(int x, int y, int z)
{
    auto mesh = getRenderMesh();
    if (!mesh) return;
    
    if (!isVoxelSolid(x, y, z)) return;
    
    uint8_t blockType = getVoxel(x, y, z);
    int voxelIdx = x + y * SIZE + z * SIZE * SIZE;
    
    // Add 1x1 face for each exposed direction
    for (int face = 0; face < 6; ++face)
    {
        if (isFaceExposed(x, y, z, face))
        {
            uint16_t newQuadIdx = static_cast<uint16_t>(mesh->quads.size());
            addQuad(mesh->quads, static_cast<float>(x), static_cast<float>(y),
                   static_cast<float>(z), face, 1, 1, blockType);
            
            uint32_t key = voxelIdx * 6 + face;
            mesh->voxelFaceToQuadIndex[key] = newQuadIdx;
        }
    }
    
    mesh->isExploded[voxelIdx] = true;
    mesh->needsGPUUpload = true;
}

// Upload mesh to GPU immediately
void VoxelChunk::uploadMeshToGPU()
{
    auto mesh = getRenderMesh();
    if (!mesh || !m_isClientChunk) return;
    
    // Trigger GPU upload via renderer
    extern std::unique_ptr<InstancedQuadRenderer> g_instancedQuadRenderer;
    if (g_instancedQuadRenderer)
    {
        g_instancedQuadRenderer->uploadChunkMesh(this);
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






