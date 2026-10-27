// Which vertex formats NVN games use are vertex-buffer formats on this GPU (gfx_vk_draw.inc vtx_fmt maps NVN's to these): prints the unsupported ones
#include <stdio.h>
#include <vulkan/vulkan.h>
int main(void) {
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance in; if (vkCreateInstance(&ic, NULL, &in)) return 1;
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(in, &n, &pd);
    int bad = 0;
    for (int f = 9; f <= 124; f++) {   // R8_UNORM .. B10G11R11: the ranges vtx_fmt uses
        if ((f >= 44 && f <= 56) || (f >= 112 && f <= 121)) continue;   // BGR / ABGR orders and 64-bit: not mapped
        VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(pd, (VkFormat)f, &p);
        if (!(p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) { printf("format %d: not a vertex format\n", f); bad++; }
    }
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd, &pr);
    printf("%d unsupported; vertex input bindings %u, attributes %u\n", bad, pr.limits.maxVertexInputBindings, pr.limits.maxVertexInputAttributes);
    return 0;
}
