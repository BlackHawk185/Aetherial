// VoxelChunk.cpp - 256x256x256 dynamic physics-enabled voxel chunks
#include "VoxelChunk.h"
#include "BlockType.h"
#include "MeshGenerationPool.h"

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
    
    // Use direct quad manipulation instead of full remesh
    setVoxelWithQuadManipulation(x, y, z, type);
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
                    // Water culling: only render if air above (prevent vertical stacking reflections)
                    if (blockType == BlockID::WATER) {
                        uint8_t blockAbove = (y + 1 < SIZE) ? getVoxel(x, y + 1, z) : BlockID::AIR;
                        if (blockAbove != BlockID::AIR) {
                            continue;  // Water is occluded by block above, skip rendering
                        }
                    }
                    
                    glm::vec3 pos(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f);
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

void VoxelChunk::generateMeshAsync(bool generateLighting)
{
    (void)generateLighting;
    
    // Cancel any pending mesh generation
    std::lock_guard<std::mutex> lock(m_meshMutex);
    if (m_pendingMeshFuture.valid()) {
        // Previous mesh still building - let it finish, we'll replace it
    }
    
    // Copy voxel data for worker thread (thread-safe)
    auto voxelDataCopy = std::make_shared<std::array<uint8_t, VOLUME>>(voxels);
    uint32_t islandID = m_islandID;
    
    // Create promise/future pair for thread pool
    auto promise = std::make_shared<std::promise<std::shared_ptr<VoxelMesh>>>();
    m_pendingMeshFuture = promise->get_future();
    
    // Submit to thread pool instead of spawning new thread
    MeshGenerationPool::getInstance().enqueue([voxelDataCopy, islandID, promise]() {
        auto newMesh = std::make_shared<VoxelMesh>();
        
        // Inline greedy meshing for all 6 faces using copied voxel data
        std::vector<QuadFace> quads;
        
        // Helper lambda to check if a block is solid
        auto& registry = BlockTypeRegistry::getInstance();
        auto isBlockSolid = [&](uint8_t blockID) -> bool {
            if (blockID == BlockID::AIR) return false;
            const BlockTypeInfo* blockInfo = registry.getBlockType(blockID);
            if (blockInfo && blockInfo->renderType == BlockRenderType::OBJ) {
                return false;
            }
            return true;
        };
        
        // Inline greedy meshing
        for (int face = 0; face < 6; ++face) {
            std::vector<bool> visited(SIZE * SIZE * SIZE, false);
            
            for (int z = 0; z < SIZE; ++z) {
                for (int y = 0; y < SIZE; ++y) {
                    for (int x = 0; x < SIZE; ++x) {
                        int idx = x + y * SIZE + z * SIZE * SIZE;
                        uint8_t blockType = (*voxelDataCopy)[idx];
                        
                        if (!isBlockSolid(blockType)) continue;
                        if (visited[idx]) continue;
                        
                        // Check if face is exposed
                        bool exposed = false;
                        switch (face) {
                            case 0: exposed = (x == 0 || !isBlockSolid((*voxelDataCopy)[(x-1) + y*SIZE + z*SIZE*SIZE])); break;
                            case 1: exposed = (x == SIZE-1 || !isBlockSolid((*voxelDataCopy)[(x+1) + y*SIZE + z*SIZE*SIZE])); break;
                            case 2: exposed = (y == 0 || !isBlockSolid((*voxelDataCopy)[x + (y-1)*SIZE + z*SIZE*SIZE])); break;
                            case 3: exposed = (y == SIZE-1 || !isBlockSolid((*voxelDataCopy)[x + (y+1)*SIZE + z*SIZE*SIZE])); break;
                            case 4: exposed = (z == 0 || !isBlockSolid((*voxelDataCopy)[x + y*SIZE + (z-1)*SIZE*SIZE])); break;
                            case 5: exposed = (z == SIZE-1 || !isBlockSolid((*voxelDataCopy)[x + y*SIZE + (z+1)*SIZE*SIZE])); break;
                        }
                        
                        if (!exposed) continue;
                        
                        // Greedy meshing - expand width and height
                        int width = 1;
                        int height = 1;
                        
                        // Helper to check if a specific voxel's face is exposed
                        auto isFaceExposed = [&](int vx, int vy, int vz, int faceDir) -> bool {
                            switch (faceDir) {
                                case 0: return (vx == 0 || !isBlockSolid((*voxelDataCopy)[(vx-1) + vy*SIZE + vz*SIZE*SIZE]));
                                case 1: return (vx == SIZE-1 || !isBlockSolid((*voxelDataCopy)[(vx+1) + vy*SIZE + vz*SIZE*SIZE]));
                                case 2: return (vy == 0 || !isBlockSolid((*voxelDataCopy)[vx + (vy-1)*SIZE + vz*SIZE*SIZE]));
                                case 3: return (vy == SIZE-1 || !isBlockSolid((*voxelDataCopy)[vx + (vy+1)*SIZE + vz*SIZE*SIZE]));
                                case 4: return (vz == 0 || !isBlockSolid((*voxelDataCopy)[vx + vy*SIZE + (vz-1)*SIZE*SIZE]));
                                case 5: return (vz == SIZE-1 || !isBlockSolid((*voxelDataCopy)[vx + vy*SIZE + (vz+1)*SIZE*SIZE]));
                                default: return false;
                            }
                        };
                        
                        if (face == 0 || face == 1) { // X faces
                            while (z + width < SIZE) {
                                int checkIdx = x + y * SIZE + (z + width) * SIZE * SIZE;
                                uint8_t checkBlock = (*voxelDataCopy)[checkIdx];
                                if (visited[checkIdx] || !isBlockSolid(checkBlock) || checkBlock != blockType) break;
                                if (!isFaceExposed(x, y, z + width, face)) break;
                                ++width;
                            }
                            bool canExpand = true;
                            while (y + height < SIZE && canExpand) {
                                for (int dz = 0; dz < width; ++dz) {
                                    int checkIdx = x + (y + height) * SIZE + (z + dz) * SIZE * SIZE;
                                    uint8_t checkBlock = (*voxelDataCopy)[checkIdx];
                                    if (visited[checkIdx] || !isBlockSolid(checkBlock) || checkBlock != blockType || !isFaceExposed(x, y + height, z + dz, face)) {
                                        canExpand = false;
                                        break;
                                    }
                                }
                                if (canExpand) ++height;
                            }
                            for (int dy = 0; dy < height; ++dy)
                                for (int dz = 0; dz < width; ++dz)
                                    visited[x + (y + dy) * SIZE + (z + dz) * SIZE * SIZE] = true;
                        } else if (face == 2 || face == 3) { // Y faces
                            while (x + width < SIZE) {
                                int checkIdx = (x + width) + y * SIZE + z * SIZE * SIZE;
                                uint8_t checkBlock = (*voxelDataCopy)[checkIdx];
                                if (visited[checkIdx] || !isBlockSolid(checkBlock) || checkBlock != blockType) break;
                                if (!isFaceExposed(x + width, y, z, face)) break;
                                ++width;
                            }
                            bool canExpand = true;
                            while (z + height < SIZE && canExpand) {
                                for (int dx = 0; dx < width; ++dx) {
                                    int checkIdx = (x + dx) + y * SIZE + (z + height) * SIZE * SIZE;
                                    uint8_t checkBlock = (*voxelDataCopy)[checkIdx];
                                    if (visited[checkIdx] || !isBlockSolid(checkBlock) || checkBlock != blockType || !isFaceExposed(x + dx, y, z + height, face)) {
                                        canExpand = false;
                                        break;
                                    }
                                }
                                if (canExpand) ++height;
                            }
                            for (int dz = 0; dz < height; ++dz)
                                for (int dx = 0; dx < width; ++dx)
                                    visited[(x + dx) + y * SIZE + (z + dz) * SIZE * SIZE] = true;
                        } else { // Z faces
                            while (x + width < SIZE) {
                                int checkIdx = (x + width) + y * SIZE + z * SIZE * SIZE;
                                uint8_t checkBlock = (*voxelDataCopy)[checkIdx];
                                if (visited[checkIdx] || !isBlockSolid(checkBlock) || checkBlock != blockType) break;
                                if (!isFaceExposed(x + width, y, z, face)) break;
                                ++width;
                            }
                            bool canExpand = true;
                            while (y + height < SIZE && canExpand) {
                                for (int dx = 0; dx < width; ++dx) {
                                    int checkIdx = (x + dx) + (y + height) * SIZE + z * SIZE * SIZE;
                                    uint8_t checkBlock = (*voxelDataCopy)[checkIdx];
                                    if (visited[checkIdx] || !isBlockSolid(checkBlock) || checkBlock != blockType || !isFaceExposed(x + dx, y + height, z, face)) {
                                        canExpand = false;
                                        break;
                                    }
                                }
                                if (canExpand) ++height;
                            }
                            for (int dy = 0; dy < height; ++dy)
                                for (int dx = 0; dx < width; ++dx)
                                    visited[(x + dx) + (y + dy) * SIZE + z * SIZE * SIZE] = true;
                        }
                        
                        // Build quad
                        static const glm::vec3 normals[6] = {
                            glm::vec3(-1, 0, 0), glm::vec3(1, 0, 0),
                            glm::vec3(0, -1, 0), glm::vec3(0, 1, 0),
                            glm::vec3(0, 0, -1), glm::vec3(0, 0, 1)
                        };
                        
                        glm::vec3 cornerPos;
                        switch (face) {
                            case 0: cornerPos = glm::vec3(x, y, z); break;
                            case 1: cornerPos = glm::vec3(x + 1.0f, y, z + width); break;
                            case 2: cornerPos = glm::vec3(x, y, z); break;
                            case 3: cornerPos = glm::vec3(x, y + 1.0f, z + height); break;
                            case 4: cornerPos = glm::vec3(x + width, y, z); break;
                            case 5: cornerPos = glm::vec3(x, y, z + 1.0f); break;
                        }
                        
                        glm::vec3 normal = normals[face];
                        int nx = static_cast<int>(normal.x * 511.5f + 512.0f);
                        int ny = static_cast<int>(normal.y * 511.5f + 512.0f);
                        int nz = static_cast<int>(normal.z * 511.5f + 512.0f);
                        uint32_t packedNormal = (nx & 0x3FF) | ((ny & 0x3FF) << 10) | ((nz & 0x3FF) << 20);
                        
                        QuadFace quad;
                        quad.position = cornerPos;
                        quad._padding0 = 0.0f;
                        quad.width = static_cast<float>(width);
                        quad.height = static_cast<float>(height);
                        quad.packedNormal = packedNormal;
                        quad.blockType = blockType;
                        quad.faceDir = static_cast<uint32_t>(face);
                        quad.islandID = islandID;
                        
                        quads.push_back(quad);
                    }
                }
            }
        }
        
        newMesh->quads = std::move(quads);
        
        // Build voxel-to-quad mapping for direct manipulation
        for (size_t quadIdx = 0; quadIdx < newMesh->quads.size(); ++quadIdx)
        {
            const QuadFace& quad = newMesh->quads[quadIdx];
            int width = static_cast<int>(quad.width);
            int height = static_cast<int>(quad.height);
            int face = quad.faceDir;
            
            // Determine base voxel coordinates from corner position
            int baseX, baseY, baseZ;
            switch (face)
            {
                case 0: // -X
                    baseX = static_cast<int>(quad.position.x);
                    baseY = static_cast<int>(quad.position.y);
                    baseZ = static_cast<int>(quad.position.z);
                    break;
                case 1: // +X
                    baseX = static_cast<int>(quad.position.x) - 1;
                    baseY = static_cast<int>(quad.position.y);
                    baseZ = static_cast<int>(quad.position.z) - width;
                    break;
                case 2: // -Y
                    baseX = static_cast<int>(quad.position.x);
                    baseY = static_cast<int>(quad.position.y);
                    baseZ = static_cast<int>(quad.position.z);
                    break;
                case 3: // +Y
                    baseX = static_cast<int>(quad.position.x);
                    baseY = static_cast<int>(quad.position.y) - 1;
                    baseZ = static_cast<int>(quad.position.z) - height;
                    break;
                case 4: // -Z
                    baseX = static_cast<int>(quad.position.x) - width;
                    baseY = static_cast<int>(quad.position.y);
                    baseZ = static_cast<int>(quad.position.z);
                    break;
                case 5: // +Z
                    baseX = static_cast<int>(quad.position.x);
                    baseY = static_cast<int>(quad.position.y);
                    baseZ = static_cast<int>(quad.position.z) - 1;
                    break;
                default:
                    continue;
            }
            
            // Map all voxels covered by this quad
            if (face == 0 || face == 1) // X faces: width=Z, height=Y
            {
                for (int dy = 0; dy < height; ++dy)
                    for (int dz = 0; dz < width; ++dz)
                    {
                        int vx = baseX;
                        int vy = baseY + dy;
                        int vz = baseZ + dz;
                        if (vx >= 0 && vx < SIZE && vy >= 0 && vy < SIZE && vz >= 0 && vz < SIZE)
                        {
                            int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                            uint32_t key = voxelIdx * 6 + face;
                            newMesh->voxelFaceToQuadIndex[key] = static_cast<uint16_t>(quadIdx);
                        }
                    }
            }
            else if (face == 2 || face == 3) // Y faces: width=X, height=Z
            {
                for (int dz = 0; dz < height; ++dz)
                    for (int dx = 0; dx < width; ++dx)
                    {
                        int vx = baseX + dx;
                        int vy = baseY;
                        int vz = baseZ + dz;
                        if (vx >= 0 && vx < SIZE && vy >= 0 && vy < SIZE && vz >= 0 && vz < SIZE)
                        {
                            int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                            uint32_t key = voxelIdx * 6 + face;
                            newMesh->voxelFaceToQuadIndex[key] = static_cast<uint16_t>(quadIdx);
                        }
                    }
            }
            else // Z faces: width=X, height=Y
            {
                for (int dy = 0; dy < height; ++dy)
                    for (int dx = 0; dx < width; ++dx)
                    {
                        int vx = baseX + dx;
                        int vy = baseY + dy;
                        int vz = baseZ;
                        if (vx >= 0 && vx < SIZE && vy >= 0 && vy < SIZE && vz >= 0 && vz < SIZE)
                        {
                            int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                            uint32_t key = voxelIdx * 6 + face;
                            newMesh->voxelFaceToQuadIndex[key] = static_cast<uint16_t>(quadIdx);
                        }
                    }
            }
        }
        
        promise->set_value(newMesh);
    });
}

