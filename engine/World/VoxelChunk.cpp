// VoxelChunk.cpp - 32x32x32 dynamic physics-enabled voxel chunks
#include "VoxelChunk.h"
#include "BlockType.h"

#include "../Time/DayNightController.h"  // For dynamic sun direction

#include <algorithm>
#include <chrono>
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
    
    // Initialize collision mesh with empty shared_ptr
    collisionMesh = std::make_shared<CollisionMesh>();
    
    mesh.needsUpdate = true;
    
    // Initialize light maps
    for (int face = 0; face < 6; ++face) {
        lightMaps.getFaceMap(face).textureHandle = 0;
        // Fill with default lighting (mid-gray = normal lighting)
        std::fill(lightMaps.getFaceMap(face).data.begin(), lightMaps.getFaceMap(face).data.end(), 128);
    }
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
    lightingDirty = true;  // NEW: Mark lighting as needing update when voxels change
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
    
    auto startTime = std::chrono::high_resolution_clock::now();
    (void)startTime; // Reserved for future timing metrics
    
    std::lock_guard<std::mutex> lock(meshMutex);
    mesh.quads.clear();
    clearAllModelInstances();  // Clear all model instances before scanning

    auto grassScanStart = std::chrono::high_resolution_clock::now();
    (void)grassScanStart; // Reserved for future timing metrics
    
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
                    addModelInstance(blockID, instancePos);
                }
            }
        }
    }
    
    auto greedyMeshStart = std::chrono::high_resolution_clock::now();
    (void)greedyMeshStart; // Reserved for future timing metrics

    // Simple mesh generation - one quad per exposed face
    generateSimpleMesh();
    
    auto collisionStart = std::chrono::high_resolution_clock::now();
    (void)collisionStart; // Reserved for future timing metrics
    
    // Build collision mesh immediately after generating vertices
    buildCollisionMeshFromVertices();
    
    auto lightingStart = std::chrono::high_resolution_clock::now();
    (void)lightingStart; // Reserved for future timing metrics
    
    mesh.needsUpdate = true;
    meshDirty = false;
    
    // NEW: Mark lighting as needing recalculation since geometry changed
    lightingDirty = true;
    
    // CONDITIONAL LIGHTING GENERATION: Only generate if requested (skip during world gen)
    if (generateLighting) {
        generatePerFaceLightMaps();
        
        // Only initialize light maps if they're empty (preserve generated lighting data)
        for (int face = 0; face < 6; ++face) {
            FaceLightMap& faceMap = lightMaps.getFaceMap(face);
            if (faceMap.data.empty()) {
                faceMap.data.resize(32 * 32 * 3);
                std::fill(faceMap.data.begin(), faceMap.data.end(), 255); // Full white default for unlit chunks
            }
        }
        
        // Mark lighting as clean since we just generated it
        lightingDirty = false;
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    (void)endTime; // Reserved for future timing metrics
    
    // Note: Mesh generation profiling disabled to reduce console spam
    // Timing: scan, greedy meshing, collision, lighting
    // Can be re-enabled for performance debugging if needed
    
    // Note: updateLightMapTextures() will be called during rendering when OpenGL context is available
}

void VoxelChunk::buildCollisionMeshFromVertices()
{
    // Build collision mesh directly from quadFaces (no need for separate storage)
    auto newMesh = std::make_shared<CollisionMesh>();

    for (const auto& quad : mesh.quads)
    {
        newMesh->faces.push_back({
            quad.position,
            quad.normal,
            quad.width,
            quad.height
        });
    }
    
    // Atomically update the collision mesh - safe for concurrent reads
    setCollisionMesh(newMesh);
}

void VoxelChunk::buildCollisionMesh()
{
    // Wrapper for backwards compatibility (called by ConnectivityTest)
    std::lock_guard<std::mutex> lock(meshMutex);
    buildCollisionMeshFromVertices();
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

    if (dist < 64.0f)
        return 0;  // High detail
    else if (dist < 128.0f)
        return 1;  // Medium detail
    else
        return 2;  // Low detail
}

bool VoxelChunk::shouldRender(const Vec3& cameraPos, float maxDistance) const
{
    Vec3 chunkCenter(SIZE * 0.5f, SIZE * 0.5f, SIZE * 0.5f);
    Vec3 distance = cameraPos - chunkCenter;
    float dist =
        std::sqrt(distance.x * distance.x + distance.y * distance.y + distance.z * distance.z);
    return dist <= maxDistance;
}

