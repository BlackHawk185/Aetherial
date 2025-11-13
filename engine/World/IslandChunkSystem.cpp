// IslandChunkSystem.cpp - Implementation of physics-driven chunking
#include "IslandChunkSystem.h"

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <memory>
#include <string>
#include <chrono>
#include <random>
#include <unordered_set>
#include <queue>

#include "VoxelChunk.h"
#include "BlockType.h"
#include "TreeGenerator.h"
#include "../Rendering/GPUMeshQueue.h"
#include "../Profiling/Profiler.h"
#include "../libs/FastNoiseSIMD/FastNoiseSIMD.h"
#include "../Rendering/InstancedQuadRenderer.h"
#include "../Rendering/ModelInstanceRenderer.h"
#include "../Culling/Frustum.h"
#include "../../libs/FastNoiseLite/FastNoiseLite.h"

IslandChunkSystem g_islandSystem;

IslandChunkSystem::IslandChunkSystem()
{
    // Initialize system
    // Seed random number generator for island velocity
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    
    // Set static pointer so VoxelChunk can access island system for inter-chunk culling
    VoxelChunk::s_islandSystem = this;
}

IslandChunkSystem::~IslandChunkSystem()
{
    // Clear static pointer before destruction
    VoxelChunk::s_islandSystem = nullptr;
    
    // Clean up all islands
    // Collect IDs first to avoid iterator invalidation
    std::vector<uint32_t> islandIDs;
    for (auto& [id, island] : m_islands)
    {
        islandIDs.push_back(id);
    }
    
    // Now safely destroy all islands
    for (uint32_t id : islandIDs)
    {
        destroyIsland(id);
    }
}

uint32_t IslandChunkSystem::createIsland(const Vec3& physicsCenter)
{
    return createIsland(physicsCenter, 0);  // 0 = auto-assign ID
}

uint32_t IslandChunkSystem::createIsland(const Vec3& physicsCenter, uint32_t forceIslandID)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    
    // Determine island ID
    uint32_t islandID;
    if (forceIslandID == 0)
    {
        // Auto-assign: use next available ID
        islandID = m_nextIslandID++;
    }
    else
    {
        // Force specific ID (for network sync)
        islandID = forceIslandID;
        // Update next ID to avoid collisions
        if (forceIslandID >= m_nextIslandID)
        {
            m_nextIslandID = forceIslandID + 1;
        }
    }
    
    // Create the island
    FloatingIsland& island = m_islands[islandID];
    island.islandID = islandID;
    island.physicsCenter = physicsCenter;
    island.needsPhysicsUpdate = true;
    island.acceleration = Vec3(0.0f, 0.0f, 0.0f);
    
    // Set initial random drift velocity for natural island movement
    // Use island position as seed for deterministic random values
    uint32_t seedX = static_cast<uint32_t>(std::abs(physicsCenter.x * 73856093.0f));
    uint32_t seedY = static_cast<uint32_t>(std::abs(physicsCenter.y * 19349663.0f));
    uint32_t seedZ = static_cast<uint32_t>(std::abs(physicsCenter.z * 83492791.0f));
    std::mt19937 rng(seedX ^ seedY ^ seedZ);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    
    island.velocity = Vec3(
        dist(rng),  // Random X drift
        dist(rng) * 0.3f,  // Reduced Y drift (mostly horizontal movement)
        dist(rng)   // Random Z drift
    );
    
    std::cout << "[ISLAND] Created island " << islandID 
              << " with drift velocity (" << island.velocity.x 
              << ", " << island.velocity.y 
              << ", " << island.velocity.z << ")" << std::endl;
    
    return islandID;
}

void IslandChunkSystem::destroyIsland(uint32_t islandID)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto it = m_islands.find(islandID);
    if (it == m_islands.end())
        return;

    m_islands.erase(it);
}

FloatingIsland* IslandChunkSystem::getIsland(uint32_t islandID)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto it = m_islands.find(islandID);
    return (it != m_islands.end()) ? &it->second : nullptr;
}

const FloatingIsland* IslandChunkSystem::getIsland(uint32_t islandID) const
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto it = m_islands.find(islandID);
    return (it != m_islands.end()) ? &it->second : nullptr;
}

Vec3 IslandChunkSystem::getIslandCenter(uint32_t islandID) const
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto it = m_islands.find(islandID);
    if (it != m_islands.end())
    {
        return it->second.physicsCenter;
    }
    return Vec3(0.0f, 0.0f, 0.0f);  // Return zero vector if island not found
}

Vec3 IslandChunkSystem::getIslandVelocity(uint32_t islandID) const
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto it = m_islands.find(islandID);
    if (it != m_islands.end())
    {
        return it->second.velocity;
    }
    return Vec3(0.0f, 0.0f, 0.0f);  // Return zero velocity if island not found
}

void IslandChunkSystem::addChunkToIsland(uint32_t islandID, const Vec3& chunkCoord)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto itIsl = m_islands.find(islandID);
    FloatingIsland* island = (itIsl != m_islands.end()) ? &itIsl->second : nullptr;
    if (!island)
        return;

    // Check if chunk already exists
    if (island->chunks.find(chunkCoord) != island->chunks.end())
        return;

    // Create new chunk and set island context
    auto newChunk = std::make_unique<VoxelChunk>();
    newChunk->setIslandContext(islandID, chunkCoord);
    newChunk->setIsClient(m_isClient);  // Inherit client flag from island system
    island->chunks[chunkCoord] = std::move(newChunk);
}

void IslandChunkSystem::removeChunkFromIsland(uint32_t islandID, const Vec3& chunkCoord)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto itIsl = m_islands.find(islandID);
    FloatingIsland* island = (itIsl != m_islands.end()) ? &itIsl->second : nullptr;
    if (!island)
        return;

    auto it = island->chunks.find(chunkCoord);
    if (it != island->chunks.end())
    {
        island->chunks.erase(it);
    }
}

VoxelChunk* IslandChunkSystem::getChunkFromIsland(uint32_t islandID, const Vec3& chunkCoord)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto itIsl = m_islands.find(islandID);
    FloatingIsland* island = (itIsl != m_islands.end()) ? &itIsl->second : nullptr;
    if (!island)
        return nullptr;

    auto it = island->chunks.find(chunkCoord);
    if (it != island->chunks.end())
    {
        return it->second.get();
    }

    return nullptr;
}

