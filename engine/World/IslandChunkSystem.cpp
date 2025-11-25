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
#include <unordered_map>
#include <queue>
#include <deque>
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>

#include "VoxelChunk.h"
#include "BlockType.h"
#include "TreeGenerator.h"
#include "../Profiling/Profiler.h"
#include "../libs/FastNoiseSIMD/FastNoiseSIMD.h"
#include "../Culling/Frustum.h"
#include "../../libs/FastNoiseLite/FastNoiseLite.h"

// Sparse bitset: 16KB chunks only allocated where needed
// THREAD-SAFE: Uses atomic operations for parallel BFS
// Memory: ~16KB per 128³ region with voxels (vs 50 bytes/voxel for hash set)
struct SparseBitset {
    static constexpr int CHUNK_BITS = 17;  // 128K bits = 16KB per chunk
    static constexpr int CHUNK_SIZE = 1 << CHUNK_BITS;
    static constexpr uint64_t CHUNK_MASK = CHUNK_SIZE - 1;
    static constexpr int WORDS_PER_CHUNK = CHUNK_SIZE / 64;  // 2048 uint64_t per chunk
    
    std::unordered_map<int64_t, std::atomic<uint64_t>*> chunks;
    mutable std::mutex chunksMutex;  // Protects map modifications only (mutable for const functions)
    
    ~SparseBitset() {
        for (auto& [key, ptr] : chunks) {
            delete[] ptr;
        }
    }
    
    // Thread-safe set - returns true if this thread set the bit (was previously unset)
    bool testAndSet(int64_t hash) {
        int64_t chunkKey = hash >> CHUNK_BITS;
        uint64_t bitIdx = hash & CHUNK_MASK;
        
        std::atomic<uint64_t>* chunk = getOrCreateChunk(chunkKey);
        
        uint64_t wordIdx = bitIdx >> 6;
        uint64_t bitMask = 1ULL << (bitIdx & 63);
        
        // Atomic fetch_or returns OLD value
        uint64_t oldValue = chunk[wordIdx].fetch_or(bitMask, std::memory_order_relaxed);
        return (oldValue & bitMask) == 0;  // True if we set it (wasn't set before)
    }
    
    bool test(int64_t hash) const {
        int64_t chunkKey = hash >> CHUNK_BITS;
        std::lock_guard<std::mutex> lock(chunksMutex);
        auto it = chunks.find(chunkKey);
        if (it == chunks.end()) return false;
        
        uint64_t bitIdx = hash & CHUNK_MASK;
        uint64_t wordIdx = bitIdx >> 6;
        uint64_t bitMask = 1ULL << (bitIdx & 63);
        return (it->second[wordIdx].load(std::memory_order_relaxed) & bitMask) != 0;
    }
    
