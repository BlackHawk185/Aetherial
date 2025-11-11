// VoxelChunk.cpp - 256x256x256 dynamic physics-enabled voxel chunks
#include "VoxelChunk.h"
#include "BlockType.h"

#include "../Time/DayNightController.h"  // For dynamic sun direction
#include "../Rendering/GPUMeshQueue.h"  // For region-based remeshing queue

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
        
        // NOTE: Callback is NOT called here - it will be called by GPUMeshQueue
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
    
    auto t_start = std::chrono::high_resolution_clock::now();
    
    (void)generateLighting; // Parameter kept for API compatibility but unused (real-time lighting only)
    
    // Build into a new mesh (no locks needed - parallel-safe!)
    auto newMesh = std::make_shared<VoxelMesh>();
    
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

    // DIRECT ASSIGNMENT - fast, no atomic overhead!
    renderMesh = newMesh;
    
    // Update model instances (protected by implicit synchronization)
    m_modelInstances = std::move(tempModelInstances);
    
    meshDirty = false;
    
    // Enable incremental updates after first full mesh generation (client-only)
    if (m_isClientChunk) {
        m_incrementalUpdatesEnabled = true;
    }
    
    auto t_end = std::chrono::high_resolution_clock::now();
    
    // Performance tracking
    auto prescan_ms = std::chrono::duration<double, std::milli>(t_prescan_end - t_prescan_start).count();
    auto mesh_ms = std::chrono::duration<double, std::milli>(t_mesh_end - t_mesh_start).count();
    auto total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    
    std::cout << "🔧 MESH GEN: Total=" << total_ms << "ms (Prescan=" << prescan_ms 
              << "ms, Mesh=" << mesh_ms << "ms) Quads=" << newMesh->quads.size() << std::endl;
}

// Generate mesh for a single region only (partial update)
void VoxelChunk::generateMeshForRegion(int regionIndex)
{
    PROFILE_SCOPE("VoxelChunk::generateMeshForRegion");
    
    if (regionIndex < 0 || regionIndex >= ChunkConfig::TOTAL_REGIONS) {
        std::cerr << "⚠️  Invalid region index: " << regionIndex << std::endl;
        return;
    }
    
    auto t_start = std::chrono::high_resolution_clock::now();
    
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
    
    if (!renderMesh) {
        renderMesh = std::make_shared<VoxelMesh>();
    }
    
    // Remove all quads from this region (we'll regenerate them)
    // This is a simple approach - we could optimize by tracking quad indices per region
    auto& quads = renderMesh->quads;
    quads.erase(
        std::remove_if(quads.begin(), quads.end(), [&](const QuadFace& quad) {
            int qx = static_cast<int>(quad.position.x);
            int qy = static_cast<int>(quad.position.y);
            int qz = static_cast<int>(quad.position.z);
            
            // Adjust for face offset to get actual voxel coords
            int face = quad.faceDir;
            if (face == 1) qy--;       // Top face
            else if (face == 3) qz--;  // Front face
            else if (face == 5) qx--;  // Right face
            
            return (qx >= minX && qx < maxX &&
                    qy >= minY && qy < maxY &&
                    qz >= minZ && qz < maxZ);
        }),
        quads.end()
    );
    
    // Generate new quads for this region
    std::vector<QuadFace> newQuads;
    for (int z = minZ; z < maxZ; ++z) {
        for (int y = minY; y < maxY; ++y) {
            for (int x = minX; x < maxX; ++x) {
                if (!isVoxelSolid(x, y, z)) continue;
                
                uint8_t blockType = getVoxel(x, y, z);
                
                // Check all 6 faces
                for (int face = 0; face < 6; ++face) {
                    if (isFaceExposed(x, y, z, face)) {
                        addQuad(newQuads, static_cast<float>(x), static_cast<float>(y),
                               static_cast<float>(z), face, 1, 1, blockType);
                    }
                }
            }
        }
    }
    
    // Append new quads to mesh
    quads.insert(quads.end(), newQuads.begin(), newQuads.end());
    
    // Clear this region's dirty flag
    if (regionIndex >= 0 && regionIndex < ChunkConfig::TOTAL_REGIONS) {
        m_regionDirtyFlags[regionIndex] = false;
    }
    
    auto t_end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    
    std::cout << "🔧 REGION MESH GEN: Region=" << regionIndex << " Time=" << ms 
              << "ms Quads=" << newQuads.size() << " Total=" << quads.size() << std::endl;
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
    // Queue all dirty regions for remeshing via GPUMeshQueue
    // This is called by the renderer callback when mesh changes occur
    if (!g_gpuMeshQueue) return;
    
    for (int regionIndex : m_dirtyRegions) {
        g_gpuMeshQueue->queueRegionMesh(this, regionIndex, nullptr);
    }
    
    // Clear dirty flags after queuing
    clearAllRegionDirtyFlags();
}




