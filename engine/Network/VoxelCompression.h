// VoxelCompression.h - LZ4 compression for voxel chunk data
#pragma once
#include <cstdint>
#include <vector>

/**
 * LZ4 compression wrapper optimized for voxel data.
 * LZ4 provides fast compression/decompression with good compression ratios
 * for voxel chunks, which often contain repetitive patterns.
 */
class VoxelCompression {
public:
    /**
     * Compress voxel data using LZ4
     * @param input - Raw voxel data (VoxelChunk::VOLUME bytes)
     * @param inputSize - Size of input data 
     * @param output - Vector to store compressed data
     * @return Size of compressed data, or 0 if compression failed
     */
    static uint32_t compressLZ4(const uint8_t* input, uint32_t inputSize, std::vector<uint8_t>& output);
    
    /**
     * Decompress LZ4-compressed voxel data
     * @param input - Compressed data
     * @param inputSize - Size of compressed data
     * @param output - Buffer to store decompressed data (must be pre-allocated to VoxelChunk::VOLUME)
     * @param outputSize - Expected size of decompressed data (VoxelChunk::VOLUME for standard chunk)
     * @return true if decompression succeeded
     */
    static bool decompressLZ4(const uint8_t* input, uint32_t inputSize, uint8_t* output, uint32_t outputSize);
    
    /**
     * Get maximum compressed size for given input size
     * @param inputSize - Size of input data
     * @return Maximum possible compressed size
     */
    static uint32_t getMaxCompressedSize(uint32_t inputSize);
    
    // Legacy methods for backward compatibility during transition
    static uint32_t compressRLE(const uint8_t* input, uint32_t inputSize, std::vector<uint8_t>& output) {
        return compressLZ4(input, inputSize, output);
    }
    
    static bool decompressRLE(const uint8_t* input, uint32_t inputSize, uint8_t* output, uint32_t outputSize) {
        return decompressLZ4(input, inputSize, output, outputSize);
    }
};