// Simple hash-based value noise in [-1,1] for (x,z)
static inline float vc_hashToUnit(int xi, int zi, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(xi) * 374761393u ^ static_cast<uint32_t>(zi) * 668265263u ^ (seed * 0x9E3779B9u);
    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
    float u = (h & 0x00FFFFFFu) / 16777215.0f; // [0,1]
    return u * 2.0f - 1.0f; // [-1,1]
}

// Smooth noise function that interpolates between grid points to avoid patterns
static inline float vc_smoothNoise(float x, float z, uint32_t seed)
{
    const float freq = 1.0f / 12.0f; // sample at same frequency but smoothly
    
    // Get the fractional and integer parts
    float fx = x * freq;
    float fz = z * freq;
    int x0 = static_cast<int>(std::floor(fx));
    int z0 = static_cast<int>(std::floor(fz));
    int x1 = x0 + 1;
    int z1 = z0 + 1;
    
    // Get fractional parts for interpolation
    float sx = fx - x0;
    float sz = fz - z0;
    
    // Sample the four corners
    float n00 = vc_hashToUnit(x0, z0, seed);
    float n10 = vc_hashToUnit(x1, z0, seed);
    float n01 = vc_hashToUnit(x0, z1, seed);
    float n11 = vc_hashToUnit(x1, z1, seed);
    
    // Smooth interpolation (cosine interpolation for smoother result)
    float ix = 0.5f * (1.0f - std::cos(sx * 3.14159265f));
    float iz = 0.5f * (1.0f - std::cos(sz * 3.14159265f));
    
    // Bilinear interpolation
    float nx0 = n00 * (1.0f - ix) + n10 * ix;
    float nx1 = n01 * (1.0f - ix) + n11 * ix;
    float result = nx0 * (1.0f - iz) + nx1 * iz;
    
    return result; // returns [-1,1]
}

// Light mapping utilities
float VoxelChunk::computeAmbientOcclusion(int x, int y, int z, int face) const
{
    // Simple ambient occlusion calculation based on neighboring voxels
    // Higher values = more occlusion (darker), range [0.0, 1.0]
    
    static const int faceOffsets[6][3] = {
        {0, 0, 1},   // +Z (front)
        {0, 0, -1},  // -Z (back)
        {0, 1, 0},   // +Y (top)
        {0, -1, 0},  // -Y (bottom)
        {1, 0, 0},   // +X (right)
        {-1, 0, 0}   // -X (left)
    };
    
    // Get the face normal to determine which neighbors to check
    int fx = faceOffsets[face][0];
    int fy = faceOffsets[face][1];
    int fz = faceOffsets[face][2];
    
    // Check 8 neighboring positions around this face
    float occlusion = 0.0f;
    int sampleCount = 0;
    
    // Create a 3x3 grid of offsets perpendicular to the face normal
    for (int du = -1; du <= 1; du++)
    {
        for (int dv = -1; dv <= 1; dv++)
        {
            if (du == 0 && dv == 0) continue; // Skip center
            
            int checkX = x, checkY = y, checkZ = z;
            
            // Map du,dv to world space based on face orientation
            if (face <= 1) // Z faces: map to X,Y
            {
                checkX += du;
                checkY += dv;
            }
            else if (face <= 3) // Y faces: map to X,Z
            {
                checkX += du;
                checkZ += dv;
            }
            else // X faces: map to Z,Y
            {
                checkZ += du;
                checkY += dv;
            }
            
            // Also offset by face direction to check the neighboring voxels
            checkX += fx;
            checkY += fy;
            checkZ += fz;
            
            // Sample the voxel at this position
            if (getVoxel(checkX, checkY, checkZ) != 0)
            {
                occlusion += 0.15f; // Each solid neighbor adds occlusion
            }
            sampleCount++;
        }
    }
    
    // Return ambient lighting factor (1.0 = bright, 0.0 = dark)
    // Clamp to reasonable range for subtle effect
    return std::max(0.3f, 1.0f - occlusion);
}