bool VoxelChunk::tryUploadPendingMesh()
{
    std::lock_guard<std::mutex> lock(m_meshMutex);
    
    if (!m_pendingMeshFuture.valid()) {
        return false; // No pending mesh
    }
    
    // Check if mesh is ready (non-blocking)
    if (m_pendingMeshFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return false; // Still building
    }
    
    // Mesh ready - retrieve and upload
    renderMesh = m_pendingMeshFuture.get();
    uploadMeshToGPU();
    
    return true; // Upload complete
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
                
                // Check if this is an OBJ block - don't merge, create 1x1x1 cube
                const BlockTypeInfo* info = BlockTypeRegistry::getInstance().getBlockType(blockType);
                bool isOBJBlock = (info && info->renderType == BlockRenderType::OBJ);
                
                // Determine merge dimensions based on face direction
                int width = 1;
                int height = 1;
                
                // Skip greedy merging for OBJ blocks
                if (isOBJBlock) {
                    visited[idx] = true;
                    addQuad(quads, static_cast<float>(x), static_cast<float>(y),
                           static_cast<float>(z), face, 1, 1, blockType);
                    continue;
                }
                
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

// INTERCHUNK CULLING ENABLED - Check neighboring chunks for proper face culling
// With 64³ chunks, remeshing neighbors is cheaper than rendering extra faces
bool VoxelChunk::isFaceExposed(int x, int y, int z, int face) const
{
    // Industry standard: 0=-X, 1=+X, 2=-Y, 3=+Y, 4=-Z, 5=+Z
    static const int dx[6] = {-1,  1,  0,  0,  0,  0};
    static const int dy[6] = { 0,  0, -1,  1,  0,  0};
    static const int dz[6] = { 0,  0,  0,  0, -1,  1};
    
    int nx = x + dx[face];
    int ny = y + dy[face];
    int nz = z + dz[face];
    
    // Intra-chunk check
    if (nx >= 0 && nx < SIZE && ny >= 0 && ny < SIZE && nz >= 0 && nz < SIZE)
    {
        return !isVoxelSolid(nx, ny, nz);
    }
    
    // Inter-chunk check: query neighboring chunk
    if (!s_islandSystem) return true;  // No island system = render boundary faces
    
    // Calculate neighbor chunk coordinate
    glm::vec3 neighborChunkCoord = m_chunkCoord;
    int localX = nx;
    int localY = ny;
    int localZ = nz;
    
    if (nx < 0) { neighborChunkCoord.x -= 1; localX = SIZE - 1; }
    else if (nx >= SIZE) { neighborChunkCoord.x += 1; localX = 0; }
    
    if (ny < 0) { neighborChunkCoord.y -= 1; localY = SIZE - 1; }
    else if (ny >= SIZE) { neighborChunkCoord.y += 1; localY = 0; }
    
    if (nz < 0) { neighborChunkCoord.z -= 1; localZ = SIZE - 1; }
    else if (nz >= SIZE) { neighborChunkCoord.z += 1; localZ = 0; }
    
    Vec3 neighborCoord(neighborChunkCoord.x, neighborChunkCoord.y, neighborChunkCoord.z);
    VoxelChunk* neighborChunk = s_islandSystem->getChunkFromIsland(m_islandID, neighborCoord);
    
    if (!neighborChunk) return true;  // No neighbor chunk = render face
    
    // Check if neighbor voxel is solid
    return !neighborChunk->isVoxelSolid(localX, localY, localZ);
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
    
    // CRITICAL: Remove old mapping entries for all voxels in this quad
    // Otherwise they'll still point to the now-deleted quad
    if (face == 0 || face == 1) // X faces: width=Z, height=Y
    {
        for (int dy = 0; dy < height; ++dy)
            for (int dz = 0; dz < width; ++dz)
            {
                int vx = baseX;
                int vy = baseY + dy;
                int vz = baseZ + dz;
                if (vx >= 0 && vx < SIZE && vy >= 0 && vy < SIZE && vz >= 0 && vz < SIZE)
                {
                    int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                    uint32_t key = voxelIdx * 6 + face;
                    mesh->voxelFaceToQuadIndex.erase(key);
                }
            }
    }
    else if (face == 2 || face == 3) // Y faces: width=X, height=Z
    {
        for (int dz = 0; dz < height; ++dz)
            for (int dx = 0; dx < width; ++dx)
            {
                int vx = baseX + dx;
                int vy = baseY;
                int vz = baseZ + dz;
                if (vx >= 0 && vx < SIZE && vy >= 0 && vy < SIZE && vz >= 0 && vz < SIZE)
                {
                    int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                    uint32_t key = voxelIdx * 6 + face;
                    mesh->voxelFaceToQuadIndex.erase(key);
                }
            }
    }
    else // Z faces: width=X, height=Y
    {
        for (int dy = 0; dy < height; ++dy)
            for (int dx = 0; dx < width; ++dx)
            {
                int vx = baseX + dx;
                int vy = baseY + dy;
                int vz = baseZ;
                if (vx >= 0 && vx < SIZE && vy >= 0 && vy < SIZE && vz >= 0 && vz < SIZE)
                {
                    int voxelIdx = vx + vy * SIZE + vz * SIZE * SIZE;
                    uint32_t key = voxelIdx * 6 + face;
                    mesh->voxelFaceToQuadIndex.erase(key);
                }
            }
    }
    
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

// ========================================
// DIRECT QUAD MANIPULATION FOR BLOCK CHANGES
// ========================================

// Set voxel using direct quad manipulation (no full remesh)
void VoxelChunk::setVoxelWithQuadManipulation(int x, int y, int z, uint8_t type)
{
    if (x < 0 || x >= SIZE || y < 0 || y >= SIZE || z < 0 || z >= SIZE)
        return;
    
    uint8_t oldType = getVoxel(x, y, z);
    if (oldType == type)
        return;
    
    // Update voxel data
    voxels[x + y * SIZE + z * SIZE * SIZE] = type;
    
    // Handle OBJ block instances
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
        // Water culling: only render if air above
        bool shouldRender = true;
        if (type == BlockID::WATER) {
            uint8_t blockAbove = (y + 1 < SIZE) ? getVoxel(x, y + 1, z) : BlockID::AIR;
            shouldRender = (blockAbove == BlockID::AIR);
        }
        
        if (shouldRender) {
            glm::vec3 pos(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f);
            m_modelInstances[type].push_back(pos);
        }
    }
    
    // If we removed a block, check if water below should now be visible
    if (oldType != BlockID::AIR && type == BlockID::AIR && y > 0) {
        uint8_t blockBelow = getVoxel(x, y - 1, z);
        if (blockBelow == BlockID::WATER) {
            const BlockTypeInfo* waterInfo = registry.getBlockType(BlockID::WATER);
            if (waterInfo && waterInfo->renderType == BlockRenderType::OBJ) {
                // Water below now has air above - make it visible
                glm::vec3 posBelow(static_cast<float>(x) + 0.5f, static_cast<float>(y - 1) + 0.5f, static_cast<float>(z) + 0.5f);
                auto& waterInstances = m_modelInstances[BlockID::WATER];
                // Check if not already present
                if (std::find(waterInstances.begin(), waterInstances.end(), posBelow) == waterInstances.end()) {
                    waterInstances.push_back(posBelow);
                }
            }
        }
    }
    
    // If we placed a block on top of water, hide the water below
    if (type != BlockID::AIR && oldType == BlockID::AIR && y > 0) {
        uint8_t blockBelow = getVoxel(x, y - 1, z);
        if (blockBelow == BlockID::WATER) {
            glm::vec3 posBelow(static_cast<float>(x), static_cast<float>(y - 1), static_cast<float>(z));
            auto it = m_modelInstances.find(BlockID::WATER);
            if (it != m_modelInstances.end()) {
                auto& instances = it->second;
                instances.erase(std::remove(instances.begin(), instances.end(), posBelow), instances.end());
            }
        }
    }
    
    auto mesh = getRenderMesh();
    if (!mesh) {
        // No mesh yet - create one
        if (m_isClientChunk) {
            generateMeshAsync();
        }
        return;
    }
    
    // Check if we have the voxel-to-quad mapping
    if (mesh->voxelFaceToQuadIndex.empty()) {
        // No mapping - need full remesh
        if (m_isClientChunk) {
            generateMeshAsync();
        }
        return;
    }
    
    // Remove old quads for this voxel
    if (oldType != 0) {
        removeVoxelQuads(x, y, z);
    }
    
    // Add new quads for this voxel (this also handles neighbor updates)
    if (type != 0) {
        addVoxelQuads(x, y, z);
    }
    
    mesh->needsGPUUpload = true;
    if (m_isClientChunk) {
        uploadMeshToGPU();
    }
}

// Remove all quads for a voxel (used when breaking blocks)
void VoxelChunk::removeVoxelQuads(int x, int y, int z)
{
    auto mesh = getRenderMesh();
    if (!mesh)
        return;
    
    int voxelIdx = x + y * SIZE + z * SIZE * SIZE;
    
    // For each face direction, find and explode any greedy quads containing this voxel
    for (int face = 0; face < 6; ++face)
    {
        uint32_t key = voxelIdx * 6 + face;
        auto it = mesh->voxelFaceToQuadIndex.find(key);
        
        if (it != mesh->voxelFaceToQuadIndex.end())
        {
            uint16_t quadIdx = it->second;
            
            // Check if this is a greedy quad (width > 1 or height > 1)
            if (quadIdx < mesh->quads.size())
            {
                QuadFace& quad = mesh->quads[quadIdx];
                bool isGreedyQuad = (quad.width > 1.0f || quad.height > 1.0f);
                
                if (isGreedyQuad)
                {
                    // Explode the greedy quad - this will:
                    // 1. Zero out the original quad
                    // 2. Remove ALL mapping entries for voxels in the quad
                    // 3. Create 1x1 quads for all remaining solid exposed voxels
                    // 4. Update the mapping for those voxels
                    explodeQuad(quadIdx);
                    
                    // NOTE: explodeQuad already erased the mapping entry, don't erase again!
                }
                else
                {
                    // This is already a 1x1 quad, just zero it out and remove mapping
                    quad.width = 0;
                    quad.height = 0;
                    mesh->voxelFaceToQuadIndex.erase(it);
                }
            }
            else
            {
                // Invalid quad index, just remove the mapping
                mesh->voxelFaceToQuadIndex.erase(it);
            }
        }
    }
    
    mesh->isExploded[voxelIdx] = false;
    
    // When breaking a block, check neighbors and add faces that are now exposed
    static const int dx[6] = {-1,  1,  0,  0,  0,  0};
    static const int dy[6] = { 0,  0, -1,  1,  0,  0};
    static const int dz[6] = { 0,  0,  0,  0, -1,  1};
    static const int oppositeFace[6] = {1, 0, 3, 2, 5, 4};
    
    for (int face = 0; face < 6; ++face)
    {
        int nx = x + dx[face];
        int ny = y + dy[face];
        int nz = z + dz[face];
        
        if (nx >= 0 && nx < SIZE && ny >= 0 && ny < SIZE && nz >= 0 && nz < SIZE)
        {
            if (isVoxelSolid(nx, ny, nz))
            {
                int oppFace = oppositeFace[face];
                int neighborVoxelIdx = nx + ny * SIZE + nz * SIZE * SIZE;
                uint32_t neighborKey = neighborVoxelIdx * 6 + oppFace;
                
                // Check if neighbor already has a face in this direction
                auto it = mesh->voxelFaceToQuadIndex.find(neighborKey);
                
                if (it == mesh->voxelFaceToQuadIndex.end())
                {
                    // Neighbor doesn't have a face pointing at us
                    // Check if it's now exposed (it should be since we just removed this block)
                    if (isFaceExposed(nx, ny, nz, oppFace))
                    {
                        // Add a new 1x1 face for the neighbor
                        // NOTE: explodeQuad might have already created this face, so check again
                        if (mesh->voxelFaceToQuadIndex.find(neighborKey) == mesh->voxelFaceToQuadIndex.end())
                        {
                            uint16_t newQuadIdx = static_cast<uint16_t>(mesh->quads.size());
                            uint8_t neighborType = getVoxel(nx, ny, nz);
                            addQuad(mesh->quads, static_cast<float>(nx), static_cast<float>(ny),
                                   static_cast<float>(nz), oppFace, 1, 1, neighborType);
                            mesh->voxelFaceToQuadIndex[neighborKey] = newQuadIdx;
                            mesh->isExploded[neighborVoxelIdx] = true;
                        }
                    }
                }
            }
        }
    }
}

// Add quads for a newly placed voxel
void VoxelChunk::addVoxelQuads(int x, int y, int z)
{
    auto mesh = getRenderMesh();
    if (!mesh)
        return;
    
    if (!isVoxelSolid(x, y, z))
        return;
    
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
    
    // When placing a block, check neighbors and explode their greedy quads if needed
    // This handles the case where a neighbor's face is now covered
    static const int dx[6] = {-1,  1,  0,  0,  0,  0};
    static const int dy[6] = { 0,  0, -1,  1,  0,  0};
    static const int dz[6] = { 0,  0,  0,  0, -1,  1};
    static const int oppositeFace[6] = {1, 0, 3, 2, 5, 4};
    
    for (int face = 0; face < 6; ++face)
    {
        int nx = x + dx[face];
        int ny = y + dy[face];
        int nz = z + dz[face];
        
        if (nx >= 0 && nx < SIZE && ny >= 0 && ny < SIZE && nz >= 0 && nz < SIZE)
        {
            if (isVoxelSolid(nx, ny, nz))
            {
                int oppFace = oppositeFace[face];
                int neighborVoxelIdx = nx + ny * SIZE + nz * SIZE * SIZE;
                uint32_t neighborKey = neighborVoxelIdx * 6 + oppFace;
                
                auto it = mesh->voxelFaceToQuadIndex.find(neighborKey);
                if (it != mesh->voxelFaceToQuadIndex.end())
                {
                    uint16_t quadIdx = it->second;
                    
                    // The neighbor has a face pointing at us
                    // If it's a greedy quad, explode it and recreate individual faces
                    if (quadIdx < mesh->quads.size())
                    {
                        // Check dimensions before potentially reallocating
                        float quadWidth = mesh->quads[quadIdx].width;
                        float quadHeight = mesh->quads[quadIdx].height;
                        bool isGreedyQuad = (quadWidth > 1.0f || quadHeight > 1.0f);
                        
                        if (isGreedyQuad)
                        {
                            // Explode the greedy quad - it will recreate only exposed faces
                            // NOTE: This modifies mesh->quads and may invalidate references
                            explodeQuad(quadIdx);
                        }
                        else
                        {
                            // Single face that's now covered - just remove it
                            if (!isFaceExposed(nx, ny, nz, oppFace))
                            {
                                mesh->quads[quadIdx].width = 0;
                                mesh->quads[quadIdx].height = 0;
                                mesh->voxelFaceToQuadIndex.erase(it);
                            }
                        }
                    }
                }
            }
        }
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