void IslandChunkSystem::generateFloatingIslandOrganic(uint32_t islandID, uint32_t seed, float radius, BiomeType biome)
{
    PROFILE_SCOPE("IslandChunkSystem::generateFloatingIslandOrganic");
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    FloatingIsland* island = getIsland(islandID);
    if (!island)
        return;

    // Get biome palette for block selection
    BiomeSystem biomeSystem;
    BiomePalette palette = biomeSystem.getPalette(biome);
    
    std::cout << "[BIOME] Island " << islandID << " - " << biomeSystem.getBiomeName(biome) << std::endl;

    // WORLDGEN OPTIMIZATION: Cache chunk map to avoid mutex locking on every voxel set
    // We can safely access island->chunks without locking during single-threaded generation
    auto& chunkMap = island->chunks;
    
    // Helper lambda: Direct voxel lookup without mutex (worldgen is single-threaded)
    // Used throughout worldgen to avoid getBlockIDInIsland() which has mutex locks
    auto getVoxelDirect = [&](const Vec3& pos) -> uint8_t {
        int chunkX = static_cast<int>(std::floor(pos.x / VoxelChunk::SIZE));
        int chunkY = static_cast<int>(std::floor(pos.y / VoxelChunk::SIZE));
        int chunkZ = static_cast<int>(std::floor(pos.z / VoxelChunk::SIZE));
        Vec3 chunkCoord(chunkX, chunkY, chunkZ);
        
        auto it = chunkMap.find(chunkCoord);
        if (it == chunkMap.end() || !it->second) return 0;
        
        int localX = static_cast<int>(std::floor(pos.x)) - (chunkX * VoxelChunk::SIZE);
        int localY = static_cast<int>(std::floor(pos.y)) - (chunkY * VoxelChunk::SIZE);
        int localZ = static_cast<int>(std::floor(pos.z)) - (chunkZ * VoxelChunk::SIZE);
        
        return it->second->getVoxel(localX, localY, localZ);
    };
    
    // **NOISE CONFIGURATION**
    float densityThreshold = 0.35f;
    float baseHeightRatio = 0.075f;  // Height as a factor of radius (reduced for flatter islands)
    float noise3DFrequency = 0.02f;
    float noise2DFrequency = 0.015f;
    int fractalOctaves = 2;
    float fractalGain = 0.4f;
    
    // **ENVIRONMENT OVERRIDES** for noise parameters
    const char* freq3DEnv = std::getenv("NOISE_FREQ_3D");
    if (freq3DEnv) noise3DFrequency = std::stof(freq3DEnv);
    
    const char* freq2DEnv = std::getenv("NOISE_FREQ_2D");
    if (freq2DEnv) noise2DFrequency = std::stof(freq2DEnv);
    
    const char* thresholdEnv = std::getenv("NOISE_THRESHOLD");
    if (thresholdEnv) densityThreshold = std::stof(thresholdEnv);
    
    auto voxelGenStart = std::chrono::high_resolution_clock::now();
    
    // **PRE-GENERATE NOISE MAP**
    // Generate 3D noise grid for entire island bounds ONCE instead of per-voxel lookups
    int islandHeight = static_cast<int>(radius * baseHeightRatio);
    
    // Sample every other block (2x2x2 grid) for 8x memory reduction + 8x faster generation
    // Trilinear interpolation fills in the gaps with imperceptible quality loss
    constexpr int NOISE_SAMPLE_RATE = 2;
    int gridSizeX = (static_cast<int>(radius) * 2) / NOISE_SAMPLE_RATE;
    int gridSizeY = (islandHeight * 2) / NOISE_SAMPLE_RATE;
    int gridSizeZ = (static_cast<int>(radius) * 2) / NOISE_SAMPLE_RATE;
    
    // Offset to handle negative coordinates
    int gridOffsetX = gridSizeX / 2;
    int gridOffsetY = gridSizeY / 2;
    int gridOffsetZ = gridSizeZ / 2;
    
    auto noiseMapStart = std::chrono::high_resolution_clock::now();
    
    // Use uint8_t (0-255) instead of float to save 4x memory
    std::vector<uint8_t> noiseMap(gridSizeX * gridSizeY * gridSizeZ);
    
    std::cout << "   └─ Allocating Noise Map: " << gridSizeX << "x" << gridSizeY << "x" << gridSizeZ
              << " (" << (noiseMap.size() / (1024 * 1024)) << " MB)" << std::endl;
    
    // Use FastNoiseSIMD for SIMD-accelerated noise generation (4-8x faster)
    FastNoiseSIMD* simdNoise = FastNoiseSIMD::NewFastNoiseSIMD(seed);
    simdNoise->SetNoiseType(FastNoiseSIMD::PerlinFractal);
    simdNoise->SetFrequency(noise3DFrequency);
    simdNoise->SetFractalType(FastNoiseSIMD::FBM);
    simdNoise->SetFractalOctaves(fractalOctaves);
    simdNoise->SetFractalLacunarity(2.0f);
    simdNoise->SetFractalGain(fractalGain);
    
    // Generate noise for the entire grid with SIMD acceleration
    int totalSamples = gridSizeX * gridSizeY * gridSizeZ;
    std::vector<float> noiseValues(totalSamples);
    simdNoise->FillNoiseSet(noiseValues.data(),
                            -gridOffsetX * NOISE_SAMPLE_RATE,
                            -gridOffsetY * NOISE_SAMPLE_RATE,
                            -gridOffsetZ * NOISE_SAMPLE_RATE,
                            gridSizeX, gridSizeY, gridSizeZ,
                            static_cast<float>(NOISE_SAMPLE_RATE));
    
    // Quantize to uint8
    for (int gz = 0; gz < gridSizeZ; ++gz) {
        for (int gy = 0; gy < gridSizeY; ++gy) {
            for (int gx = 0; gx < gridSizeX; ++gx) {
                int srcIdx = gx + gy * gridSizeX + gz * gridSizeX * gridSizeY;
                float noise = (noiseValues[srcIdx] + 1.0f) * 0.5f; // Normalize to [0, 1]
                noiseMap[srcIdx] = static_cast<uint8_t>(noise * 255.0f);
            }
        }
    }
    
    delete simdNoise;
    
    auto noiseMapEnd = std::chrono::high_resolution_clock::now();
    auto noiseMapDuration = std::chrono::duration_cast<std::chrono::milliseconds>(noiseMapEnd - noiseMapStart).count();
    std::cout << "   └─ Noise Map Generated: " << noiseMapDuration << "ms (" 
              << noiseMap.size() << " samples)" << std::endl;
    
    // Pre-computed lookup table for uint8 -> float conversion (1KB, eliminates division)
    static constexpr float UINT8_TO_FLOAT[256] = {
        0.0f, 0.003921569f, 0.007843138f, 0.011764706f, 0.015686275f, 0.019607844f, 0.023529412f, 0.02745098f,
        0.03137255f, 0.03529412f, 0.039215688f, 0.043137256f, 0.047058824f, 0.050980393f, 0.05490196f, 0.05882353f,
        0.0627451f, 0.06666667f, 0.07058824f, 0.07450981f, 0.078431375f, 0.08235294f, 0.08627451f, 0.09019608f,
        0.09411765f, 0.09803922f, 0.101960786f, 0.105882354f, 0.10980392f, 0.11372549f, 0.11764706f, 0.12156863f,
        0.1254902f, 0.12941177f, 0.13333334f, 0.13725491f, 0.14117648f, 0.14509805f, 0.14901961f, 0.15294118f,
        0.15686275f, 0.16078432f, 0.16470589f, 0.16862746f, 0.17254902f, 0.1764706f, 0.18039216f, 0.18431373f,
        0.1882353f, 0.19215687f, 0.19607843f, 0.2f, 0.20392157f, 0.20784314f, 0.21176471f, 0.21568628f,
        0.21960784f, 0.22352941f, 0.22745098f, 0.23137255f, 0.23529412f, 0.23921569f, 0.24313726f, 0.24705882f,
        0.2509804f, 0.25490198f, 0.25882354f, 0.2627451f, 0.26666668f, 0.27058825f, 0.27450982f, 0.2784314f,
        0.28235295f, 0.28627452f, 0.2901961f, 0.29411766f, 0.29803923f, 0.3019608f, 0.30588236f, 0.30980393f,
        0.3137255f, 0.31764707f, 0.32156864f, 0.3254902f, 0.32941177f, 0.33333334f, 0.3372549f, 0.34117648f,
        0.34509805f, 0.34901962f, 0.3529412f, 0.35686275f, 0.36078432f, 0.3647059f, 0.36862746f, 0.37254903f,
        0.3764706f, 0.38039216f, 0.38431373f, 0.3882353f, 0.39215687f, 0.39607844f, 0.4f, 0.40392157f,
        0.40784314f, 0.4117647f, 0.41568628f, 0.41960785f, 0.42352942f, 0.427451f, 0.43137255f, 0.43529412f,
        0.4392157f, 0.44313726f, 0.44705883f, 0.4509804f, 0.45490196f, 0.45882353f, 0.4627451f, 0.46666667f,
        0.47058824f, 0.4745098f, 0.47843137f, 0.48235294f, 0.4862745f, 0.49019608f, 0.49411765f, 0.49803922f,
        0.5019608f, 0.5058824f, 0.50980395f, 0.5137255f, 0.5176471f, 0.52156866f, 0.5254902f, 0.5294118f,
        0.53333336f, 0.5372549f, 0.5411765f, 0.54509807f, 0.54901963f, 0.5529412f, 0.5568628f, 0.56078434f,
        0.5647059f, 0.5686275f, 0.57254905f, 0.5764706f, 0.5803922f, 0.58431375f, 0.5882353f, 0.5921569f,
        0.59607846f, 0.6f, 0.6039216f, 0.60784316f, 0.6117647f, 0.6156863f, 0.61960787f, 0.62352943f,
        0.627451f, 0.6313726f, 0.63529414f, 0.6392157f, 0.6431373f, 0.64705884f, 0.6509804f, 0.654902f,
        0.65882355f, 0.6627451f, 0.6666667f, 0.67058825f, 0.6745098f, 0.6784314f, 0.68235296f, 0.6862745f,
        0.6901961f, 0.69411767f, 0.69803923f, 0.7019608f, 0.7058824f, 0.70980394f, 0.7137255f, 0.7176471f,
        0.72156864f, 0.7254902f, 0.7294118f, 0.73333335f, 0.7372549f, 0.7411765f, 0.74509805f, 0.7490196f,
        0.7529412f, 0.75686276f, 0.7607843f, 0.7647059f, 0.76862746f, 0.772549f, 0.7764706f, 0.78039217f,
        0.78431374f, 0.7882353f, 0.7921569f, 0.79607844f, 0.8f, 0.8039216f, 0.80784315f, 0.8117647f,
        0.8156863f, 0.81960785f, 0.8235294f, 0.827451f, 0.83137256f, 0.8352941f, 0.8392157f, 0.84313726f,
        0.8470588f, 0.8509804f, 0.85490197f, 0.85882354f, 0.8627451f, 0.8666667f, 0.87058824f, 0.8745098f,
        0.8784314f, 0.88235295f, 0.8862745f, 0.8901961f, 0.89411765f, 0.8980392f, 0.9019608f, 0.90588236f,
        0.9098039f, 0.9137255f, 0.91764706f, 0.92156863f, 0.9254902f, 0.9294118f, 0.93333334f, 0.9372549f,
        0.9411765f, 0.94509804f, 0.9490196f, 0.9529412f, 0.95686275f, 0.9607843f, 0.9647059f, 0.96862745f,
        0.972549f, 0.9764706f, 0.98039216f, 0.98431373f, 0.9882353f, 0.99215686f, 0.99607843f, 1.0f
    };
    
    // Lambda to sample from pre-generated noise map with trilinear interpolation
    auto sampleNoise = [&](float x, float y, float z) -> float {
        // Convert to grid space (account for sample rate)
        float gxf = (x / NOISE_SAMPLE_RATE) + gridOffsetX;
        float gyf = (y / NOISE_SAMPLE_RATE) + gridOffsetY;
        float gzf = (z / NOISE_SAMPLE_RATE) + gridOffsetZ;
        
        // Get integer coordinates and fractional parts for interpolation
        int gx0 = static_cast<int>(std::floor(gxf));
        int gy0 = static_cast<int>(std::floor(gyf));
        int gz0 = static_cast<int>(std::floor(gzf));
        int gx1 = gx0 + 1;
        int gy1 = gy0 + 1;
        int gz1 = gz0 + 1;
        
        // Clamp to grid bounds
        gx0 = std::max(0, std::min(gx0, gridSizeX - 1));
        gx1 = std::max(0, std::min(gx1, gridSizeX - 1));
        gy0 = std::max(0, std::min(gy0, gridSizeY - 1));
        gy1 = std::max(0, std::min(gy1, gridSizeY - 1));
        gz0 = std::max(0, std::min(gz0, gridSizeZ - 1));
        gz1 = std::max(0, std::min(gz1, gridSizeZ - 1));
        
        // Fractional parts
        float fx = gxf - std::floor(gxf);
        float fy = gyf - std::floor(gyf);
        float fz = gzf - std::floor(gzf);
        
        // Get 8 corner values
        int idx000 = gx0 + gy0 * gridSizeX + gz0 * gridSizeX * gridSizeY;
        int idx001 = gx0 + gy0 * gridSizeX + gz1 * gridSizeX * gridSizeY;
        int idx010 = gx0 + gy1 * gridSizeX + gz0 * gridSizeX * gridSizeY;
        int idx011 = gx0 + gy1 * gridSizeX + gz1 * gridSizeX * gridSizeY;
        int idx100 = gx1 + gy0 * gridSizeX + gz0 * gridSizeX * gridSizeY;
        int idx101 = gx1 + gy0 * gridSizeX + gz1 * gridSizeX * gridSizeY;
        int idx110 = gx1 + gy1 * gridSizeX + gz0 * gridSizeX * gridSizeY;
        int idx111 = gx1 + gy1 * gridSizeX + gz1 * gridSizeX * gridSizeY;
        
        float v000 = UINT8_TO_FLOAT[noiseMap[idx000]];
        float v001 = UINT8_TO_FLOAT[noiseMap[idx001]];
        float v010 = UINT8_TO_FLOAT[noiseMap[idx010]];
        float v011 = UINT8_TO_FLOAT[noiseMap[idx011]];
        float v100 = UINT8_TO_FLOAT[noiseMap[idx100]];
        float v101 = UINT8_TO_FLOAT[noiseMap[idx101]];
        float v110 = UINT8_TO_FLOAT[noiseMap[idx110]];
        float v111 = UINT8_TO_FLOAT[noiseMap[idx111]];
        
        // Trilinear interpolation
        float v00 = v000 * (1.0f - fx) + v100 * fx;
        float v01 = v001 * (1.0f - fx) + v101 * fx;
        float v10 = v010 * (1.0f - fx) + v110 * fx;
        float v11 = v011 * (1.0f - fx) + v111 * fx;
        
        float v0 = v00 * (1.0f - fy) + v10 * fy;
        float v1 = v01 * (1.0f - fy) + v11 * fy;
        
        return v0 * (1.0f - fz) + v1 * fz;
    };
    
    // **BFS CONNECTIVITY-AWARE GENERATION**
    // Only place voxels reachable from center - guarantees connectivity
    float radiusSquared = (radius * 1.4f) * (radius * 1.4f);
    float radiusDivisor = 1.0f / (radius * 1.2f);
    
    long long voxelsGenerated = 0;
    long long voxelsSampled = 0;
    
    // Chunk cache: avoid hash map lookups when placing consecutive voxels in same chunk
    VoxelChunk* cachedChunk = nullptr;
    Vec3 cachedChunkCoord(-999999, -999999, -999999);
    
    // Helper lambda: Direct voxel placement without mutex (worldgen is single-threaded)
    auto setVoxelDirect = [&](const Vec3& pos, uint8_t blockID) {
        int chunkX = static_cast<int>(std::floor(pos.x / VoxelChunk::SIZE));
        int chunkY = static_cast<int>(std::floor(pos.y / VoxelChunk::SIZE));
        int chunkZ = static_cast<int>(std::floor(pos.z / VoxelChunk::SIZE));
        
        int localX = static_cast<int>(std::floor(pos.x)) - (chunkX * VoxelChunk::SIZE);
        int localY = static_cast<int>(std::floor(pos.y)) - (chunkY * VoxelChunk::SIZE);
        int localZ = static_cast<int>(std::floor(pos.z)) - (chunkZ * VoxelChunk::SIZE);
        
        // Check chunk cache to avoid hash map lookup
        if (cachedChunk == nullptr || cachedChunkCoord.x != chunkX || 
            cachedChunkCoord.y != chunkY || cachedChunkCoord.z != chunkZ) {
            Vec3 chunkCoord(chunkX, chunkY, chunkZ);
            std::unique_ptr<VoxelChunk>& chunkPtr = chunkMap[chunkCoord];
            if (!chunkPtr) {
                chunkPtr = std::make_unique<VoxelChunk>();
                chunkPtr->setIslandContext(islandID, chunkCoord);
                chunkPtr->setIsClient(m_isClient);
            }
            cachedChunk = chunkPtr.get();
            cachedChunkCoord = chunkCoord;
        }
        
        cachedChunk->setVoxelDataDirect(localX, localY, localZ, blockID);
    };
    
    // BFS from center
    std::queue<Vec3> frontier;
    
    // OPTIMIZATION: Replace unordered_set with sparse 3D grid for visited tracking
    // This eliminates millions of hash computations and lookups
    // Use chunk-based storage: 16x16x16 sub-grids indexed by chunk coordinate
    constexpr int VISIT_CHUNK_SIZE = 16;
    std::unordered_map<int64_t, std::vector<bool>> visitedChunks;
    
    auto encodeChunkPos = [](int cx, int cy, int cz) -> int64_t {
        return (static_cast<int64_t>(cx + 8192) << 32) |
               (static_cast<int64_t>(cy + 8192) << 16) |
               static_cast<int64_t>(cz + 8192);
    };
    
    auto isVisited = [&](const Vec3& p) -> bool {
        int cx = static_cast<int>(std::floor(p.x / VISIT_CHUNK_SIZE));
        int cy = static_cast<int>(std::floor(p.y / VISIT_CHUNK_SIZE));
        int cz = static_cast<int>(std::floor(p.z / VISIT_CHUNK_SIZE));
        int64_t chunkKey = encodeChunkPos(cx, cy, cz);
        
        auto it = visitedChunks.find(chunkKey);
        if (it == visitedChunks.end()) return false;
        
        int lx = static_cast<int>(p.x) - (cx * VISIT_CHUNK_SIZE);
        int ly = static_cast<int>(p.y) - (cy * VISIT_CHUNK_SIZE);
        int lz = static_cast<int>(p.z) - (cz * VISIT_CHUNK_SIZE);
        
        // Handle negative coordinates
        if (lx < 0) lx += VISIT_CHUNK_SIZE;
        if (ly < 0) ly += VISIT_CHUNK_SIZE;
        if (lz < 0) lz += VISIT_CHUNK_SIZE;
        
        int idx = lx + ly * VISIT_CHUNK_SIZE + lz * VISIT_CHUNK_SIZE * VISIT_CHUNK_SIZE;
        return it->second[idx];
    };
    
    auto markVisited = [&](const Vec3& p) {
        int cx = static_cast<int>(std::floor(p.x / VISIT_CHUNK_SIZE));
        int cy = static_cast<int>(std::floor(p.y / VISIT_CHUNK_SIZE));
        int cz = static_cast<int>(std::floor(p.z / VISIT_CHUNK_SIZE));
        int64_t chunkKey = encodeChunkPos(cx, cy, cz);
        
        auto& visitChunk = visitedChunks[chunkKey];
        if (visitChunk.empty()) {
            visitChunk.resize(VISIT_CHUNK_SIZE * VISIT_CHUNK_SIZE * VISIT_CHUNK_SIZE, false);
        }
        
        int lx = static_cast<int>(p.x) - (cx * VISIT_CHUNK_SIZE);
        int ly = static_cast<int>(p.y) - (cy * VISIT_CHUNK_SIZE);
        int lz = static_cast<int>(p.z) - (cz * VISIT_CHUNK_SIZE);
        
        // Handle negative coordinates
        if (lx < 0) lx += VISIT_CHUNK_SIZE;
        if (ly < 0) ly += VISIT_CHUNK_SIZE;
        if (lz < 0) lz += VISIT_CHUNK_SIZE;
        
        int idx = lx + ly * VISIT_CHUNK_SIZE + lz * VISIT_CHUNK_SIZE * VISIT_CHUNK_SIZE;
        visitChunk[idx] = true;
    };
    
    Vec3 startPos(0, 0, 0);
    frontier.push(startPos);
    markVisited(startPos);
    setVoxelDirect(startPos, palette.deepBlock);
    voxelsGenerated++;
    
    while (!frontier.empty())
    {
        Vec3 current = frontier.front();
        frontier.pop();
        
        // Check all 6 neighbors
        static const Vec3 neighbors[6] = {
            Vec3(1, 0, 0), Vec3(-1, 0, 0),
            Vec3(0, 1, 0), Vec3(0, -1, 0),
            Vec3(0, 0, 1), Vec3(0, 0, -1)
        };
        
        for (const Vec3& delta : neighbors)
        {
            Vec3 neighbor = current + delta;
            float dx = neighbor.x;
            float dy = neighbor.y;
            float dz = neighbor.z;
            
            // EARLY EXIT 1: Vertical bounds
            if (dy < -islandHeight || dy > islandHeight) continue;
            
            // EARLY EXIT 2: Radial bounds (eliminates corners/outer regions)
            float distanceSquared = dx * dx + dy * dy + dz * dz;
            if (distanceSquared > radiusSquared) continue;
            
            // EARLY EXIT 3: Only NOW check visited (avoid hash ops for out-of-bounds positions)
            if (isVisited(neighbor)) continue;
            markVisited(neighbor);
            voxelsSampled++;
            
            // Vertical density
            float islandHeightRange = islandHeight * 2.0f;
            float normalizedY = (dy + islandHeight) / islandHeightRange;
            float centerOffset = normalizedY - 0.5f;
            float verticalDensity = 1.0f - (centerOffset * centerOffset * 4.0f);
            verticalDensity = std::max(0.0f, verticalDensity);
            
            if (verticalDensity < 0.01f) continue;
            
            // Radial falloff
            float distanceFromCenter = std::sqrt(distanceSquared);
            float islandBase = 1.0f - (distanceFromCenter * radiusDivisor);
            islandBase = std::max(0.0f, islandBase);
            islandBase = islandBase * islandBase;
            
            if (islandBase < 0.01f) continue;
            
            // Sample from pre-generated noise map (instant lookup vs expensive calculation)
            float noise = sampleNoise(dx, dy, dz);
            
            float finalDensity = islandBase * verticalDensity * noise;
            
            if (finalDensity > densityThreshold)
            {
                setVoxelDirect(neighbor, palette.deepBlock);
                voxelsGenerated++;
                frontier.push(neighbor);  // Expand BFS frontier
            }
        }
    }
    
    auto voxelGenEnd = std::chrono::high_resolution_clock::now();
    auto voxelGenDuration = std::chrono::duration_cast<std::chrono::milliseconds>(voxelGenEnd - voxelGenStart).count();
    
    std::cout << "🔨 Voxel Generation (BFS): " << voxelGenDuration << "ms (" << voxelsGenerated << " voxels, " 
              << island->chunks.size() << " chunks)" << std::endl;
    std::cout << "   └─ Positions Sampled: " << voxelsSampled << " (connectivity-aware)" << std::endl;
    
    // **SURFACE DETECTION PASS**
    // Now that all voxels are placed, determine which should be surface/subsurface blocks
    // Also collect surface voxel positions for connectivity analysis
    auto surfacePassStart = std::chrono::high_resolution_clock::now();
    int surfaceBlocksPlaced = 0;
    int subsurfaceBlocksPlaced = 0;
    long long voxelsChecked = 0;
    long long neighborChecks = 0;
    
    std::vector<Vec3> surfaceVoxelPositions;  // Collect surface blocks with horizontal air exposure
    
    static const Vec3 checkNeighbors[6] = {
        Vec3(1, 0, 0), Vec3(-1, 0, 0),
        Vec3(0, 1, 0), Vec3(0, -1, 0),
        Vec3(0, 0, 1), Vec3(0, 0, -1)
    };
    
    static const Vec3 horizontalNeighbors[4] = {
        Vec3(1, 0, 0), Vec3(-1, 0, 0),
        Vec3(0, 0, 1), Vec3(0, 0, -1)
    };
    
    auto iterationStart = std::chrono::high_resolution_clock::now();
    
    // Iterate through all chunks and check each solid block
    for (auto& [chunkCoord, chunk] : island->chunks)
    {
        if (!chunk) continue;
        
        for (int lz = 0; lz < VoxelChunk::SIZE; ++lz)
        {
            for (int ly = 0; ly < VoxelChunk::SIZE; ++ly)
            {
                for (int lx = 0; lx < VoxelChunk::SIZE; ++lx)
                {
                    uint8_t currentBlock = chunk->getVoxel(lx, ly, lz);
                    if (currentBlock == BlockID::AIR) continue;
                    if (currentBlock != palette.deepBlock) continue;  // Only process deep blocks
                    
                    voxelsChecked++;
                    
                    // Calculate world position
                    Vec3 worldPos = chunkCoord * VoxelChunk::SIZE + Vec3(lx, ly, lz);
                    
                    // Single-pass neighbor scan: check for both air and surface blocks
                    bool hasAirNeighbor = false;
                    bool hasSurfaceNeighbor = false;
                    
                    for (const Vec3& delta : checkNeighbors)
                    {
                        Vec3 neighborPos = worldPos + delta;
                        neighborChecks++;
                        uint8_t neighborBlock = getVoxelDirect(neighborPos);
                        
                        if (neighborBlock == BlockID::AIR)
                            hasAirNeighbor = true;
                        else if (neighborBlock == palette.surfaceBlock)
                            hasSurfaceNeighbor = true;
                        
                        // Early exit if we found both - no need to check remaining neighbors
                        if (hasAirNeighbor && hasSurfaceNeighbor)
                            break;
                    }
                    
                    if (hasAirNeighbor)
                    {
                        // This is a surface block - replace with surface block type
                        chunk->setVoxel(lx, ly, lz, palette.surfaceBlock);
                        surfaceBlocksPlaced++;
                        
                        // Check if this surface block has horizontal air exposure (walls)
                        // We collect these for satellite detection via surface shell connectivity
                        bool hasHorizontalAir = false;
                        for (const Vec3& delta : horizontalNeighbors)
                        {
                            Vec3 neighborPos = worldPos + delta;
                            uint8_t neighborBlock = getVoxelDirect(neighborPos);
                            if (neighborBlock == BlockID::AIR)
                            {
                                hasHorizontalAir = true;
                                break;
                            }
                        }
                        
                        // Include blocks with ANY air exposure for satellite detection
                        // (top/bottom exposed blocks connect surface shells vertically)
                        surfaceVoxelPositions.push_back(worldPos);
                    }
                    else if (hasSurfaceNeighbor)
                    {
                        // This is a subsurface block (1 layer below surface)
                        chunk->setVoxel(lx, ly, lz, palette.subsurfaceBlock);
                        subsurfaceBlocksPlaced++;
                    }
                    // else: remains deep block
                }
            }
        }
    }
    
    auto iterationEnd = std::chrono::high_resolution_clock::now();
    auto iterationDuration = std::chrono::duration_cast<std::chrono::milliseconds>(iterationEnd - iterationStart).count();
    
    auto surfacePassEnd = std::chrono::high_resolution_clock::now();
    auto surfacePassDuration = std::chrono::duration_cast<std::chrono::milliseconds>(surfacePassEnd - surfacePassStart).count();
    
    std::cout << "🎨 Surface Detection: " << surfacePassDuration << "ms (" 
              << surfaceBlocksPlaced << " surface, " 
              << subsurfaceBlocksPlaced << " subsurface)" << std::endl;
    std::cout << "   └─ Voxels Checked: " << voxelsChecked << " (" << neighborChecks << " neighbor lookups)" << std::endl;
    std::cout << "   └─ Iteration Time: " << iterationDuration << "ms" << std::endl;
    
    // **WATER BASIN PASS**
    auto waterStart = std::chrono::high_resolution_clock::now();
    
    // Step 1: Fill ground-level basins
    auto basinFillStart = std::chrono::high_resolution_clock::now();
    std::unordered_set<int64_t> waterPositions = placeWaterBasins(islandID, palette, seed);
    auto basinFillEnd = std::chrono::high_resolution_clock::now();
    auto basinFillDuration = std::chrono::duration_cast<std::chrono::milliseconds>(basinFillEnd - basinFillStart).count();
    
    // Step 2: Cull exposed water - ONLY check newly placed water
    auto initialCullStart = std::chrono::high_resolution_clock::now();
    cullExposedWater(islandID, &waterPositions);
    auto initialCullEnd = std::chrono::high_resolution_clock::now();
    auto initialCullDuration = std::chrono::duration_cast<std::chrono::milliseconds>(initialCullEnd - initialCullStart).count();
    
    // Update waterPositions after culling - remove positions that no longer have water
    auto decodePos = [](int64_t key) -> Vec3 {
        int x = static_cast<int>((key >> 32) & 0xFFFF) - 32768;
        int y = static_cast<int>((key >> 16) & 0xFFFF) - 32768;
        int z = static_cast<int>(key & 0xFFFF) - 32768;
        return Vec3(x, y, z);
    };
    
    std::unordered_set<int64_t> survivingWater;
    for (int64_t key : waterPositions) {
        Vec3 pos = decodePos(key);
        uint8_t block = getVoxelDirect(pos);
        if (block == BlockID::WATER || block == BlockID::ICE || block == BlockID::LAVA) {
            survivingWater.insert(key);
        }
    }
    waterPositions = std::move(survivingWater);
    
    // Step 3: Iteratively add layers upward with flood-fill + cull
    auto layerExpansionStart = std::chrono::high_resolution_clock::now();
    int layersAdded = 0;
    int maxLayers = std::max(palette.minWaterDepth, palette.maxWaterDepth);
    
    for (int layer = 0; layer < maxLayers; ++layer)
    {
        auto layerStart = std::chrono::high_resolution_clock::now();
        
        // Find all water surface positions (water with air above) - OPTIMIZED: only scan tracked water positions
        auto findSurfacesStart = std::chrono::high_resolution_clock::now();
        std::vector<Vec3> waterSurfaces;
        
        for (int64_t key : waterPositions)
        {
            Vec3 waterPos = decodePos(key);
            Vec3 abovePos = waterPos + Vec3(0, 1, 0);
            
            // Check if air above
            uint8_t blockAbove = getVoxelDirect(abovePos);
            if (blockAbove == BlockID::AIR)
            {
                waterSurfaces.push_back(waterPos);
            }
        }
        
        auto findSurfacesEnd = std::chrono::high_resolution_clock::now();
        auto findSurfacesDuration = std::chrono::duration_cast<std::chrono::milliseconds>(findSurfacesEnd - findSurfacesStart).count();
        
        if (waterSurfaces.empty()) break; // No water to expand from
        
        // Horizontal flood-fill from each water surface position
        auto floodFillStart = std::chrono::high_resolution_clock::now();
        std::unordered_set<int64_t> newWaterSet;
        auto encodePos = [](const Vec3& p) -> int64_t {
            return (static_cast<int64_t>(p.x + 32768) << 32) | 
                   (static_cast<int64_t>(p.y + 32768) << 16) | 
                   static_cast<int64_t>(p.z + 32768);
        };
        
        static const Vec3 horizNeighbors[4] = {
            Vec3(1, 0, 0), Vec3(-1, 0, 0),
            Vec3(0, 0, 1), Vec3(0, 0, -1)
        };
        
        for (const Vec3& surfacePos : waterSurfaces)
        {
            Vec3 startPos = surfacePos + Vec3(0, 1, 0);
            int64_t startKey = encodePos(startPos);
            if (newWaterSet.count(startKey)) continue;
            
            // Flood fill horizontally at this Y level
            std::queue<Vec3> fillQueue;
            fillQueue.push(startPos);
            newWaterSet.insert(startKey);
            
            while (!fillQueue.empty())
            {
                Vec3 current = fillQueue.front();
                fillQueue.pop();
                
                uint8_t currentBlock = getVoxelDirect(current);
                if (currentBlock != BlockID::AIR) continue;
                
                // Check if solid below
                uint8_t blockBelow = getVoxelDirect(current + Vec3(0, -1, 0));
                if (blockBelow == BlockID::AIR) continue;
                
                // Expand horizontally
                for (const Vec3& delta : horizNeighbors)
                {
                    Vec3 neighbor = current + delta;
                    int64_t neighborKey = encodePos(neighbor);
                    if (newWaterSet.count(neighborKey)) continue;
                    newWaterSet.insert(neighborKey);
                    fillQueue.push(neighbor);
                }
            }
        }
        
        auto floodFillEnd = std::chrono::high_resolution_clock::now();
        auto floodFillDuration = std::chrono::duration_cast<std::chrono::milliseconds>(floodFillEnd - floodFillStart).count();
        
        if (newWaterSet.empty()) break;
        
        // Place new water layer
        auto placeWaterStart = std::chrono::high_resolution_clock::now();
        for (int64_t key : newWaterSet)
        {
            int x = static_cast<int>((key >> 32) & 0xFFFF) - 32768;
            int y = static_cast<int>((key >> 16) & 0xFFFF) - 32768;
            int z = static_cast<int>(key & 0xFFFF) - 32768;
            setBlockIDWithAutoChunk(islandID, Vec3(x, y, z), palette.waterBlock);
        }
        auto placeWaterEnd = std::chrono::high_resolution_clock::now();
        auto placeWaterDuration = std::chrono::duration_cast<std::chrono::milliseconds>(placeWaterEnd - placeWaterStart).count();
        
        // Cull exposed water at this layer - ONLY check newly placed water
        auto cullStart = std::chrono::high_resolution_clock::now();
        cullExposedWater(islandID, &newWaterSet);
        auto cullEnd = std::chrono::high_resolution_clock::now();
        auto cullDuration = std::chrono::duration_cast<std::chrono::milliseconds>(cullEnd - cullStart).count();
        
        // Check if any water survived culling - if not, we're done
        // Also update waterPositions to track only surviving water for next layer
        std::unordered_set<int64_t> updatedWaterPositions;
        bool anyWaterRemains = false;
        for (int64_t key : newWaterSet)
        {
            Vec3 pos = decodePos(key);
            uint8_t block = getVoxelDirect(pos);
            if (block == BlockID::WATER || block == BlockID::ICE || block == BlockID::LAVA)
            {
                anyWaterRemains = true;
                updatedWaterPositions.insert(key);
            }
        }
        
        if (!anyWaterRemains) break; // Layer was completely culled, stop
        
        // Update tracked water positions: combine with previous layer's water
        waterPositions.insert(updatedWaterPositions.begin(), updatedWaterPositions.end());
        
        auto layerEnd = std::chrono::high_resolution_clock::now();
        auto layerDuration = std::chrono::duration_cast<std::chrono::milliseconds>(layerEnd - layerStart).count();
        std::cout << "   └─ Layer " << layer << ": " << layerDuration << "ms "
                  << "(find=" << findSurfacesDuration << "ms, "
                  << "flood=" << floodFillDuration << "ms, "
                  << "place=" << placeWaterDuration << "ms, "
                  << "cull=" << cullDuration << "ms)" << std::endl;
        
        layersAdded++;
    }
    
    auto layerExpansionEnd = std::chrono::high_resolution_clock::now();
    auto layerExpansionDuration = std::chrono::duration_cast<std::chrono::milliseconds>(layerExpansionEnd - layerExpansionStart).count();
    
    auto waterEnd = std::chrono::high_resolution_clock::now();
    auto waterDuration = std::chrono::duration_cast<std::chrono::milliseconds>(waterEnd - waterStart).count();
    std::cout << "💧 Water Basins: " << waterDuration << "ms (" << layersAdded << " layers added)" << std::endl;
    std::cout << "   └─ Layer Expansion: " << layerExpansionDuration << "ms" << std::endl;
    
    // **VEGETATION DECORATION PASS**
    // Place grass GLB models and voxel trees based on biome vegetation density
    auto decorationStart = std::chrono::high_resolution_clock::now();
    
    int grassPlaced = 0;
    int treesPlaced = 0;
    
    // Use biome-specific vegetation density - SPARSE trees for Terralith-like feel
    float grassChance = palette.vegetationDensity * 80.0f;  // Lots of grass models (0-80%)
    float treeChance = palette.vegetationDensity * 1.5f;    // Much sparser trees (0-1.5%)
    
    // Direct chunk iteration - we already have the island pointer, no locks needed
    for (auto& [chunkCoord, chunk] : island->chunks) {
        if (!chunk) continue;
        
        // Scan each voxel in this chunk
        for (int z = 0; z < VoxelChunk::SIZE; ++z) {
            for (int x = 0; x < VoxelChunk::SIZE; ++x) {
                for (int y = VoxelChunk::SIZE - 1; y >= 0; --y) {  // Scan top-down
                    uint8_t blockID = chunk->getVoxel(x, y, z);
                    if (blockID == BlockID::AIR) continue;
                    
                    // Found solid block - check if surface is exposed (only place on surface blocks)
                    if (blockID != palette.surfaceBlock) {
                        break;  // Not a surface block, skip this column
                    }
                    
                    if (y + 1 < VoxelChunk::SIZE) {
                        uint8_t blockAbove = chunk->getVoxel(x, y + 1, z);
                        if (blockAbove == BlockID::AIR) {
                            float roll = (std::rand() % 100);
                            
                            // Calculate world position for tree placement
                            Vec3 worldPos = chunkCoord * VoxelChunk::SIZE + Vec3(x, y + 1, z);
                            
                            // Place voxel tree (rarer)
                            if (roll < treeChance) {
                                // Use world position as seed for variety
                                uint32_t treeSeed = static_cast<uint32_t>(
                                    static_cast<int>(worldPos.x) * 73856093 ^ 
                                    static_cast<int>(worldPos.y) * 19349663 ^ 
                                    static_cast<int>(worldPos.z) * 83492791
                                );
                                TreeGenerator::generateTree(this, islandID, worldPos, treeSeed, palette.vegetationDensity);
                                treesPlaced++;
                            }
                            // Place grass GLB (more common)
                            else if (roll < grassChance) {
                                chunk->setVoxel(x, y + 1, z, BlockID::DECOR_GRASS);
                                grassPlaced++;
                            }
                        }
                    } else {
                        // Edge of chunk - check neighboring chunk above (rare case)
                        Vec3 aboveChunkCoord = chunkCoord + Vec3(0, 1, 0);
                        auto itAbove = island->chunks.find(aboveChunkCoord);
                        if (itAbove != island->chunks.end() && itAbove->second) {
                            uint8_t blockAbove = itAbove->second->getVoxel(x, 0, z);
                            if (blockAbove == BlockID::AIR) {
                                float roll = (std::rand() % 100);
                                Vec3 worldPos = chunkCoord * VoxelChunk::SIZE + Vec3(x, y + 1, z);
                                
                                if (roll < treeChance) {
                                    uint32_t treeSeed = static_cast<uint32_t>(
                                        static_cast<int>(worldPos.x) * 73856093 ^ 
                                        static_cast<int>(worldPos.y) * 19349663 ^ 
                                        static_cast<int>(worldPos.z) * 83492791
                                    );
                                    TreeGenerator::generateTree(this, islandID, worldPos, treeSeed, palette.vegetationDensity);
                                    treesPlaced++;
                                }
                                else if (roll < grassChance) {
                                    itAbove->second->setVoxel(x, 0, z, BlockID::DECOR_GRASS);
                                    grassPlaced++;
                                }
                            }
                        }
                    }
                    
                    break;  // Found topmost solid block in this column, move to next
                }
            }
        }
    }
    
    auto decorationEnd = std::chrono::high_resolution_clock::now();
    auto decorationDuration = std::chrono::duration_cast<std::chrono::milliseconds>(decorationEnd - decorationStart).count();
    std::cout << "🌿 Vegetation: " << decorationDuration << "ms (" 
              << grassPlaced << " grass, " << treesPlaced << " trees)" << std::endl;
    
    // Chunks will be registered with renderer when client receives them via network
    // (Don't queue mesh generation here - will be done when chunk is registered with renderer)
    std::cout << "   └─ Island created - mesh generation will happen when chunks are registered with renderer" << std::endl;
    
    auto totalEnd = std::chrono::high_resolution_clock::now();
    auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - startTime).count();
    
    std::cout << "✅ Island Generation Complete: " << totalDuration << "ms total" << std::endl;
    std::cout << "   └─ Breakdown: Voxels=" << voxelGenDuration << "ms (" 
              << (voxelGenDuration * 100 / std::max(1LL, totalDuration)) << "%)" << std::endl;
}

