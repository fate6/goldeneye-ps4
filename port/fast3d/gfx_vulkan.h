#ifndef GFX_VULKAN_H
#define GFX_VULKAN_H

/*
 * fast3d Vulkan rendering backend (gfx_vulkan.cpp).
 *
 * Needs Vulkan 1.3 (dynamic rendering, extended dynamic state) and
 * VK_KHR_push_descriptor. Framebuffers use OpenGL's coordinate conventions
 * unchanged (memory row 0 = the bottom of the image), so gfx_pc.cpp drives
 * this backend exactly like gfx_opengl.cpp. Presentation is up to the
 * platform: the backend blits the finished screen image (fb 0) into the
 * image the platform hands it, right way up.
 */

#include <stdbool.h>
#include <stdint.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES   /* every entry point comes through get_instance_proc_addr */
#endif
#include <vulkan/vulkan.h>

#include "gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GfxVulkanPlatform {
    const char *name;
    /* Entry point to the Vulkan implementation (the loader's, or a statically
     * linked driver's vk_icdGetInstanceProcAddr). */
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    /* The acquired images are R8G8B8A8 with linear tiling (choose the
     * transfer that format/tiling supports). */
    bool present_image_linear;
    /* Optional: called once the device exists (create presentation images). */
    bool (*init)(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                 PFN_vkGetDeviceProcAddr get_device_proc_addr, uint32_t queue_family);
    /* Optional: the image to show this frame in, or VK_NULL_HANDLE to skip
     * presenting. Blocks until the image is free. It must allow TRANSFER_DST;
     * its contents are left in VK_IMAGE_LAYOUT_GENERAL. */
    VkImage (*acquire)(uint32_t *width, uint32_t *height);
    /* Optional: called after the frame that fills the acquired image has been
     * submitted (queue a flip behind it). */
    void (*present)(void);
    /* Optional, used instead of acquire when set: a buffer to write the frame
     * into in the PS4 VideoOut tiled layout (shaders/gfx_vulkan_tile.comp),
     * for scan-out that takes tiled memory only. Blocks until it is free.
     * *micro picks the pixel order within a micro tile (see the shader). */
    VkBuffer (*acquire_buffer)(uint32_t *width, uint32_t *height, uint32_t *micro);
};

/* Call before gfx_init() with the rendering API set to gfx_vulkan_api. */
void gfx_vulkan_set_platform(const struct GfxVulkanPlatform *platform);

/* Write the last presented screen image (fb 0) as a P6 PPM, top row first
 * (GE_PCDUMP / screenshots). Waits for the GPU. */
bool gfx_vulkan_dump_screen(const char *path);

/* A platform without a window (the Linux test path): Vulkan through the
 * system loader, no presentation; frames are only seen through dumps. */
const struct GfxVulkanPlatform *gfx_vulkan_headless_platform(void);

#ifdef __cplusplus
}
#endif

extern struct GfxRenderingAPI gfx_vulkan_api;

#endif /* GFX_VULKAN_H */
