// VulkanBuffer.h - Buffer wrapper with VMA integration and GPU architecture awareness
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <bitset>

// Vulkan buffer wrapper with automatic memory management via VMA
// Handles both integrated GPU (direct mapping) and discrete GPU (staging) paths
class VulkanBuffer {
public:
    VulkanBuffer() = default;
    ~VulkanBuffer() { destroy(); }

    // No copy (VMA handles memory)
    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    // Move semantics
    VulkanBuffer(VulkanBuffer&& other) noexcept
        : buffer(other.buffer), allocation(other.allocation), 
          allocator(other.allocator), mappedPtr(other.mappedPtr),
          size(other.size), usageFlags(other.usageFlags),
          isPersistentlyMapped(other.isPersistentlyMapped) {
        other.buffer = VK_NULL_HANDLE;
        other.allocation = VK_NULL_HANDLE;
        other.mappedPtr = nullptr;
        other.isPersistentlyMapped = false;
    }

    VulkanBuffer& operator=(VulkanBuffer&& other) noexcept {
        if (this != &other) {
            destroy();
            buffer = other.buffer;
            allocation = other.allocation;
            allocator = other.allocator;
            mappedPtr = other.mappedPtr;
            size = other.size;
            usageFlags = other.usageFlags;
            isPersistentlyMapped = other.isPersistentlyMapped;
            other.buffer = VK_NULL_HANDLE;
            other.allocation = VK_NULL_HANDLE;
            other.mappedPtr = nullptr;
            other.isPersistentlyMapped = false;
        }
        return *this;
    }

    // Create buffer with automatic memory allocation
    // For persistent mapping: VMA_MEMORY_USAGE_CPU_TO_GPU
    // For device-local: VMA_MEMORY_USAGE_GPU_ONLY
    bool create(VmaAllocator alloc, VkDeviceSize bufferSize, 
                VkBufferUsageFlags usage, VmaMemoryUsage memUsage,
                VmaAllocationCreateFlags allocFlags = 0,
                VkMemoryPropertyFlags requiredFlags = 0,
                VkMemoryPropertyFlags preferredFlags = 0) {
        destroy();

        allocator = alloc;
        size = bufferSize;
        usageFlags = usage;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = memUsage;
        allocInfo.flags = allocFlags;
        allocInfo.requiredFlags = requiredFlags;
        allocInfo.preferredFlags = preferredFlags;

        VmaAllocationInfo allocResult;
        VkResult result = vmaCreateBuffer(allocator, &bufferInfo, &allocInfo,
                                          &buffer, &allocation, &allocResult);
        
        if (result != VK_SUCCESS) {
            // Print detailed error
            const char* errorStr = "UNKNOWN";
            switch (result) {
                case VK_ERROR_OUT_OF_HOST_MEMORY: errorStr = "OUT_OF_HOST_MEMORY"; break;
                case VK_ERROR_OUT_OF_DEVICE_MEMORY: errorStr = "OUT_OF_DEVICE_MEMORY"; break;
                case VK_ERROR_INITIALIZATION_FAILED: errorStr = "INITIALIZATION_FAILED"; break;
                case VK_ERROR_FEATURE_NOT_PRESENT: errorStr = "FEATURE_NOT_PRESENT"; break;
                case VK_ERROR_MEMORY_MAP_FAILED: errorStr = "MEMORY_MAP_FAILED"; break;
                default: break;
            }
            std::cerr << "[VulkanBuffer] Failed to create buffer: VkResult=" << result << " (" << errorStr << ")\n";
            std::cerr << "  size=" << bufferSize << " bytes, usage=" << usage << ", memUsage=" << memUsage << "\n";
            std::cerr << "  VMA alloc info: usage=" << allocInfo.usage
                      << ", flags=0x" << std::hex << allocInfo.flags
                      << ", requiredFlags=0x" << allocInfo.requiredFlags
                      << ", preferredFlags=0x" << allocInfo.preferredFlags << std::dec << "\n";

            // Optional diagnostics: log memory type bits to help identify why VMA failed
            VmaAllocatorInfo allocatorInfo{};
            vmaGetAllocatorInfo(allocator, &allocatorInfo);
            if (allocatorInfo.device != VK_NULL_HANDLE) {
                VkBuffer tempBuffer = VK_NULL_HANDLE;
                if (vkCreateBuffer(allocatorInfo.device, &bufferInfo, nullptr, &tempBuffer) == VK_SUCCESS) {
                    VkMemoryRequirements memReq{};
                    vkGetBufferMemoryRequirements(allocatorInfo.device, tempBuffer, &memReq);
                    std::cerr << "  memoryTypeBits=0x" << std::hex << memReq.memoryTypeBits << std::dec
                              << " (" << std::bitset<32>(memReq.memoryTypeBits) << ")\n";
                    if (allocatorInfo.physicalDevice != VK_NULL_HANDLE) {
                        VkPhysicalDeviceMemoryProperties memProps{};
                        vkGetPhysicalDeviceMemoryProperties(allocatorInfo.physicalDevice, &memProps);
                        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                            bool allowed = (memReq.memoryTypeBits & (1u << i)) != 0;
                            const uint32_t heapIndex = memProps.memoryTypes[i].heapIndex;
                            VkMemoryHeap heap = memProps.memoryHeaps[heapIndex];
                            std::cerr << "    type " << i
                                      << ": allowed=" << (allowed ? "yes" : " no")
                                      << ", flags=0x" << std::hex << memProps.memoryTypes[i].propertyFlags
                                      << ", heap=" << heapIndex
                                      << ", heapFlags=0x" << heap.flags
                                      << ", test=" << (memReq.memoryTypeBits & (1u << i))
                                      << std::dec << "\n";
                        }
                    }
                    vkDestroyBuffer(allocatorInfo.device, tempBuffer, nullptr);
                }

                // Probe if adding TRANSFER_DST opens access to host-visible types
                VkBufferCreateInfo probeInfo = bufferInfo;
                probeInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VkBuffer probeBuffer = VK_NULL_HANDLE;
                if (vkCreateBuffer(allocatorInfo.device, &probeInfo, nullptr, &probeBuffer) == VK_SUCCESS) {
                    VkMemoryRequirements probeReq{};
                    vkGetBufferMemoryRequirements(allocatorInfo.device, probeBuffer, &probeReq);
                    std::cerr << "  probe (SRC|DST) memoryTypeBits=0x" << std::hex << probeReq.memoryTypeBits << std::dec << "\n";
                    vkDestroyBuffer(allocatorInfo.device, probeBuffer, nullptr);
                }
            }
            return false;
        }

