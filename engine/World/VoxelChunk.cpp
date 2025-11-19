// VoxelChunk.cpp - 256x256x256 dynamic physics-enabled voxel chunks
#include "VoxelChunk.h"
#include "BlockType.h"

#include "../Time/DayNightController.h"  // For dynamic sun direction
#include "../Rendering/Vulkan/VulkanQuadRenderer.h"  // For Vulkan GPU upload

#include <algorithm>
#include <cmath>
#include <iostream>
#include <chrono>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

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
    if (oldType == type) return;
    
    // Update voxel data
    voxels[voxelIdx] = type;
    
    // Track OBJ-type blocks
    auto& registry = BlockTypeRegistry::getInstance();
    const BlockTypeInfo* oldBlockInfo = registry.getBlockType(oldType);
    const BlockTypeInfo* newBlockInfo = registry.getBlockType(type);
    
    if (oldBlockInfo && oldBlockInfo->renderType == BlockRenderType::OBJ) {
        auto it = m_modelInstances.find(oldType);
        if (it != m_modelInstances.end()) {
            auto& instances = it->second;
            glm::vec3 pos(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
            instances.erase(std::remove(instances.begin(), instances.end(), pos), instances.end());
        }
    }
    
    if (newBlockInfo && newBlockInfo->renderType == BlockRenderType::OBJ) {
        glm::vec3 pos(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        m_modelInstances[type].push_back(pos);
    }
    
    // Remesh chunk (Vulkan path generates mesh directly)
    if (m_isClientChunk)
    {
        generateMesh();
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
                    glm::vec3 pos(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                    m_modelInstances[blockType].push_back(pos);
                }
            }
        }
    }
}

void VoxelChunk::setIslandContext(uint32_t islandID, const glm::vec3& chunkCoord)
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
    (void)generateLighting; // Parameter kept for API compatibility but unused (real-time lighting only)
    
    // Initialize mesh if needed
    if (!renderMesh) {
        renderMesh = std::make_shared<VoxelMesh>();
    }
    
    // Clear previous mesh data
    renderMesh->quads.clear();
    
    // Generate greedy mesh for all 6 faces
    for (int face = 0; face < 6; ++face) {
        greedyMeshFace(renderMesh->quads, face);
    }
    
    // Upload to GPU
    uploadMeshToGPU();
}

// Greedy meshing for a single face direction
void VoxelChunk::greedyMeshFace(std::vector<QuadFace>& quads, int face)
{
    // Industry standard face ordering: 0=-X, 1=+X, 2=-Y, 3=+Y, 4=-Z, 5=+Z
    // Process each face direction with proper axis mapping
    
    // Iterate through each voxel position and build greedy rectangles
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
                if (face == 0 || face == 1) // X faces: expand in Z direction (width), Y direction (height)
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
                else if (face == 2 || face == 3) // Y faces: expand in X direction (width), Z direction (height)
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
                else // Z faces: expand in X direction (width), Y direction (height)
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
                
                // Mark merged area as visited
                if (face == 0 || face == 1) // X faces
                {
                    for (int dy = 0; dy < height; ++dy)
                        for (int dz = 0; dz < width; ++dz)
                            visited[x + (y + dy) * SIZE + (z + dz) * SIZE * SIZE] = true;
                }
                else if (face == 2 || face == 3) // Y faces
                {
                    for (int dz = 0; dz < height; ++dz)
                        for (int dx = 0; dx < width; ++dx)
                            visited[(x + dx) + y * SIZE + (z + dz) * SIZE * SIZE] = true;
                }
                else // Z faces
                {
                    for (int dy = 0; dy < height; ++dy)
                        for (int dx = 0; dx < width; ++dx)
                            visited[(x + dx) + (y + dy) * SIZE + z * SIZE * SIZE] = true;
                }
                
                // Add merged quad (island-relative position)
                addQuad(quads, static_cast<float>(x), static_cast<float>(y),
                       static_cast<float>(z), face, width, height, blockType);
            }
        }
    }
}

