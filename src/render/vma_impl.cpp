// The single translation unit that compiles VulkanMemoryAllocator (plan 0.1, D13). Function
// pointers come from volk, so VMA must not link the Vulkan loader statically.
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