        // If mapped flag was set, store the mapped pointer
        if (allocFlags & VMA_ALLOCATION_CREATE_MAPPED_BIT) {
            mappedPtr = allocResult.pMappedData;
            isPersistentlyMapped = true;
        }

        return true;
    }

    // Map memory for CPU access (if not already mapped)
    void* map() {
        if (mappedPtr) return mappedPtr;
        vmaMapMemory(allocator, allocation, &mappedPtr);
        return mappedPtr;
    }

    // Unmap memory (only if not persistently mapped)
    void unmap() {
        // Don't unmap if buffer was created with VMA_ALLOCATION_CREATE_MAPPED_BIT
        // The mapping persists for the lifetime of the buffer
        if (mappedPtr && !isPersistentlyMapped) {
            vmaUnmapMemory(allocator, allocation);
            mappedPtr = nullptr;
        }
    }

    // Upload data to buffer (assumes mapped or performs temp mapping)
    void upload(const void* data, size_t dataSize, size_t offset = 0) {
        if (!mappedPtr) {
            void* tempMap = map();
            memcpy(static_cast<char*>(tempMap) + offset, data, dataSize);
            unmap();
        } else {
            memcpy(static_cast<char*>(mappedPtr) + offset, data, dataSize);
            // Note: For HOST_COHERENT memory, no flush needed
            // For non-coherent, would need vmaFlushAllocation
        }
    }

    // Flush mapped memory (for non-coherent memory)
    void flush(VkDeviceSize flushSize = VK_WHOLE_SIZE, VkDeviceSize flushOffset = 0) {
        vmaFlushAllocation(allocator, allocation, flushOffset, flushSize);
    }

    void destroy() {
        if (buffer != VK_NULL_HANDLE && allocator != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, buffer, allocation);
            buffer = VK_NULL_HANDLE;
            allocation = VK_NULL_HANDLE;
            mappedPtr = nullptr;
        }
    }

    VkBuffer getBuffer() const { return buffer; }
    VkDeviceSize getSize() const { return size; }
    void* getMappedPtr() const { return mappedPtr; }

private:
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    void* mappedPtr = nullptr;
    VkDeviceSize size = 0;
    VkBufferUsageFlags usageFlags = 0;
    bool isPersistentlyMapped = false;
};
