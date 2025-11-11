// VoxelChunk.cpp - 256x256x256 dynamic physics-enabled voxel chunks
#include "VoxelChunk.h"
#include "BlockType.h"

#include "../Time/DayNightController.h"  // For dynamic sun direction
#include "../Rendering/GPUMeshQueue.h"  // For region-based remeshing queue (greedy meshing)

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
    
    // Initialize render mesh with empty shared_ptr
    renderMesh = std::make_shared<VoxelMesh>();
    
    // Initialize region dirty flags to false
    m_regionDirtyFlags.fill(false);
    m_dirtyRegions.clear();
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
    
    // Mark region dirty for remeshing (new region-based system)
    if (m_incrementalUpdatesEnabled) {
        // Mark the region containing this voxel as dirty
        markRegionDirtyAtVoxel(x, y, z);
        
        // Queue dirty regions for remeshing (callback will fire after remesh completes)
        processDirtyRegions();
        
        // NOTE: Callback is NOT called here - it will be called by GreedyMeshQueue
        // after the mesh is actually regenerated to avoid 1-frame lag
    } else {
        // Incremental updates not enabled yet - mark dirty and trigger full regeneration
        meshDirty = true;
        if (m_meshUpdateCallback) {
            m_meshUpdateCallback(this);
        }
    }
}

void VoxelChunk::setVoxelDataDirect(int x, int y, int z, uint8_t type)
{
    // SERVER-ONLY: Direct voxel data modification without any mesh operations
    if (x < 0 || x >= SIZE || y < 0 || y >= SIZE || z < 0 || z >= SIZE)
        return;
    
    voxels[x + y * SIZE + z * SIZE * SIZE] = type;
    meshDirty = true;  // Mark dirty for consistency, but no mesh generation happens
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
    
    // Initialize mesh if needed
    if (!renderMesh) {
        renderMesh = std::make_shared<VoxelMesh>();
    }
    
    // Queue all regions for meshing through the unified queue system
    if (g_greedyMeshQueue) {
        for (int i = 0; i < ChunkConfig::TOTAL_REGIONS; ++i) {
            g_greedyMeshQueue->queueRegionMesh(this, i);
        }
    }
    
    meshDirty = false;
    
    // Enable incremental updates after first full mesh generation (client-only)
    if (m_isClientChunk) {
        m_incrementalUpdatesEnabled = true;
    }
}