void VoxelChunk::generatePerFaceLightMaps()
{
    // Reduced debug output for performance
    // Generate separate light maps for each face direction with proper inter-chunk raycasting
    
    const int LIGHTMAP_SIZE = FaceLightMap::LIGHTMAP_SIZE;
    const Vec3 sunDirection = g_dayNightController ? g_dayNightController->getSunDirection() : Vec3(0.3f, 0.8f, 0.5f).normalized();  // Use dynamic sun direction, fallback for early init
    const float sunIntensity = 1.2f;
    const float ambientIntensity = 0.0f;  // DISABLED for lightmap testing - was 0.2f
    
    // Face normals - MUST match addGreedyQuad face ordering
    // 0=-Y (bottom), 1=+Y (top), 2=-Z (back), 3=+Z (front), 4=-X (left), 5=+X (right)
    Vec3 faceNormals[6] = {
        Vec3(0, -1, 0),  // 0: -Y (bottom)
        Vec3(0, 1, 0),   // 1: +Y (top)
        Vec3(0, 0, -1),  // 2: -Z (back)
        Vec3(0, 0, 1),   // 3: +Z (front)
        Vec3(-1, 0, 0),  // 4: -X (left)
        Vec3(1, 0, 0)    // 5: +X (right)
    };
    
    // Generate a light map for each face direction
    for (int faceIndex = 0; faceIndex < 6; ++faceIndex) {
        FaceLightMap& faceMap = lightMaps.getFaceMap(faceIndex);
        faceMap.data.resize(LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3);
        
        Vec3 faceNormal = faceNormals[faceIndex];
        
        for (int v = 0; v < LIGHTMAP_SIZE; v++) {
            for (int u = 0; u < LIGHTMAP_SIZE; u++) {
                float normalizedU = static_cast<float>(u) / (LIGHTMAP_SIZE - 1);  // 0 to 1
                float normalizedV = static_cast<float>(v) / (LIGHTMAP_SIZE - 1);  // 0 to 1
                
                // Calculate world position for this light map texel
                Vec3 worldPos = calculateWorldPositionFromLightMapUV(faceIndex, normalizedU, normalizedV);
                
                // Calculate ray start position (slightly offset from surface in face normal direction)
                Vec3 rayStart = worldPos + faceNormal * 0.1f;
                
                // Use full inter-chunk/inter-island raycasting for shadow occlusion
                // This will check occlusion across chunk boundaries and between islands
                bool isOccluded = performSunRaycast(rayStart, sunDirection, SIZE * 3.0f);  // Extended range for inter-chunk
                
                // Pure shadow map: 1.0 = lit (not occluded), 0.0 = shadowed (occluded)
                // No directional/lambert lighting - just binary shadow occlusion
                float finalLight = isOccluded ? 0.0f : 1.0f;
                
                // Store in light map (clamp to valid range)
                int index = (v * LIGHTMAP_SIZE + u) * 3;
                uint8_t lightByte = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, finalLight * 255.0f)));
                faceMap.data[index] = lightByte;
                faceMap.data[index + 1] = lightByte;
                faceMap.data[index + 2] = lightByte;
            }
        }
    }
}

// Helper function to calculate world position from light map UV coordinates
// IMPORTANT: Face index and (u,v)->axis mapping MUST match addGreedyQuad() lightmap mapping
// Direction indices:
// 0=+X (U=Y,V=Z), 1=-X (U=Z,V=Y), 2=+Y (U=Z,V=X), 3=-Y (U=X,V=Z), 4=+Z (U=X,V=Y), 5=-Z (U=Y,V=X)
Vec3 VoxelChunk::calculateWorldPositionFromLightMapUV(int faceIndex, float u, float v) const
{
    float worldU = u * SIZE;
    float worldV = v * SIZE;

    switch (faceIndex)
    {
        case 0: // +X: X fixed at SIZE
            return Vec3(SIZE - 0.5f, worldU, worldV);                   // U->Y, V->Z
        case 1: // -X: X fixed at 0
            return Vec3(0.5f,        worldV, worldU);                   // U->Z, V->Y
        case 2: // +Y: Y fixed at SIZE
            return Vec3(worldV,      SIZE - 0.5f, worldU);              // U->Z, V->X
        case 3: // -Y: Y fixed at 0
            return Vec3(worldU,      0.5f,        worldV);              // U->X, V->Z
        case 4: // +Z: Z fixed at SIZE
            return Vec3(worldU,      worldV,      SIZE - 0.5f);         // U->X, V->Y
        case 5: // -Z: Z fixed at 0
            return Vec3(worldV,      worldU,      0.5f);                // U->Y, V->X
        default:
            return Vec3(SIZE * 0.5f, SIZE * 0.5f, SIZE * 0.5f);
    }
}