    size_t memoryUsage() const {
        std::lock_guard<std::mutex> lock(chunksMutex);
        return chunks.size() * WORDS_PER_CHUNK * sizeof(std::atomic<uint64_t>);
    }
    
private:
    std::atomic<uint64_t>* getOrCreateChunk(int64_t chunkKey) {
        // Fast path: chunk already exists
        {
            std::lock_guard<std::mutex> lock(chunksMutex);
            auto it = chunks.find(chunkKey);
            if (it != chunks.end()) return it->second;
        }
        
        // Slow path: allocate new chunk
        auto* newChunk = new std::atomic<uint64_t>[WORDS_PER_CHUNK];
        for (int i = 0; i < WORDS_PER_CHUNK; ++i) {
            newChunk[i].store(0, std::memory_order_relaxed);
        }
        
        std::lock_guard<std::mutex> lock(chunksMutex);
        // Double-check another thread didn't create it
        auto it = chunks.find(chunkKey);
        if (it != chunks.end()) {
            delete[] newChunk;
            return it->second;
        }
        
        chunks[chunkKey] = newChunk;
        return newChunk;
    }
};

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

    // Create new chunk and set island context with transform
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
    try {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    FloatingIsland* island = getIsland(islandID);
    if (!island) {
        std::cerr << "ERROR: Island " << islandID << " not found!" << std::endl;
        return;
    }

    // Get biome palette for block selection
    BiomeSystem biomeSystem;
    BiomePalette palette = biomeSystem.getPalette(biome);
    
    // Biome assigned

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
    float densityThreshold = 0.25f;  // Lower = more solid/contiguous terrain (was 0.35)
    float baseHeightRatio = 0.15f;  // Height as a factor of radius (doubled for taller islands)
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
    
    // Sample every 8th block (8x8x8 grid) for 512x memory reduction + 512x faster generation
    // Trilinear interpolation fills in the gaps with imperceptible quality loss
    constexpr int NOISE_SAMPLE_RATE = 8;  // Was 4, now 8 for 8x additional memory savings
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
    // Use aligned memory allocation for SIMD operations
    float* noiseValues = FastNoiseSIMD::GetEmptySet(gridSizeX, gridSizeY, gridSizeZ);
    simdNoise->FillNoiseSet(noiseValues,
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
    
    // Free SIMD-aligned memory
    FastNoiseSIMD::FreeNoiseSet(noiseValues);
    
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
    
    // **OCTREE BFS: TWO-PHASE GENERATION**
    // Phase 1: Block-level BFS for connectivity
    // Phase 2: Per-voxel placement within connected blocks
    // Block size scales with chunk size: use half chunk size for coarse BFS
    const int BLOCK_SIZE = VoxelChunk::SIZE / 2;  // 64→32, 128→64, 256→128
    float radiusSquared = (radius * 1.4f) * (radius * 1.4f);
    float radiusDivisor = 1.0f / (radius * 1.2f);
    
    long long voxelsGenerated = 0;
    long long voxelsSampled = 0;
    long long blocksChecked = 0;
    
    // Chunk cache for voxel placement
    VoxelChunk* cachedChunk = nullptr;
    Vec3 cachedChunkCoord(-999999, -999999, -999999);
    
    auto setVoxelDirect = [&](const Vec3& pos, uint8_t blockID) {
        int chunkX = static_cast<int>(std::floor(pos.x / VoxelChunk::SIZE));
        int chunkY = static_cast<int>(std::floor(pos.y / VoxelChunk::SIZE));
        int chunkZ = static_cast<int>(std::floor(pos.z / VoxelChunk::SIZE));
        
        int localX = static_cast<int>(std::floor(pos.x)) - (chunkX * VoxelChunk::SIZE);
        int localY = static_cast<int>(std::floor(pos.y)) - (chunkY * VoxelChunk::SIZE);
        int localZ = static_cast<int>(std::floor(pos.z)) - (chunkZ * VoxelChunk::SIZE);
        
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
    
    static const Vec3 neighbors[6] = {
        Vec3(1, 0, 0), Vec3(-1, 0, 0),
        Vec3(0, 1, 0), Vec3(0, -1, 0),
        Vec3(0, 0, 1), Vec3(0, 0, -1)
    };
    
    const float islandHeightRange = islandHeight * 2.0f;
    const float invHeightRange = 1.0f / islandHeightRange;
    const float heightOffset = static_cast<float>(islandHeight);
    
    // Surface block collection for water/vegetation placement
    std::vector<Vec3> surfaceBlockPositions;
    surfaceBlockPositions.reserve(100000);
    
    // PHASE 1: Block-level BFS with inline voxel placement and surface detection
    auto blockBFSStart = std::chrono::high_resolution_clock::now();
    
    struct BlockCoord {
        int x, y, z;
        bool operator==(const BlockCoord& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    
    struct BlockHash {
        size_t operator()(const BlockCoord& b) const {
            return std::hash<int>()(b.x) ^ (std::hash<int>()(b.y) << 1) ^ (std::hash<int>()(b.z) << 2);
        }
    };
    
    std::unordered_set<BlockCoord, BlockHash> connectedBlocks;
    std::unordered_set<BlockCoord, BlockHash> visitedBlocks;
    std::vector<BlockCoord> currentBlockLevel;
    std::vector<BlockCoord> nextBlockLevel;
    
    BlockCoord startBlock = {0, 0, 0};
    currentBlockLevel.push_back(startBlock);
    visitedBlocks.insert(startBlock);
    
    while (!currentBlockLevel.empty()) {
        nextBlockLevel.clear();
        
        for (const BlockCoord& blockCoord : currentBlockLevel) {
            blocksChecked++;
            
            // Sample density at block center
            float centerX = blockCoord.x * BLOCK_SIZE + BLOCK_SIZE * 0.5f;
            float centerY = blockCoord.y * BLOCK_SIZE + BLOCK_SIZE * 0.5f;
            float centerZ = blockCoord.z * BLOCK_SIZE + BLOCK_SIZE * 0.5f;
            
            // Bounds check
            if (centerY < -heightOffset || centerY > heightOffset) continue;
            float distSq = centerX * centerX + centerY * centerY + centerZ * centerZ;
            if (distSq > radiusSquared) continue;
            
            // Density check
            float normalizedY = (centerY + heightOffset) * invHeightRange;
            float centerOffset = normalizedY - 0.5f;
            float verticalDensity = 1.0f - (centerOffset * centerOffset * 4.0f);
            verticalDensity = verticalDensity > 0.0f ? verticalDensity : 0.0f;
            
            float distFromCenter = std::sqrt(distSq);
            float islandBase = 1.0f - (distFromCenter * radiusDivisor);
            islandBase = islandBase > 0.0f ? islandBase : 0.0f;
            islandBase = islandBase * islandBase;
            
            float noise = sampleNoise(centerX, centerY, centerZ);
            float blockDensity = islandBase * verticalDensity * noise;
            
            // If block has sufficient density, mark as connected, place voxels, and queue neighbors
            if (blockDensity > densityThreshold * 0.5f) {  // Lower threshold for blocks
                connectedBlocks.insert(blockCoord);
                
                // Place voxels in this block immediately
                int baseX = blockCoord.x * BLOCK_SIZE;
                int baseY = blockCoord.y * BLOCK_SIZE;
                int baseZ = blockCoord.z * BLOCK_SIZE;
                
                // Track voxels in this block for surface detection
                std::vector<Vec3> blockVoxels;
                blockVoxels.reserve(BLOCK_SIZE * BLOCK_SIZE * BLOCK_SIZE);
                
                for (int dx = 0; dx < BLOCK_SIZE; ++dx) {
                    for (int dy = 0; dy < BLOCK_SIZE; ++dy) {
                        for (int dz = 0; dz < BLOCK_SIZE; ++dz) {
                            float x = baseX + dx;
                            float y = baseY + dy;
                            float z = baseZ + dz;
                            
                            voxelsSampled++;
                            
                            // Bounds check
                            if (y < -heightOffset || y > heightOffset) continue;
                            float distanceSquared = x * x + y * y + z * z;
                            if (distanceSquared > radiusSquared) continue;
                            
                            // Density calculation (per-voxel for organic edges)
                            float normalizedY = (y + heightOffset) * invHeightRange;
                            float centerOffset = normalizedY - 0.5f;
                            float verticalDensity = 1.0f - (centerOffset * centerOffset * 4.0f);
                            verticalDensity = verticalDensity > 0.0f ? verticalDensity : 0.0f;
                            if (verticalDensity < 0.01f) continue;
                            
                            float distanceFromCenter = std::sqrt(distanceSquared);
                            float islandBase = 1.0f - (distanceFromCenter * radiusDivisor);
                            islandBase = islandBase > 0.0f ? islandBase : 0.0f;
                            islandBase = islandBase * islandBase;
                            if (islandBase < 0.01f) continue;
                            
                            float noise = sampleNoise(x, y, z);
                            float finalDensity = islandBase * verticalDensity * noise;
                            
                            if (finalDensity > densityThreshold) {
                                Vec3 pos(x, y, z);
                                setVoxelDirect(pos, palette.deepBlock);
                                blockVoxels.push_back(pos);
                                voxelsGenerated++;
                            }
                        }
                    }
                }
                
                // Detect surface voxels in this block (has air neighbor)
                for (const Vec3& voxelPos : blockVoxels) {
                    bool hasAirNeighbor = false;
                    for (const Vec3& delta : neighbors) {
                        Vec3 neighborPos = voxelPos + delta;
                        
                        // Use cached chunk lookup
                        int nx = static_cast<int>(neighborPos.x);
                        int ny = static_cast<int>(neighborPos.y);
                        int nz = static_cast<int>(neighborPos.z);
                        int nChunkX = static_cast<int>(std::floor(static_cast<float>(nx) / VoxelChunk::SIZE));
                        int nChunkY = static_cast<int>(std::floor(static_cast<float>(ny) / VoxelChunk::SIZE));
                        int nChunkZ = static_cast<int>(std::floor(static_cast<float>(nz) / VoxelChunk::SIZE));
                        
                        uint8_t neighborBlock = BlockID::AIR;
                        if (cachedChunk == nullptr || cachedChunkCoord.x != nChunkX || 
                            cachedChunkCoord.y != nChunkY || cachedChunkCoord.z != nChunkZ) {
                            Vec3 nChunkCoord(nChunkX, nChunkY, nChunkZ);
                            auto it = island->chunks.find(nChunkCoord);
                            if (it != island->chunks.end() && it->second) {
                                cachedChunk = it->second.get();
                                cachedChunkCoord = nChunkCoord;
                                int localX = nx - (nChunkX * VoxelChunk::SIZE);
                                int localY = ny - (nChunkY * VoxelChunk::SIZE);
                                int localZ = nz - (nChunkZ * VoxelChunk::SIZE);
                                neighborBlock = cachedChunk->getVoxel(localX, localY, localZ);
                            }
                        } else {
                            int localX = nx - (nChunkX * VoxelChunk::SIZE);
                            int localY = ny - (nChunkY * VoxelChunk::SIZE);
                            int localZ = nz - (nChunkZ * VoxelChunk::SIZE);
                            neighborBlock = cachedChunk->getVoxel(localX, localY, localZ);
                        }
                        
                        if (neighborBlock == BlockID::AIR) {
                            hasAirNeighbor = true;
                            break;
                        }
                    }
                    if (hasAirNeighbor) {
                        setVoxelDirect(voxelPos, palette.surfaceBlock);
                        surfaceBlockPositions.push_back(voxelPos);
                    }
                }
                
                // Queue neighbor blocks
                for (const Vec3& delta : neighbors) {
                    BlockCoord neighbor = {
                        blockCoord.x + static_cast<int>(delta.x),
                        blockCoord.y + static_cast<int>(delta.y),
                        blockCoord.z + static_cast<int>(delta.z)
                    };
                    
                    if (visitedBlocks.find(neighbor) == visitedBlocks.end()) {
                        visitedBlocks.insert(neighbor);
                        nextBlockLevel.push_back(neighbor);
                    }
                }
            }
        }
        
        std::swap(currentBlockLevel, nextBlockLevel);
    }
    
    auto voxelGenEnd = std::chrono::high_resolution_clock::now();
    auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(voxelGenEnd - voxelGenStart).count();
    
    size_t memoryUsed = connectedBlocks.size() * sizeof(BlockCoord) + visitedBlocks.size() * sizeof(BlockCoord);
    std::cout << "🔨 Voxel Generation (Octree BFS): " << totalDuration << "ms (" << voxelsGenerated << " voxels, " 
              << island->chunks.size() << " chunks, " << surfaceBlockPositions.size() << " surface)" << std::endl;
    std::cout << "   └─ Samples: " << voxelsSampled << std::endl;
    std::cout << "   └─ Memory: " << (memoryUsed / 1024) << " KB (visited tracking)" << std::endl;
    
    // **WATER PLACEMENT PASS**
    auto waterStart = std::chrono::high_resolution_clock::now();
    
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
    
    std::vector<Vec3> currentSurface = surfaceBlockPositions;
    int totalWaterPlaced = 0;
    int totalWaterCulled = 0;
    int iterations = 0;
    int maxIterations = 20;
    std::unordered_set<int> activeYLevels; // Track Y levels with water
    
    while (iterations < maxIterations) {
        // Place water above current surface
        int waterPlaced = 0;
        std::unordered_set<int> newYLevels;
        
        for (const Vec3& surfacePos : currentSurface) {
            // After first iteration, skip positions not at active Y levels
            if (iterations > 0) {
                int posY = static_cast<int>(surfacePos.y);
                if (activeYLevels.find(posY) == activeYLevels.end() && 
                    activeYLevels.find(posY - 1) == activeYLevels.end()) {
                    continue;
                }
            }
            
            Vec3 abovePos = surfacePos + Vec3(0, 1, 0);
            if (getVoxelDirect(abovePos) == BlockID::AIR) {
                setVoxelDirect(abovePos, palette.waterBlock);
                newYLevels.insert(static_cast<int>(abovePos.y));
                waterPlaced++;
            }
        }
        
        if (waterPlaced == 0) break;
        
        // Cull exposed water using flood fill - ONLY check newly placed water
        std::unordered_set<int64_t> toRemove;
        std::queue<Vec3> floodQueue;
        std::unordered_set<int64_t> newlyPlacedWater; // Track what we just placed
        
        // Find all newly placed water blocks with exposed sides or bottom
        for (const Vec3& surfacePos : currentSurface) {
            // Skip if not at active Y level (after first iteration)
            if (iterations > 0) {
                int posY = static_cast<int>(surfacePos.y);
                if (activeYLevels.find(posY) == activeYLevels.end() && 
                    activeYLevels.find(posY - 1) == activeYLevels.end()) {
                    continue;
                }
            }
            
            Vec3 waterPos = surfacePos + Vec3(0, 1, 0);
            if (getVoxelDirect(waterPos) != palette.waterBlock) continue;
            
            int64_t waterKey = encodePos(waterPos);
            if (newYLevels.find(static_cast<int>(waterPos.y)) != newYLevels.end()) {
                newlyPlacedWater.insert(waterKey);
            }
            
            // Only check newly placed water for exposure
            if (newlyPlacedWater.find(waterKey) == newlyPlacedWater.end()) continue;
            
            // Check bottom + 4 sides (skip top)
            bool hasExposedFace = false;
            for (int n = 0; n < 6; ++n) {
                if (n == 2) continue; // Skip top
                Vec3 neighborPos = waterPos + neighbors[n];
                if (getVoxelDirect(neighborPos) == BlockID::AIR) {
                    hasExposedFace = true;
                    break;
                }
            }
            
            if (hasExposedFace) {
                if (toRemove.find(waterKey) == toRemove.end()) {
                    floodQueue.push(waterPos);
                    toRemove.insert(waterKey);
                }
            }
        }
        
        // Flood fill to find all connected exposed water - ONLY at newly placed Y levels
        while (!floodQueue.empty()) {
            Vec3 current = floodQueue.front();
            floodQueue.pop();
            
            // Check all 6 neighbors
            for (int n = 0; n < 6; ++n) {
                Vec3 neighborPos = current + neighbors[n];
                
                // Only spread to water at the same Y levels we just placed
                if (newYLevels.find(static_cast<int>(neighborPos.y)) == newYLevels.end()) {
                    continue;
                }
                
                if (getVoxelDirect(neighborPos) == palette.waterBlock) {
                    int64_t key = encodePos(neighborPos);
                    if (toRemove.find(key) == toRemove.end()) {
                        toRemove.insert(key);
                        floodQueue.push(neighborPos);
                    }
                }
            }
        }
        
        // Remove all exposed water
        for (int64_t key : toRemove) {
            Vec3 pos = decodePos(key);
            setVoxelDirect(pos, BlockID::AIR);
        }
        
        int culled = toRemove.size();
        int survived = waterPlaced - culled;
        totalWaterPlaced += survived;
        totalWaterCulled += culled;
        
        std::cout << "   └─ Iteration " << iterations << ": placed=" << waterPlaced 
                  << ", culled=" << culled << ", survived=" << survived << std::endl;
        
        // If we culled everything we placed, we're done
        if (culled == waterPlaced) break;
        
        // Update active Y levels and surface positions
        activeYLevels.clear();
        for (Vec3& surfacePos : currentSurface) {
            Vec3 waterPos = surfacePos + Vec3(0, 1, 0);
            if (getVoxelDirect(waterPos) == palette.waterBlock) {
                // Water survived, surface rises to water position
                surfacePos = waterPos;
                activeYLevels.insert(static_cast<int>(waterPos.y));
            }
            // If no water survived, surfacePos stays at current height
        }
        
        iterations++;
    }
    
    auto waterEnd = std::chrono::high_resolution_clock::now();
    auto waterDuration = std::chrono::duration_cast<std::chrono::milliseconds>(waterEnd - waterStart).count();
    std::cout << "💧 Water: " << waterDuration << "ms (" << totalWaterPlaced << " blocks, " << totalWaterCulled << " culled, " << iterations << " iterations)" << std::endl;
    
    // **VEGETATION DECORATION PASS**
    auto decorationStart = std::chrono::high_resolution_clock::now();
    
    int grassPlaced = 0;
    int treesPlaced = 0;
    
    // Use biome-specific vegetation density - SPARSE trees for Terralith-like feel
    float grassChance = palette.vegetationDensity * 80.0f;  // Lots of grass models (0-80%)
    float treeChance = palette.vegetationDensity * 1.5f;    // Much sparser trees (0-1.5%)
    
    // Use collected surface block positions
    for (const Vec3& surfacePos : surfaceBlockPositions)
    {
        Vec3 abovePos = surfacePos + Vec3(0, 1, 0);
        uint8_t blockAbove = getVoxelDirect(abovePos);
        
        if (blockAbove != BlockID::AIR) continue;
        
        float roll = (std::rand() % 100);
        
        if (roll < treeChance)
        {
            uint32_t treeSeed = static_cast<uint32_t>(
                static_cast<int>(abovePos.x) * 73856093 ^ 
                static_cast<int>(abovePos.y) * 19349663 ^ 
                static_cast<int>(abovePos.z) * 83492791
            );
            TreeGenerator::generateTree(this, islandID, abovePos, treeSeed, palette.vegetationDensity);
            treesPlaced++;
        }
        else if (roll < grassChance)
        {
            setVoxelDirect(abovePos, BlockID::DECOR_GRASS);
            grassPlaced++;
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
    auto finalTotalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - startTime).count();
    
    std::cout << "✅ Island Generation Complete: " << finalTotalDuration << "ms total" << std::endl;
    std::cout << "   └─ Breakdown: Voxels=" << totalDuration << "ms (" 
              << (totalDuration * 100 / std::max(1LL, finalTotalDuration)) << "%)" << std::endl;
    
    // Report memory usage for this island
    size_t islandMemory = 0;
    for (const auto& [chunkCoord, chunkPtr] : island->chunks)
    {
        if (chunkPtr)
        {
            islandMemory += chunkPtr->getMemoryUsage();
        }
    }
    std::cout << "   └─ Memory: " << (islandMemory / (1024 * 1024)) << " MB (" 
              << island->chunks.size() << " chunks, all ACTIVE)" << std::endl;
    
    } catch (const std::exception& e) {
        std::cerr << "FATAL ERROR in island generation (ID=" << islandID << ", radius=" << radius << "): " << e.what() << std::endl;
        throw;  // Re-throw to propagate to async handler
    } catch (...) {
        std::cerr << "FATAL ERROR in island generation (ID=" << islandID << ", radius=" << radius << "): Unknown exception" << std::endl;
        throw;
    }
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
    PROFILE_SCOPE("IslandChunkSystem::updateIslandPhysics");
    
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

void IslandChunkSystem::updateChunkStates(const Vec3& playerPosition)
{
    // Update chunk states based on distance from player
    // ACTIVE: Within interaction distance (can be modified)
    // INACTIVE: Beyond interaction distance but within render distance (compressed, mesh on GPU)
    
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    
    float interactionDistSq = static_cast<float>(m_interactionDistance * VoxelChunk::SIZE * m_interactionDistance * VoxelChunk::SIZE);
    float renderDistSq = static_cast<float>(m_renderDistance * VoxelChunk::SIZE * m_renderDistance * VoxelChunk::SIZE);
    
    for (auto& [islandID, island] : m_islands)
    {
        // Convert player position to island-relative coordinates
        glm::mat4 invTransform = glm::inverse(island.getTransformMatrix());
        glm::vec4 islandRelativePos = invTransform * glm::vec4(playerPosition.x, playerPosition.y, playerPosition.z, 1.0f);
        Vec3 playerPosInIsland(islandRelativePos.x, islandRelativePos.y, islandRelativePos.z);
        
        for (auto& [chunkCoord, chunkPtr] : island.chunks)
        {
            if (!chunkPtr) continue;
            
            // Calculate chunk center in island-relative space
            Vec3 chunkCenter(
                chunkCoord.x * VoxelChunk::SIZE + VoxelChunk::SIZE * 0.5f,
                chunkCoord.y * VoxelChunk::SIZE + VoxelChunk::SIZE * 0.5f,
                chunkCoord.z * VoxelChunk::SIZE + VoxelChunk::SIZE * 0.5f
            );
            
            float dx = chunkCenter.x - playerPosInIsland.x;
            float dy = chunkCenter.y - playerPosInIsland.y;
            float dz = chunkCenter.z - playerPosInIsland.z;
            float distSq = dx * dx + dy * dy + dz * dz;
            
            // Activate chunks within interaction distance
            if (distSq <= interactionDistSq)
            {
                if (!chunkPtr->isActive())
                {
                    chunkPtr->activate();
                }
            }
            // Deactivate chunks beyond interaction distance but within render distance
            else if (distSq > interactionDistSq && distSq <= renderDistSq)
            {
                if (chunkPtr->isActive())
                {
                    chunkPtr->deactivate();
                }
            }
            // Chunks beyond render distance could be unloaded, but we keep them for now
        }
    }
}

size_t IslandChunkSystem::getTotalMemoryUsage() const
{
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    
    size_t totalBytes = 0;
    size_t activeChunks = 0;
    size_t inactiveChunks = 0;
    
    for (const auto& [islandID, island] : m_islands)
    {
        for (const auto& [chunkCoord, chunkPtr] : island.chunks)
        {
            if (chunkPtr)
            {
                totalBytes += chunkPtr->getMemoryUsage();
                if (chunkPtr->isActive())
                    activeChunks++;
                else
                    inactiveChunks++;
            }
        }
    }
    
    std::cout << "📊 Chunk Memory: " << (totalBytes / (1024 * 1024)) << " MB "
              << "(" << activeChunks << " active, " << inactiveChunks << " inactive)" << std::endl;
    
    return totalBytes;
}

void IslandChunkSystem::regenerateNeighborChunkMeshes(uint32_t islandID, const Vec3& chunkCoord)
{
    // Regenerate meshes for all 6 neighboring chunks (for interchunk culling)
    // Only affects client-side chunks (server doesn't have meshes)
    if (!m_isClient) return;
    
    std::lock_guard<std::mutex> lock(m_islandsMutex);
    auto itIsl = m_islands.find(islandID);
    if (itIsl == m_islands.end()) return;
    
    FloatingIsland& island = itIsl->second;
    
    // Check all 6 neighbors: -X, +X, -Y, +Y, -Z, +Z
    static const Vec3 neighborOffsets[6] = {
        Vec3(-1, 0, 0), Vec3(1, 0, 0),
        Vec3(0, -1, 0), Vec3(0, 1, 0),
        Vec3(0, 0, -1), Vec3(0, 0, 1)
    };
    
    for (int i = 0; i < 6; ++i)
    {
        Vec3 neighborCoord = chunkCoord + neighborOffsets[i];
        auto it = island.chunks.find(neighborCoord);
        if (it != island.chunks.end() && it->second)
        {
            VoxelChunk* neighborChunk = it->second.get();
            if (neighborChunk->isClient())
            {
                neighborChunk->generateMeshAsync();
            }
        }
    }
}

void IslandChunkSystem::generateChunksAroundPoint(const Vec3& center)
{
    // Chunk generation around points will be used for infinite world expansion
    // Currently handled manually through createIsland()
}