uint8_t IslandChunkSystem::getVoxelFromIsland(uint32_t islandID, const Vec3& islandRelativePosition) const
{
    // Hold lock across the entire access to prevent races
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto itIsl = m_islands.find(islandID);
    if (itIsl == m_islands.end())
        return 0;

    const FloatingIsland& island = itIsl->second;

    // Convert island-relative position to chunk coordinate and local voxel position
    Vec3 chunkCoord = FloatingIsland::islandPosToChunkCoord(islandRelativePosition);
    Vec3 localPos = FloatingIsland::islandPosToLocalPos(islandRelativePosition);

    // Find the chunk
    auto it = island.chunks.find(chunkCoord);
    if (it == island.chunks.end())
        return 0; // Chunk doesn't exist

    const VoxelChunk* chunk = it->second.get();
    if (!chunk)
        return 0;

    // Get voxel from chunk using local coordinates
    int x = static_cast<int>(localPos.x);
    int y = static_cast<int>(localPos.y);
    int z = static_cast<int>(localPos.z);

    // Check bounds (should be 0-15 for 16x16x16 chunks)
    if (x < 0 || x >= VoxelChunk::SIZE || y < 0 || y >= VoxelChunk::SIZE || z < 0 || z >= VoxelChunk::SIZE)
        return 0;

    return chunk->getVoxel(x, y, z);
}