// Helper function to perform raycast toward sun for occlusion testing (local chunk only)
bool VoxelChunk::performLocalSunRaycast(const Vec3& rayStart, const Vec3& sunDirection, float maxDistance) const
{
    // Perform a simple raycast through the voxel grid toward the sun (local chunk only)
    // Returns true if ray hits a solid voxel (occluded), false if clear path
    
    const float stepSize = 0.4f;  // Smaller step size for more accuracy
    const int maxSteps = static_cast<int>(maxDistance / stepSize);
    
    Vec3 rayPos = rayStart;
    Vec3 rayStep = sunDirection * stepSize;
    
    for (int step = 0; step < maxSteps; ++step) {
        rayPos = rayPos + rayStep;
        
        // Check if ray position is outside chunk bounds
        if (rayPos.x < 0 || rayPos.x >= SIZE || 
            rayPos.y < 0 || rayPos.y >= SIZE || 
            rayPos.z < 0 || rayPos.z >= SIZE) {
            // Ray exited chunk bounds - no local occlusion (floating island characteristic)
            return false;
        }
        
        // Sample voxel at current ray position
        int voxelX = static_cast<int>(rayPos.x);
        int voxelY = static_cast<int>(rayPos.y);
        int voxelZ = static_cast<int>(rayPos.z);
        
        // Clamp to valid bounds
        voxelX = std::max(0, std::min(SIZE - 1, voxelX));
        voxelY = std::max(0, std::min(SIZE - 1, voxelY));
        voxelZ = std::max(0, std::min(SIZE - 1, voxelZ));
        
        uint8_t voxel = getVoxel(voxelX, voxelY, voxelZ);
        if (voxel != 0) {
            // Hit a solid voxel - ray is locally occluded
            return true;
        }
    }
    
    // Ray traveled maximum distance without hitting anything locally
    return false;
}

// Helper function to perform raycast toward sun for occlusion testing
bool VoxelChunk::performSunRaycast(const Vec3& rayStart, const Vec3& sunDirection, float maxDistance) const
{
    // Enhanced raycast that checks across multiple islands for proper inter-chunk lighting
    return performInterIslandSunRaycast(rayStart, sunDirection, maxDistance);
}

// Helper function to perform inter-island raycast for lighting occlusion
bool VoxelChunk::performInterIslandSunRaycast(const Vec3& rayStart, const Vec3& sunDirection, float maxDistance) const
{
    // Optimized raycast that can span multiple islands for proper lighting
    
    const float stepSize = 1.0f;  // Larger step size for performance
    const int maxSteps = static_cast<int>(maxDistance / stepSize);
    
    Vec3 rayPos = rayStart;
    Vec3 rayStep = sunDirection * stepSize;
    
    // We need to find which island this chunk belongs to first
    extern IslandChunkSystem g_islandSystem;
    
    // Find our island ID by checking all islands
    uint32_t currentIslandID = 0;
    const auto& islands = g_islandSystem.getIslands();
    
    for (const auto& [islandID, island] : islands) {
        for (const auto& [chunkCoord, chunk] : island.chunks) {
            if (chunk.get() == this) {
                currentIslandID = islandID;
                break;
            }
        }
        if (currentIslandID != 0) break;
    }
    
    if (currentIslandID == 0) {
        // Fallback to local raycast if we can't find our island
        return performLocalSunRaycast(rayStart, sunDirection, maxDistance);
    }
    
    // Get our island's center for coordinate conversion
    Vec3 islandCenter = g_islandSystem.getIslandCenter(currentIslandID);
    
    // Limit raycast steps for performance - use smaller range for inter-island checks
    const int limitedSteps = std::min(maxSteps, static_cast<int>(SIZE * 1.5f / stepSize));
    
    for (int step = 0; step < limitedSteps; ++step) {
        rayPos = rayPos + rayStep;
        
        // First check local island (fastest)
        if (rayPos.x >= 0 && rayPos.x < SIZE && 
            rayPos.y >= 0 && rayPos.y < SIZE && 
            rayPos.z >= 0 && rayPos.z < SIZE) {
            
            int voxelX = static_cast<int>(rayPos.x);
            int voxelY = static_cast<int>(rayPos.y);
            int voxelZ = static_cast<int>(rayPos.z);
            
            uint8_t voxel = getVoxel(voxelX, voxelY, voxelZ);
            if (voxel != 0) {
                return true;  // Hit local voxel
            }
        } else {
            // Only check nearby islands for efficiency
            Vec3 worldRayPos = rayPos + islandCenter;
            
            // Check only the 2 closest islands to avoid O(n²) complexity
            int islandsChecked = 0;
            for (const auto& [otherIslandID, otherIsland] : islands) {
                if (otherIslandID == currentIslandID) continue;  // Skip our own island
                if (++islandsChecked > 2) break;  // Limit to 2 nearby islands
                
                // Convert world position to island-relative position
                Vec3 otherIslandCenter = g_islandSystem.getIslandCenter(otherIslandID);
                Vec3 islandRelativePos = worldRayPos - otherIslandCenter;
                
                // Quick distance check - skip if too far
                float distToIsland = islandRelativePos.length();
                if (distToIsland > SIZE * 2.0f) continue;
                
                // Query voxel from this island
                uint8_t voxel = g_islandSystem.getVoxelFromIsland(otherIslandID, islandRelativePos);
                if (voxel != 0) {
                    return true;  // Ray is occluded by another island's voxel
                }
            }
        }
    }
    
    // Ray traveled limited distance without hitting anything significant
    return false;
}