// Generate mesh for entire chunk - GREEDY MESHING (island-relative positions)
std::vector<QuadFace> VoxelChunk::generateFullChunkMesh()
{
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
    
    // GREEDY MESHING - Merge adjacent faces of same block type
    for (int face = 0; face < 6; ++face)
    {
        greedyMeshFace(quads, face);
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    (void)duration; // Suppress unused warning

    return quads;
}

int VoxelChunk::calculateLOD(const glm::vec3& cameraPos) const
{
    // Simple distance-based LOD calculation
    glm::vec3 chunkCenter(SIZE * 0.5f, SIZE * 0.5f, SIZE * 0.5f);
    glm::vec3 distance = cameraPos - chunkCenter;
    float dist = glm::length(distance);

    // LOD distances scale with chunk size (half-chunk and full-chunk)
    if (dist < SIZE * 0.5f)
        return 0;  // High detail (within half chunk)
    else if (dist < SIZE * 1.0f)
        return 1;  // Medium detail (within full chunk)
    else
        return 2;  // Low detail (beyond chunk)
}

bool VoxelChunk::shouldRender(const glm::vec3& cameraPos, float maxDistance) const
{
    glm::vec3 chunkCenter(SIZE * 0.5f, SIZE * 0.5f, SIZE * 0.5f);
    glm::vec3 distance = cameraPos - chunkCenter;
    float dist = glm::length(distance);
    return dist <= maxDistance;
}

// ========================================
// UNIFIED CULLING - Works for intra-chunk AND inter-chunk
// ========================================

// INTRA-CHUNK CULLING ONLY - Inter-chunk culling removed for performance
// Boundary faces are always rendered (negligible visual difference, massive speed gain)
bool VoxelChunk::isFaceExposed(int x, int y, int z, int face) const
{
    // Industry standard: 0=-X, 1=+X, 2=-Y, 3=+Y, 4=-Z, 5=+Z
    static const int dx[6] = {-1,  1,  0,  0,  0,  0};
    static const int dy[6] = { 0,  0, -1,  1,  0,  0};
    static const int dz[6] = { 0,  0,  0,  0, -1,  1};
    
    int nx = x + dx[face];
    int ny = y + dy[face];
    int nz = z + dz[face];
    
    // Chunk boundary = always render (no inter-chunk culling)
    if (nx < 0 || nx >= SIZE || ny < 0 || ny >= SIZE || nz < 0 || nz >= SIZE)
    {
        return true;
    }
    
    // Face is exposed if neighbor is not solid (intra-chunk culling)
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
    
    // Convert corner position back to base voxel coordinates
    // Must reverse the corner offsets applied in addQuad()
    int baseX, baseY, baseZ;
    
    switch (face)
    {
        case 0: // -X: corner at (x, y, z)
            baseX = static_cast<int>(quad.position.x);
            baseY = static_cast<int>(quad.position.y);
            baseZ = static_cast<int>(quad.position.z);
            break;
        case 1: // +X: corner at (x+1, y, z+width) - reverse it
            baseX = static_cast<int>(quad.position.x) - 1;
            baseY = static_cast<int>(quad.position.y);
            baseZ = static_cast<int>(quad.position.z) - width;
            break;
        case 2: // -Y: corner at (x, y, z)
            baseX = static_cast<int>(quad.position.x);
            baseY = static_cast<int>(quad.position.y);
            baseZ = static_cast<int>(quad.position.z);
            break;
        case 3: // +Y: corner at (x, y+1, z+height) - reverse it
            baseX = static_cast<int>(quad.position.x);
            baseY = static_cast<int>(quad.position.y) - 1;
            baseZ = static_cast<int>(quad.position.z) - height;
            break;
        case 4: // -Z: corner at (x+width, y, z) - reverse it
            baseX = static_cast<int>(quad.position.x) - width;
            baseY = static_cast<int>(quad.position.y);
            baseZ = static_cast<int>(quad.position.z);
            break;
        case 5: // +Z: corner at (x, y, z+1) - reverse it
            baseX = static_cast<int>(quad.position.x);
            baseY = static_cast<int>(quad.position.y);
            baseZ = static_cast<int>(quad.position.z) - 1;
            break;
        default:
            return;
    }
    
    // Mark original quad as deleted (set width/height to 0)
    quad.width = 0;
    quad.height = 0;
    
    std::cout << "[EXPLODE] Quad " << quadIndex << " face=" << face << " width=" << width << " height=" << height 
              << " base=(" << baseX << "," << baseY << "," << baseZ << ")\n";
    
    // Create 1x1 replacement quads for each voxel that still exists
    // Iterate based on face direction (industry standard: 0=-X, 1=+X, 2=-Y, 3=+Y, 4=-Z, 5=+Z)
    int replacementCount = 0;
    if (face == 0 || face == 1) // X faces: width=Z, height=Y
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
                    bool solid = isVoxelSolid(vx, vy, vz);
                    bool exposed = isFaceExposed(vx, vy, vz, face);
                    std::cout << "[EXPLODE] Voxel (" << vx << "," << vy << "," << vz << ") solid=" << solid << " exposed=" << exposed << "\n";
                    if (solid && exposed)
                    {
                        uint16_t newQuadIdx = static_cast<uint16_t>(mesh->quads.size());
                        addQuad(mesh->quads, static_cast<float>(vx), static_cast<float>(vy),
                               static_cast<float>(vz), face, 1, 1, blockType);
                        
                        int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                        uint32_t key = voxelIdx * 6 + face;
                        mesh->voxelFaceToQuadIndex[key] = newQuadIdx;
                        mesh->isExploded[voxelIdx] = true;
                        replacementCount++;
                    }
                }
            }
        }
    }
    else if (face == 2 || face == 3) // Y faces: width=X, height=Z
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
                    bool solid = isVoxelSolid(vx, vy, vz);
                    bool exposed = isFaceExposed(vx, vy, vz, face);
                    std::cout << "[EXPLODE] Voxel (" << vx << "," << vy << "," << vz << ") solid=" << solid << " exposed=" << exposed << "\n";
                    if (solid && exposed)
                    {
                        uint16_t newQuadIdx = static_cast<uint16_t>(mesh->quads.size());
                        addQuad(mesh->quads, static_cast<float>(vx), static_cast<float>(vy),
                               static_cast<float>(vz), face, 1, 1, blockType);
                        
                        int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                        uint32_t key = voxelIdx * 6 + face;
                        mesh->voxelFaceToQuadIndex[key] = newQuadIdx;
                        mesh->isExploded[voxelIdx] = true;
                        replacementCount++;
                    }
                }
            }
        }
    }
    else // Z faces: width=X, height=Y
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
                    bool solid = isVoxelSolid(vx, vy, vz);
                    bool exposed = isFaceExposed(vx, vy, vz, face);
                    std::cout << "[EXPLODE] Voxel (" << vx << "," << vy << "," << vz << ") solid=" << solid << " exposed=" << exposed << "\n";
                    if (solid && exposed)
                    {
                        uint16_t newQuadIdx = static_cast<uint16_t>(mesh->quads.size());
                        addQuad(mesh->quads, static_cast<float>(vx), static_cast<float>(vy),
                               static_cast<float>(vz), face, 1, 1, blockType);
                        
                        int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                        uint32_t key = voxelIdx * 6 + face;
                        mesh->voxelFaceToQuadIndex[key] = newQuadIdx;
                        mesh->isExploded[voxelIdx] = true;
                        replacementCount++;
                    }
                }
            }
        }
    }
    
    std::cout << "[EXPLODE] Created " << replacementCount << " replacement quads\n";
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
    if (!mesh || !m_isClientChunk) {
        printf("[CHUNK] uploadMeshToGPU early return: mesh=%p, isClient=%d\n", (void*)mesh.get(), m_isClientChunk);
        return;
    }
    // Trigger GPU upload via Vulkan renderer
    extern class VulkanQuadRenderer* g_vulkanQuadRenderer;
    
    if (g_vulkanQuadRenderer)
    {
        g_vulkanQuadRenderer->uploadChunkMesh(this);
    }
}