void IslandChunkSystem::setVoxelWithMesh(uint32_t islandID, const Vec3& islandRelativePosition, uint8_t voxelType)
{
    // Acquire chunk pointer under lock, then perform heavy work without holding the map mutex
    VoxelChunk* chunk = nullptr;
    Vec3 localPos;
    Vec3 chunkCoord;
    Vec3 islandCenter;
    bool isNewChunk = false;
    {
        std::lock_guard<std::mutex> lock(m_islandsMutex);
        auto itIsl = m_islands.find(islandID);
        if (itIsl == m_islands.end())
            return;
        FloatingIsland& island = itIsl->second;
        chunkCoord = FloatingIsland::islandPosToChunkCoord(islandRelativePosition);
        localPos = FloatingIsland::islandPosToLocalPos(islandRelativePosition);
        islandCenter = island.physicsCenter;
        std::unique_ptr<VoxelChunk>& chunkPtr = island.chunks[chunkCoord];
        if (!chunkPtr)
        {
            chunkPtr = std::make_unique<VoxelChunk>();
            chunkPtr->setIslandContext(islandID, chunkCoord);
            chunkPtr->setIsClient(m_isClient);  // Inherit client flag from island system
            isNewChunk = true;
        }
        chunk = chunkPtr.get();
    }

    // Set voxel outside of islands mutex to avoid deadlocks
    int x = static_cast<int>(localPos.x);
    int y = static_cast<int>(localPos.y);
    int z = static_cast<int>(localPos.z);
    if (x < 0 || x >= VoxelChunk::SIZE || y < 0 || y >= VoxelChunk::SIZE || z < 0 || z >= VoxelChunk::SIZE)
        return;
    
    chunk->setVoxel(x, y, z, voxelType);
    // Note: mesh regeneration is handled by the caller (GameClient or GameServer)
    // to allow batch updates and neighbor chunk updates
}

