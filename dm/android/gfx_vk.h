// The Android NVN replay's Vulkan device (gfx_vk.c), shared with the front end that presents the game's window texture (front_android.c).
#pragma once
#include <pthread.h>
#include <stdint.h>
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>

typedef struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties mp;
    int has_bc, has_astc, has_surface, has_swapchain;
    pthread_mutex_t qmu;   // every vkQueueSubmit / vkQueuePresentKHR / vkQueueWaitIdle on q
} vkb_ctx;
vkb_ctx *vkb(void);        // made at first use; NULL when the device has no usable Vulkan

// The window texture the game presented last (NVN QueuePresentTexture), for the front end to draw: always in VK_IMAGE_LAYOUT_GENERAL.
// vkb_frame_lock holds it (the texture is not destroyed meanwhile) until vkb_frame_unlock; 0 = no frame yet
typedef struct { VkImage img; VkFormat fmt; uint32_t w, h; int crop[4]; uint64_t n; int flip; } vkb_frame;   // flip: stored bottom-up (NVN's lower-left window origin), shown flipped
int vkb_frame_lock(vkb_frame *f);
void vkb_pipeline_cache_save(void);   // the driver's compiled pipelines to DM_PIPELINE_CACHE, when they grew (the app paused or stopped)
void vkb_frame_unlock(void);
uint32_t vkb_mem_type(uint32_t bits, VkMemoryPropertyFlags want);   // a memory type index of bits with the wanted properties (UINT32_MAX = none)