// Model instance management (for BlockRenderType::OBJ blocks)
const std::vector<glm::vec3>& VoxelChunk::getModelInstances(uint8_t blockID) const
{
    static const std::vector<glm::vec3> empty;
    auto it = m_modelInstances.find(blockID);
    if (it != m_modelInstances.end())
        return it->second;
    return empty;
}

// Add a single quad to the mesh (width/height support for future optimizations)
void VoxelChunk::addQuad(std::vector<QuadFace>& quads, float x, float y, float z, int face, int width, int height, uint8_t blockType)
{
    // Industry standard face ordering: -X, +X, -Y, +Y, -Z, +Z
    // Face ordering: 0=-X, 1=+X, 2=-Y, 3=+Y, 4=-Z, 5=+Z
    
    static const glm::vec3 normals[6] = {
        glm::vec3(-1, 0, 0),  // -X (left)
        glm::vec3(1, 0, 0),   // +X (right)
        glm::vec3(0, -1, 0),  // -Y (bottom)
        glm::vec3(0, 1, 0),   // +Y (top)
        glm::vec3(0, 0, -1),  // -Z (back)
        glm::vec3(0, 0, 1)    // +Z (front)
    };
    
    glm::vec3 normal = normals[face];
    float w = static_cast<float>(width);
    float h = static_cast<float>(height);
    
    // INDUSTRY STANDARD: Store corner position (bottom-left in face-local space)
    // Shader rotation matrices define face-local axes:
    // -X: right=+Z, up=+Y → BL at (x, y, z)
    // +X: right=-Z, up=+Y → BL at (x+1, y, z+width) because right=-Z
    // -Y: right=+X, up=+Z → BL at (x, y, z)
    // +Y: right=+X, up=-Z → BL at (x, y+1, z+height) because up=-Z
    // -Z: right=-X, up=+Y → BL at (x+width, y, z) because right=-X
    // +Z: right=+X, up=+Y → BL at (x, y, z+1)
    glm::vec3 cornerPos;
    switch (face)
    {
        case 0: // -X (left): right=+Z, up=+Y
            cornerPos = glm::vec3(x, y, z);
            break;
        case 1: // +X (right): right=-Z, up=+Y (reversed width direction)
            cornerPos = glm::vec3(x + 1.0f, y, z + static_cast<float>(width));
            break;
        case 2: // -Y (bottom): right=+X, up=+Z
            cornerPos = glm::vec3(x, y, z);
            break;
        case 3: // +Y (top): right=+X, up=-Z (reversed height direction)
            cornerPos = glm::vec3(x, y + 1.0f, z + static_cast<float>(height));
            break;
        case 4: // -Z (back): right=-X, up=+Y (reversed width direction)
            cornerPos = glm::vec3(x + static_cast<float>(width), y, z);
            break;
        case 5: // +Z (front): right=+X, up=+Y
            cornerPos = glm::vec3(x, y, z + 1.0f);
            break;
    }
    
    // Pack normal into 10:10:10:2 format (X:10, Y:10, Z:10, W:2)
    // Map -1..1 to 0..1023, with 512 as zero (matches shader unpacking: value - 512)
    int nx = static_cast<int>(normal.x * 511.5f + 512.0f);  // -1→0, 0→512, 1→1023
    int ny = static_cast<int>(normal.y * 511.5f + 512.0f);
    int nz = static_cast<int>(normal.z * 511.5f + 512.0f);
    uint32_t packedNormal = (nx & 0x3FF) | ((ny & 0x3FF) << 10) | ((nz & 0x3FF) << 20);
    
    // Add to QuadFace array for instanced rendering
    QuadFace quadFace;
    quadFace.position = cornerPos;  // Island-relative corner position (industry standard)
    quadFace._padding0 = 0.0f;      // CRITICAL: Initialize padding for std430 alignment
    quadFace.width = w;
    quadFace.height = h;
    quadFace.packedNormal = packedNormal;
    quadFace.blockType = blockType;
    quadFace.faceDir = static_cast<uint32_t>(face);  // uint32_t to match shader
    quadFace.islandID = m_islandID;  // For SSBO transform lookup, set by renderer
    
    quads.push_back(quadFace);
}