void IslandChunkSystem::setVoxelServerOnly(uint32_t islandID, const Vec3& islandRelativePosition, uint8_t voxelType)
{
    // SERVER-ONLY: Modify voxel data WITHOUT triggering any mesh operations
    // This method directly modifies the voxel array and never calls chunk->setVoxel()
    VoxelChunk* chunk = nullptr;
    Vec3 localPos;
    Vec3 chunkCoord;
    {
        std::lock_guard<std::mutex> lock(m_islandsMutex);
        auto itIsl = m_islands.find(islandID);
        if (itIsl == m_islands.end())
            return;
        FloatingIsland& island = itIsl->second;
        chunkCoord = FloatingIsland::islandPosToChunkCoord(islandRelativePosition);
        localPos = FloatingIsland::islandPosToLocalPos(islandRelativePosition);
        std::unique_ptr<VoxelChunk>& chunkPtr = island.chunks[chunkCoord];
        if (!chunkPtr)
        {
            chunkPtr = std::make_unique<VoxelChunk>();
            chunkPtr->setIslandContext(islandID, chunkCoord);
            chunkPtr->setIsClient(false);  // Server chunks are never client chunks
        }
        chunk = chunkPtr.get();
    }

    // Directly modify voxel data without calling chunk->setVoxel()
    int x = static_cast<int>(localPos.x);
    int y = static_cast<int>(localPos.y);
    int z = static_cast<int>(localPos.z);
    if (x < 0 || x >= VoxelChunk::SIZE || y < 0 || y >= VoxelChunk::SIZE || z < 0 || z >= VoxelChunk::SIZE)
        return;
    
    // Use the direct data modification method (no mesh operations)
    chunk->setVoxelDataDirect(x, y, z, voxelType);
}