void VoxelChunk::updateLightMapTextures()
{
    // Safety check: Only create textures when we have an OpenGL context
    // This should only be called from the rendering thread
    
    for (int faceIndex = 0; faceIndex < 6; ++faceIndex) {
        FaceLightMap& faceMap = lightMaps.getFaceMap(faceIndex);
        
        // Check if lightmap data exists and is the correct size
        size_t expectedSize = FaceLightMap::LIGHTMAP_SIZE * FaceLightMap::LIGHTMAP_SIZE * 3;
        if (faceMap.data.empty() || faceMap.data.size() != expectedSize)
        {
            std::cerr << "⚠️  Face " << faceIndex << " lightmap data invalid! Size: " 
                      << faceMap.data.size() << " (expected " << expectedSize << ")" << std::endl;
            // Initialize with default white lighting if missing
            faceMap.data.resize(expectedSize, 255);
        }
        
        // Generate OpenGL texture if it doesn't exist
        if (faceMap.textureHandle == 0)
        {
            glGenTextures(1, &faceMap.textureHandle);
            
            // Check for OpenGL errors
            GLenum error = glGetError();
            if (error != GL_NO_ERROR)
            {
                std::cerr << "Failed to generate face " << faceIndex << " light map texture, OpenGL error: " << error << std::endl;
                continue;
            }
        }
        
        // Upload light map data to GPU
        glBindTexture(GL_TEXTURE_2D, faceMap.textureHandle);
        
        // Clear any previous errors
        while (glGetError() != GL_NO_ERROR);
        
        // Validate data before upload
        if (faceMap.data.empty() || faceMap.data.data() == nullptr)
        {
            std::cerr << "❌ Face " << faceIndex << " has null/empty data!" << std::endl;
            continue;
        }
        
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, FaceLightMap::LIGHTMAP_SIZE, FaceLightMap::LIGHTMAP_SIZE, 0, GL_RGB, GL_UNSIGNED_BYTE, faceMap.data.data());
        
        GLenum uploadError = glGetError();
        if (uploadError != GL_NO_ERROR)
        {
            std::cerr << "❌ glTexImage2D failed for face " << faceIndex 
                      << " error: " << uploadError 
                      << " size: " << FaceLightMap::LIGHTMAP_SIZE 
                      << " data size: " << faceMap.data.size() << std::endl;
            continue;
        }
        
        // Set texture parameters for light maps
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        // Check for any OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            std::cerr << "OpenGL error in updateLightMapTextures for face " << faceIndex << ": " << error << std::endl;
        }
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
}

void VoxelChunk::markLightMapsDirty() {
    // Mark all face light map textures as needing recreation
    for (int face = 0; face < 6; ++face) {
        lightMaps.getFaceMap(face).textureHandle = 0; // Force recreation
    }
}