// Generate mesh for a single region only (partial update)
// Returns quads for this region - caller must merge into renderMesh (thread-safe)
std::vector<QuadFace> VoxelChunk::generateMeshForRegion(int regionIndex)
{
    PROFILE_SCOPE("VoxelChunk::generateMeshForRegion");
    
    std::vector<QuadFace> newQuads;
    
    if (regionIndex < 0 || regionIndex >= ChunkConfig::TOTAL_REGIONS) {
        std::cerr << "⚠️  Invalid region index: " << regionIndex << std::endl;
        return newQuads;
    }
    
    // Calculate region boundaries
    int rx = regionIndex % ChunkConfig::REGIONS_PER_AXIS;
    int ry = (regionIndex / ChunkConfig::REGIONS_PER_AXIS) % ChunkConfig::REGIONS_PER_AXIS;
    int rz = regionIndex / (ChunkConfig::REGIONS_PER_AXIS * ChunkConfig::REGIONS_PER_AXIS);
    
    int minX = rx * ChunkConfig::REGION_SIZE;
    int minY = ry * ChunkConfig::REGION_SIZE;
    int minZ = rz * ChunkConfig::REGION_SIZE;
    
    int maxX = std::min(minX + ChunkConfig::REGION_SIZE, SIZE);
    int maxY = std::min(minY + ChunkConfig::REGION_SIZE, SIZE);
    int maxZ = std::min(minZ + ChunkConfig::REGION_SIZE, SIZE);
    
    // Early exit: Check if region has ANY solid voxels before doing expensive greedy meshing
    bool hasAnySolidVoxels = false;
    for (int z = minZ; z < maxZ && !hasAnySolidVoxels; ++z) {
        for (int y = minY; y < maxY && !hasAnySolidVoxels; ++y) {
            for (int x = minX; x < maxX && !hasAnySolidVoxels; ++x) {
                if (isVoxelSolid(x, y, z)) {
                    hasAnySolidVoxels = true;
                }
            }
        }
    }
    
    if (!hasAnySolidVoxels) {
        return newQuads;  // Empty region - no quads needed
    }
    
    // Generate new quads for this region using greedy meshing (into LOCAL buffer)
    
    // Temp bitmask for greedy merging within this region
    int regionSizeX = maxX - minX;
    int regionSizeY = maxY - minY;
    int regionSizeZ = maxZ - minZ;
    std::vector<bool> merged(regionSizeX * regionSizeY * regionSizeZ, false);
    
    // Greedy meshing per face direction
    for (int face = 0; face < 6; ++face)
    {
        merged.assign(regionSizeX * regionSizeY * regionSizeZ, false);
        
        // Determine sweep axes (same logic as full chunk)
        int uAxis, vAxis, wAxis;
        switch (face)
        {
            case 0: case 1: uAxis = 0; vAxis = 2; wAxis = 1; break;
            case 2: case 3: uAxis = 0; vAxis = 1; wAxis = 2; break;
            case 4: case 5: uAxis = 2; vAxis = 1; wAxis = 0; break;
            default: continue;
        }
        
        // Sweep through region slices
        for (int w = 0; w < regionSizeZ; ++w)
        {
            for (int v = 0; v < regionSizeY; ++v)
            {
                for (int u = 0; u < regionSizeX; ++u)
                {
                    // Local region coords
                    int rx, ry, rz;
                    if (face <= 1) { rx = u; ry = w; rz = v; }
                    else if (face <= 3) { rx = u; ry = v; rz = w; }
                    else { rx = w; ry = v; rz = u; }
                    
                    // Global chunk coords
                    int x = minX + rx;
                    int y = minY + ry;
                    int z = minZ + rz;
                    
                    int idx = rx + ry * regionSizeX + rz * regionSizeX * regionSizeY;
                    if (merged[idx]) continue;
                    if (!isVoxelSolid(x, y, z)) continue;
                    if (!isFaceExposed(x, y, z, face)) continue;
                    
                    uint8_t blockType = getVoxel(x, y, z);
                    
                    // Greedy width expansion
                    int width = 1;
                    for (int du = u + 1; du < regionSizeX; ++du)
                    {
                        int xu = minX + (face <= 1 ? du : (face <= 3 ? du : rx));
                        int yu = minY + (face <= 1 ? ry : (face <= 3 ? ry : v));
                        int zu = minZ + (face <= 1 ? v : (face <= 3 ? w : du));
                        
                        int idxu = (face <= 1 ? du : (face <= 3 ? du : rx)) +
                                   (face <= 1 ? ry : (face <= 3 ? ry : v)) * regionSizeX +
                                   (face <= 1 ? v : (face <= 3 ? w : du)) * regionSizeX * regionSizeY;
                        
                        if (xu >= maxX || merged[idxu]) break;
                        if (!isVoxelSolid(xu, yu, zu)) break;
                        if (getVoxel(xu, yu, zu) != blockType) break;
                        if (!isFaceExposed(xu, yu, zu, face)) break;
                        
                        width++;
                    }
                    
                    // Greedy height expansion
                    int height = 1;
                    bool canExtendHeight = true;
                    for (int dv = v + 1; dv < regionSizeY && canExtendHeight; ++dv)
                    {
                        for (int du = u; du < u + width; ++du)
                        {
                            int xv = minX + (face <= 1 ? du : (face <= 3 ? du : rx));
                            int yv = minY + (face <= 1 ? ry : (face <= 3 ? dv : dv));
                            int zv = minZ + (face <= 1 ? dv : (face <= 3 ? w : du));
                            
                            int idxv = (face <= 1 ? du : (face <= 3 ? du : rx)) +
                                       (face <= 1 ? ry : (face <= 3 ? dv : dv)) * regionSizeX +
                                       (face <= 1 ? dv : (face <= 3 ? w : du)) * regionSizeX * regionSizeY;
                            
                            if (yv >= maxY || zv >= maxZ || merged[idxv]) {
                                canExtendHeight = false;
                                break;
                            }
                            if (!isVoxelSolid(xv, yv, zv)) {
                                canExtendHeight = false;
                                break;
                            }
                            if (getVoxel(xv, yv, zv) != blockType) {
                                canExtendHeight = false;
                                break;
                            }
                            if (!isFaceExposed(xv, yv, zv, face)) {
                                canExtendHeight = false;
                                break;
                            }
                        }
                        if (canExtendHeight) height++;
                    }
                    
                    // Mark merged
                    for (int dv = 0; dv < height; ++dv)
                    {
                        for (int du = 0; du < width; ++du)
                        {
                            int idxm = (face <= 1 ? u + du : (face <= 3 ? u + du : rx)) +
                                       (face <= 1 ? ry : (face <= 3 ? v + dv : v + dv)) * regionSizeX +
                                       (face <= 1 ? v + dv : (face <= 3 ? w : u + du)) * regionSizeX * regionSizeY;
                            merged[idxm] = true;
                        }
                    }
                    
                    // Add merged quad with repeat textures
                    addQuad(newQuads, static_cast<float>(x), static_cast<float>(y),
                           static_cast<float>(z), face, width, height, blockType);
                }
            }
        }
    }
    
    // Return quads - caller will merge into renderMesh on main thread
    return newQuads;
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
// REGION-BASED DIRTY TRACKING
// ========================================

void VoxelChunk::markRegionDirty(int regionIndex)
{
    if (regionIndex < 0 || regionIndex >= ChunkConfig::TOTAL_REGIONS)
        return;
    
    if (!m_regionDirtyFlags[regionIndex]) {
        m_regionDirtyFlags[regionIndex] = true;
        m_dirtyRegions.push_back(regionIndex);
    }
}

void VoxelChunk::markRegionDirtyAtVoxel(int x, int y, int z)
{
    if (x < 0 || x >= SIZE || y < 0 || y >= SIZE || z < 0 || z >= SIZE)
        return;
    
    int regionIndex = ChunkConfig::voxelToRegionIndex(x, y, z);
    markRegionDirty(regionIndex);
    
    // Also mark neighbor regions if on boundary
    // This ensures proper face culling across region boundaries
    if (x % ChunkConfig::REGION_SIZE == 0 && x > 0) {
        int neighborIndex = ChunkConfig::voxelToRegionIndex(x - 1, y, z);
        markRegionDirty(neighborIndex);
    }
    if ((x + 1) % ChunkConfig::REGION_SIZE == 0 && x < SIZE - 1) {
        int neighborIndex = ChunkConfig::voxelToRegionIndex(x + 1, y, z);
        markRegionDirty(neighborIndex);
    }
    
    if (y % ChunkConfig::REGION_SIZE == 0 && y > 0) {
        int neighborIndex = ChunkConfig::voxelToRegionIndex(x, y - 1, z);
        markRegionDirty(neighborIndex);
    }
    if ((y + 1) % ChunkConfig::REGION_SIZE == 0 && y < SIZE - 1) {
        int neighborIndex = ChunkConfig::voxelToRegionIndex(x, y + 1, z);
        markRegionDirty(neighborIndex);
    }
    
    if (z % ChunkConfig::REGION_SIZE == 0 && z > 0) {
        int neighborIndex = ChunkConfig::voxelToRegionIndex(x, y, z - 1);
        markRegionDirty(neighborIndex);
    }
    if ((z + 1) % ChunkConfig::REGION_SIZE == 0 && z < SIZE - 1) {
        int neighborIndex = ChunkConfig::voxelToRegionIndex(x, y, z + 1);
        markRegionDirty(neighborIndex);
    }
}

bool VoxelChunk::isRegionDirty(int regionIndex) const
{
    if (regionIndex < 0 || regionIndex >= ChunkConfig::TOTAL_REGIONS)
        return false;
    
    return m_regionDirtyFlags[regionIndex];
}

void VoxelChunk::clearAllRegionDirtyFlags()
{
    m_regionDirtyFlags.fill(false);
    m_dirtyRegions.clear();
}

void VoxelChunk::processDirtyRegions()
{
    // Queue all dirty regions for remeshing via GreedyMeshQueue
    // This is called by the renderer callback when mesh changes occur
    if (!g_greedyMeshQueue) return;
    
    for (int regionIndex : m_dirtyRegions) {
        g_greedyMeshQueue->queueRegionMesh(this, regionIndex);
    }
    
    // Clear dirty flags after queuing
    clearAllRegionDirtyFlags();
}