void IslandChunkSystem::setVoxelWithAutoChunk(uint32_t islandID, const Vec3& islandRelativePos, uint8_t voxelType)
{
    // Acquire chunk pointer under lock, then write outside the lock
    VoxelChunk* chunk = nullptr;
    int localX = 0, localY = 0, localZ = 0;
    {
        std::lock_guard<std::mutex> lock(m_islandsMutex);
        auto itIsl = m_islands.find(islandID);
        if (itIsl == m_islands.end())
            return;
        FloatingIsland& island = itIsl->second;

        int chunkX = static_cast<int>(std::floor(islandRelativePos.x / VoxelChunk::SIZE));
        int chunkY = static_cast<int>(std::floor(islandRelativePos.y / VoxelChunk::SIZE));
        int chunkZ = static_cast<int>(std::floor(islandRelativePos.z / VoxelChunk::SIZE));
        Vec3 chunkCoord(chunkX, chunkY, chunkZ);

        // Calculate local coordinates correctly for negative positions
        // Use the same floor-based calculation to keep coordinates consistent
        localX = static_cast<int>(std::floor(islandRelativePos.x)) - (chunkX * VoxelChunk::SIZE);
        localY = static_cast<int>(std::floor(islandRelativePos.y)) - (chunkY * VoxelChunk::SIZE);
        localZ = static_cast<int>(std::floor(islandRelativePos.z)) - (chunkZ * VoxelChunk::SIZE);

        std::unique_ptr<VoxelChunk>& chunkPtr = island.chunks[chunkCoord];
        if (!chunkPtr) {
            chunkPtr = std::make_unique<VoxelChunk>();
            chunkPtr->setIslandContext(islandID, chunkCoord);
            chunkPtr->setIsClient(m_isClient);  // Inherit client flag from island system
        }
        chunk = chunkPtr.get();
    }

    if (!chunk) return;
    chunk->setVoxel(localX, localY, localZ, voxelType);
    // Mesh generation can be deferred/batched; leave as-is to avoid stutters
}