bool VoxelChunk::hasValidLightMaps() const {
    // Check if all lightmap faces have valid texture handles
    for (int face = 0; face < 6; ++face) {
        if (lightMaps.getFaceMap(face).textureHandle == 0) {
            return false;
        }
    }
    return true;
}

bool VoxelChunk::hasLightMapData() const {
    // Check if lightmap data is present (even if textures aren't created yet)
    for (int face = 0; face < 6; ++face) {
        if (lightMaps.getFaceMap(face).data.empty()) {
            return false;
        }
    }
    return true;
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

void VoxelChunk::generateSimpleMesh()
{
    PROFILE_SCOPE("VoxelChunk::generateSimpleMesh");
    
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
        
        // For each slice perpendicular to the normal direction
        for (int n = 0; n < nn; ++n)
        {
            // Build a mask for this slice
            // mask[u + v * nu] = blockID if face should be generated, 0 otherwise
            std::vector<uint8_t> mask(nu * nv, 0);
            
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
                        mask[u + v * nu] = getVoxel(x, y, z);
                    }
                }
            }
            
            // Greedy meshing: merge adjacent quads into rectangles
            for (int v = 0; v < nv; ++v)
            {
                for (int u = 0; u < nu; )
                {
                    uint8_t blockType = mask[u + v * nu];
                    if (blockType == 0)
                    {
                        ++u;
                        continue;
                    }
                    
                    // Find the width (u direction) of this quad
                    int width = 1;
                    while (u + width < nu && mask[u + width + v * nu] == blockType)
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
                            if (mask[u + k + (v + height) * nu] != blockType)
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
                    addGreedyQuad(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
                                  faceDir, width, height, blockType);
                    
                    // Clear the mask for the merged area
                    for (int h = 0; h < height; ++h)
                    {
                        for (int w = 0; w < width; ++w)
                        {
                            mask[u + w + (v + h) * nu] = 0;
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

void VoxelChunk::addModelInstance(uint8_t blockID, const Vec3& position)
{
    m_modelInstances[blockID].push_back(position);
}

void VoxelChunk::clearModelInstances(uint8_t blockID)
{
    auto it = m_modelInstances.find(blockID);
    if (it != m_modelInstances.end())
        it->second.clear();
}

void VoxelChunk::clearAllModelInstances()
{
    m_modelInstances.clear();
}

// Greedy quad generation for merged rectangles
void VoxelChunk::addGreedyQuad(float x, float y, float z, int face, int width, int height, uint8_t blockType)
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
    
    // Calculate lightmap UV coordinates based on quad position within chunk
    // Lightmap is 32x32 covering the entire 16x16 chunk face
    // Map quad center to lightmap space (0-1 range)
    float lightmapU = 0.5f;
    float lightmapV = 0.5f;
    
    switch (face)
    {
        case 0: // -Y (bottom) - map XZ to UV
            lightmapU = (x + w * 0.5f) / SIZE;
            lightmapV = (z + h * 0.5f) / SIZE;
            break;
        case 1: // +Y (top) - map XZ to UV
            lightmapU = (x + w * 0.5f) / SIZE;
            lightmapV = (z + h * 0.5f) / SIZE;
            break;
        case 2: // -Z (back) - map XY to UV
            lightmapU = (x + w * 0.5f) / SIZE;
            lightmapV = (y + h * 0.5f) / SIZE;
            break;
        case 3: // +Z (front) - map XY to UV
            lightmapU = (x + w * 0.5f) / SIZE;
            lightmapV = (y + h * 0.5f) / SIZE;
            break;
        case 4: // -X (left) - map ZY to UV
            lightmapU = (z + w * 0.5f) / SIZE;
            lightmapV = (y + h * 0.5f) / SIZE;
            break;
        case 5: // +X (right) - map ZY to UV
            lightmapU = (z + w * 0.5f) / SIZE;
            lightmapV = (y + h * 0.5f) / SIZE;
            break;
    }
    
    quadFace.lightmapU = lightmapU;
    quadFace.lightmapV = lightmapV;
    quadFace.blockType = blockType;
    quadFace.faceDir = static_cast<uint8_t>(face);
    quadFace.padding = 0;
    
    mesh.quads.push_back(quadFace);
}