// ID-based block methods (clean and efficient)
void IslandChunkSystem::setBlockIDWithAutoChunk(uint32_t islandID, const Vec3& islandRelativePos, uint8_t blockID)
{
    setVoxelWithAutoChunk(islandID, islandRelativePos, blockID);
}

void IslandChunkSystem::getAllChunks(std::vector<VoxelChunk*>& outChunks)
{
    outChunks.clear();
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    for (auto& [id, island] : m_islands)
    {
        // Add all chunks from this island
        for (auto& [chunkCoord, chunk] : island.chunks)
        {
            if (chunk)
            {
                outChunks.push_back(chunk.get());
            }
        }
    }
}

void IslandChunkSystem::getVisibleChunks(const Vec3& viewPosition,
                                         std::vector<VoxelChunk*>& outChunks)
{
    // Legacy distance-based culling fallback
    getAllChunks(outChunks);
}

void IslandChunkSystem::getVisibleChunksFrustum(const Frustum& frustum, std::vector<VoxelChunk*>& outChunks)
{
    PROFILE_SCOPE("FrustumCulling");
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    
    for (auto& [islandID, island] : m_islands)
    {
        glm::mat4 islandTransform = island.getTransformMatrix();
        
        for (auto& [chunkCoord, chunk] : island.chunks)
        {
            if (!chunk) continue;
            
            const auto& cachedAABB = chunk->getCachedWorldAABB();
            Vec3 worldMin, worldMax;
            
            if (cachedAABB.valid)
            {
                worldMin = cachedAABB.min;
                worldMax = cachedAABB.max;
            }
            else
            {
                Vec3 localMin = chunkCoord * static_cast<float>(VoxelChunk::SIZE);
                Vec3 localMax = localMin + Vec3(VoxelChunk::SIZE, VoxelChunk::SIZE, VoxelChunk::SIZE);
                
                glm::vec4 corners[8] = {
                    islandTransform * glm::vec4(localMin.x, localMin.y, localMin.z, 1.0f),
                    islandTransform * glm::vec4(localMax.x, localMin.y, localMin.z, 1.0f),
                    islandTransform * glm::vec4(localMin.x, localMax.y, localMin.z, 1.0f),
                    islandTransform * glm::vec4(localMax.x, localMax.y, localMin.z, 1.0f),
                    islandTransform * glm::vec4(localMin.x, localMin.y, localMax.z, 1.0f),
                    islandTransform * glm::vec4(localMax.x, localMin.y, localMax.z, 1.0f),
                    islandTransform * glm::vec4(localMin.x, localMax.y, localMax.z, 1.0f),
                    islandTransform * glm::vec4(localMax.x, localMax.y, localMax.z, 1.0f)
                };
                
                worldMin = Vec3(corners[0].x, corners[0].y, corners[0].z);
                worldMax = worldMin;
                
                for (int i = 1; i < 8; ++i)
                {
                    worldMin.x = std::min(worldMin.x, corners[i].x);
                    worldMin.y = std::min(worldMin.y, corners[i].y);
                    worldMin.z = std::min(worldMin.z, corners[i].z);
                    worldMax.x = std::max(worldMax.x, corners[i].x);
                    worldMax.y = std::max(worldMax.y, corners[i].y);
                    worldMax.z = std::max(worldMax.z, corners[i].z);
                }
                
                chunk->setCachedWorldAABB(worldMin, worldMax);
            }
            
            // Frustum culling disabled - always render all chunks
            //if (frustum.intersectsAABB(worldMin, worldMax))
            //{
                outChunks.push_back(chunk.get());
            //}
        }
    }
}

void IslandChunkSystem::updateIslandPhysics(float deltaTime)
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    for (auto& [id, island] : m_islands)
    {
        // Track if island actually moved/rotated this frame
        bool moved = false;
        
        // OPTIMIZATION: Only update if island is actually moving
        float velocityMagnitudeSq = island.velocity.x * island.velocity.x +
                                    island.velocity.y * island.velocity.y +
                                    island.velocity.z * island.velocity.z;
        
        if (velocityMagnitudeSq > 0.0001f) // Threshold: ~0.01 units/sec
        {
            island.physicsCenter.x += island.velocity.x * deltaTime;
            island.physicsCenter.y += island.velocity.y * deltaTime;
            island.physicsCenter.z += island.velocity.z * deltaTime;
            moved = true;
        }
        
        // OPTIMIZATION: Only update rotation if island is actually rotating
        float angularMagnitudeSq = island.angularVelocity.x * island.angularVelocity.x +
                                   island.angularVelocity.y * island.angularVelocity.y +
                                   island.angularVelocity.z * island.angularVelocity.z;
        
        if (angularMagnitudeSq > 0.0001f) // Threshold: ~0.01 rad/sec
        {
            island.rotation.x += island.angularVelocity.x * deltaTime;
            island.rotation.y += island.angularVelocity.y * deltaTime;
            island.rotation.z += island.angularVelocity.z * deltaTime;
            moved = true;
        }
        
        if (moved)
        {
            island.needsPhysicsUpdate = true;
            island.invalidateTransform();
            
            for (auto& [chunkCoord, chunk] : island.chunks)
            {
                if (chunk) chunk->invalidateCachedWorldAABB();
            }
        }
    }
}

void IslandChunkSystem::updatePlayerChunks(const Vec3& playerPosition)
{
    // Infinite world generation will be implemented in a future version
    // For now, we manually create islands in GameState
}

std::unordered_set<int64_t> IslandChunkSystem::placeWaterBasins(uint32_t islandID, const BiomePalette& palette, uint32_t seed)
{
    FloatingIsland* island = getIsland(islandID);
    if (!island) return std::unordered_set<int64_t>();
    
    auto& chunkMap = island->chunks;
    
    // Helper lambda: Direct voxel lookup without mutex
    auto getVoxelDirect = [&](const Vec3& pos) -> uint8_t {
        int chunkX = static_cast<int>(std::floor(pos.x / VoxelChunk::SIZE));
        int chunkY = static_cast<int>(std::floor(pos.y / VoxelChunk::SIZE));
        int chunkZ = static_cast<int>(std::floor(pos.z / VoxelChunk::SIZE));
        Vec3 chunkCoord(chunkX, chunkY, chunkZ);
        
        auto it = chunkMap.find(chunkCoord);
        if (it == chunkMap.end() || !it->second) return 0;
        
        int localX = static_cast<int>(std::floor(pos.x)) - (chunkX * VoxelChunk::SIZE);
        int localY = static_cast<int>(std::floor(pos.y)) - (chunkY * VoxelChunk::SIZE);
        int localZ = static_cast<int>(std::floor(pos.z)) - (chunkZ * VoxelChunk::SIZE);
        
        return it->second->getVoxel(localX, localY, localZ);
    };
    
    auto encodePos = [](const Vec3& p) -> int64_t {
        return (static_cast<int64_t>(p.x + 32768) << 32) | 
               (static_cast<int64_t>(p.y + 32768) << 16) | 
               static_cast<int64_t>(p.z + 32768);
    };
    
    std::unordered_set<int64_t> waterPositions;
    int waterBlocksPlaced = 0;
    
    for (auto& [chunkCoord, chunk] : island->chunks)
    {
        if (!chunk) continue;
        
        // Cache chunk above (most water checks look upward)
        Vec3 aboveChunkCoord = chunkCoord + Vec3(0, 1, 0);
        auto aboveIt = chunkMap.find(aboveChunkCoord);
        VoxelChunk* aboveChunk = (aboveIt != chunkMap.end()) ? aboveIt->second.get() : nullptr;
        
        for (int lz = 0; lz < VoxelChunk::SIZE; ++lz)
        {
            for (int lx = 0; lx < VoxelChunk::SIZE; ++lx)
            {
                for (int ly = 0; ly < VoxelChunk::SIZE; ++ly)
                {
                    uint8_t blockID = chunk->getVoxel(lx, ly, lz);
                    if (blockID != palette.surfaceBlock) continue;
                    
                    int wx = static_cast<int>(chunkCoord.x) * VoxelChunk::SIZE + lx;
                    int wy = static_cast<int>(chunkCoord.y) * VoxelChunk::SIZE + ly;
                    int wz = static_cast<int>(chunkCoord.z) * VoxelChunk::SIZE + lz;
                    
                    // Check block above for air (water placement position)
                    int waterY = wy + 1;
                    uint8_t aboveBlock = BlockID::AIR;
                    
                    // Determine if above position is in current chunk or chunk above
                    if (ly == VoxelChunk::SIZE - 1) {
                        // Crosses into chunk above
                        if (aboveChunk) {
                            aboveBlock = aboveChunk->getVoxel(lx, 0, lz);
                        }
                    } else {
                        // Same chunk, next Y level
                        aboveBlock = chunk->getVoxel(lx, ly + 1, lz);
                    }
                    
                    // Place water one block above surface if air
                    if (aboveBlock == BlockID::AIR)
                    {
                        Vec3 waterPos(wx, waterY, wz);
                        setBlockIDWithAutoChunk(islandID, waterPos, palette.waterBlock);
                        waterPositions.insert(encodePos(waterPos));
                        waterBlocksPlaced++;
                    }
                }
            }
        }
    }
    
    std::cout << "   └─ Initial Water: " << waterBlocksPlaced << " blocks" << std::endl;
    return waterPositions;
}

void IslandChunkSystem::cullExposedWater(uint32_t islandID, const std::unordered_set<int64_t>* waterPositionsToCheck)
{
    FloatingIsland* island = getIsland(islandID);
    if (!island) return;
    
    auto& chunkMap = island->chunks;
    
    // Helper lambda: Cached voxel lookup (avoid hash map lookups)
    VoxelChunk* cachedChunk = nullptr;
    Vec3 cachedChunkCoord(-999999, -999999, -999999);
    
    auto getVoxelCached = [&](int wx, int wy, int wz) -> uint8_t {
        int chunkX = static_cast<int>(std::floor(static_cast<float>(wx) / VoxelChunk::SIZE));
        int chunkY = static_cast<int>(std::floor(static_cast<float>(wy) / VoxelChunk::SIZE));
        int chunkZ = static_cast<int>(std::floor(static_cast<float>(wz) / VoxelChunk::SIZE));
        
        if (cachedChunk == nullptr || cachedChunkCoord.x != chunkX || 
            cachedChunkCoord.y != chunkY || cachedChunkCoord.z != chunkZ) {
            Vec3 chunkCoord(chunkX, chunkY, chunkZ);
            auto it = chunkMap.find(chunkCoord);
            if (it == chunkMap.end() || !it->second) return BlockID::AIR;
            cachedChunk = it->second.get();
            cachedChunkCoord = chunkCoord;
        }
        
        int localX = wx - (static_cast<int>(cachedChunkCoord.x) * VoxelChunk::SIZE);
        int localY = wy - (static_cast<int>(cachedChunkCoord.y) * VoxelChunk::SIZE);
        int localZ = wz - (static_cast<int>(cachedChunkCoord.z) * VoxelChunk::SIZE);
        
        return cachedChunk->getVoxel(localX, localY, localZ);
    };
    
    // Helper lambda: Direct voxel lookup without mutex (for Vec3 positions)
    auto getVoxelDirect = [&](const Vec3& pos) -> uint8_t {
        return getVoxelCached(static_cast<int>(pos.x), static_cast<int>(pos.y), static_cast<int>(pos.z));
    };
    
    static const Vec3 neighbors[6] = {
        Vec3(1, 0, 0), Vec3(-1, 0, 0),
        Vec3(0, 1, 0), Vec3(0, -1, 0),
        Vec3(0, 0, 1), Vec3(0, 0, -1)
    };
    
    auto encodePos = [](const Vec3& p) -> int64_t {
        return (static_cast<int64_t>(p.x + 32768) << 32) | 
               (static_cast<int64_t>(p.y + 32768) << 16) | 
               static_cast<int64_t>(p.z + 32768);
    };
    
    auto decodePos = [](int64_t key) -> Vec3 {
        int x = static_cast<int>((key >> 32) & 0xFFFF) - 32768;
        int y = static_cast<int>((key >> 16) & 0xFFFF) - 32768;
        int z = static_cast<int>(key & 0xFFFF) - 32768;
        return Vec3(x, y, z);
    };
    
    // PHASE 1: Find all water blocks with exposed leaks (side/bottom air)
    std::vector<Vec3> exposedWater;
    std::unordered_set<int64_t> allWater;
    
    // OPTIMIZATION: If specific water positions provided, only check those
    if (waterPositionsToCheck && !waterPositionsToCheck->empty())
    {
        // Check only the specified water positions
        for (int64_t waterKey : *waterPositionsToCheck)
        {
            Vec3 worldPos = decodePos(waterKey);
            int wx = static_cast<int>(worldPos.x);
            int wy = static_cast<int>(worldPos.y);
            int wz = static_cast<int>(worldPos.z);
            
            uint8_t blockID = getVoxelCached(wx, wy, wz);
            if (blockID != BlockID::WATER && blockID != BlockID::ICE && blockID != BlockID::LAVA)
                continue;
            
            allWater.insert(waterKey);
            
            // Check if any SIDE or BOTTOM neighbor is air (exposed leak)
            bool hasExposedLeak = false;
            for (int i = 0; i < 6; ++i)
            {
                if (i == 2) continue; // Skip checking above
                
                const Vec3& delta = neighbors[i];
                int nx = wx + static_cast<int>(delta.x);
                int ny = wy + static_cast<int>(delta.y);
                int nz = wz + static_cast<int>(delta.z);
                
                if (getVoxelCached(nx, ny, nz) == BlockID::AIR)
                {
                    hasExposedLeak = true;
                    break;
                }
            }
            
            if (hasExposedLeak)
            {
                exposedWater.push_back(worldPos);
            }
        }
    }
    else
    {
        // Check all water in all chunks (original behavior)
        for (auto& [chunkCoord, chunk] : island->chunks)
        {
            if (!chunk) continue;
            
            for (int lz = 0; lz < VoxelChunk::SIZE; ++lz)
            {
                for (int ly = 0; ly < VoxelChunk::SIZE; ++ly)
                {
                    for (int lx = 0; lx < VoxelChunk::SIZE; ++lx)
                    {
                        uint8_t blockID = chunk->getVoxel(lx, ly, lz);
                        
                        // Check if this is a water-type block
                        if (blockID != BlockID::WATER && blockID != BlockID::ICE && blockID != BlockID::LAVA)
                            continue;
                        
                        int wx = static_cast<int>(chunkCoord.x) * VoxelChunk::SIZE + lx;
                        int wy = static_cast<int>(chunkCoord.y) * VoxelChunk::SIZE + ly;
                        int wz = static_cast<int>(chunkCoord.z) * VoxelChunk::SIZE + lz;
                        Vec3 worldPos(wx, wy, wz);
                        allWater.insert(encodePos(worldPos));
                        
                        // Check if any SIDE or BOTTOM neighbor is air (exposed leak)
                        bool hasExposedLeak = false;
                        for (int i = 0; i < 6; ++i)
                        {
                            if (i == 2) continue; // Skip checking above
                            
                            const Vec3& delta = neighbors[i];
                            int nx = wx + static_cast<int>(delta.x);
                            int ny = wy + static_cast<int>(delta.y);
                            int nz = wz + static_cast<int>(delta.z);
                            
                            if (getVoxelCached(nx, ny, nz) == BlockID::AIR)
                            {
                                hasExposedLeak = true;
                                break;
                            }
                        }
                        
                        if (hasExposedLeak)
                        {
                            exposedWater.push_back(worldPos);
                        }
                    }
                }
            }
        }
    }
    
    // PHASE 2: Flood-fill from exposed water to find all HORIZONTALLY connected water
    // Only expand in X/Z directions, NOT vertically (allows layered water)
    static const Vec3 horizNeighbors[4] = {
        Vec3(1, 0, 0), Vec3(-1, 0, 0),
        Vec3(0, 0, 1), Vec3(0, 0, -1)
    };
    
    std::unordered_set<int64_t> toRemove;
    std::queue<Vec3> floodQueue;
    
    for (const Vec3& exposed : exposedWater)
    {
        int64_t key = encodePos(exposed);
        if (toRemove.count(key)) continue; // Already marked
        
        floodQueue.push(exposed);
        toRemove.insert(key);
        
        while (!floodQueue.empty())
        {
            Vec3 current = floodQueue.front();
            floodQueue.pop();
            
            // Check ONLY horizontal neighbors for connected water (X/Z only)
            for (const Vec3& delta : horizNeighbors)
            {
                Vec3 neighborPos = current + delta;
                int64_t neighborKey = encodePos(neighborPos);
                
                // Skip if not water or already marked
                if (!allWater.count(neighborKey)) continue;
                if (toRemove.count(neighborKey)) continue;
                
                // Mark and flood from this water block
                toRemove.insert(neighborKey);
                floodQueue.push(neighborPos);
            }
        }
    }
    
    // PHASE 3: Remove all connected water blocks
    for (int64_t key : toRemove)
    {
        // Decode position
        int x = static_cast<int>((key >> 32) & 0xFFFF) - 32768;
        int y = static_cast<int>((key >> 16) & 0xFFFF) - 32768;
        int z = static_cast<int>(key & 0xFFFF) - 32768;
        
        setBlockIDWithAutoChunk(islandID, Vec3(x, y, z), BlockID::AIR);
    }
    
    std::cout << "   └─ Water Culled: " << toRemove.size() << " blocks (flood-fill from " 
              << exposedWater.size() << " leak points)" << std::endl;
}

void IslandChunkSystem::generateChunksAroundPoint(const Vec3& center)
{
    // Chunk generation around points will be used for infinite world expansion
    // Currently handled manually through createIsland()
}


