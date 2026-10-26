// PS4 platform for the fast3d Vulkan backend: Mesa RADV (with DolphinPS4's
// GNM winsys) from sce_module/libGE64Radv.prx (tools_ps4/build-radv-prx.sh;
// a separate module keeps the eboot small, see there), and presentation
// through VideoOut, as in the x360
// port's VulkanPresenter (and DolphinPS4's swap chain):
//  - three linear R8G8B8A8 images at the output resolution (3840x2160 when
//    the system outputs 2160p, a PS4 Pro on a 4K TV; else 1920x1080) in
//    host-visible device-local memory at 2 MiB alignment, registered as
//    VideoOut buffers (GPU address = CPU address on the PS4);
//  - each frame blits into the next image once it is off screen (flip status
//    wait), then queues a GPU-side flip behind the frame's work
//    (ac_ps4_submit_flip); a CPU flip after the GPU finishes is the fallback.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include "platform.h"
#include "system.h"

#include "../fast3d/gfx_vulkan.h"

// From the module: the driver's Vulkan entry point, and the winsys' flip
// (DolphinPS4 port/mesa/ac_ps4_drm.c ac_ps4_submit_flip: queue a VideoOut flip
// behind the GPU work submitted so far).
typedef int (*PFN_ge64_submit_flip)(int video, unsigned buffer, unsigned mode, int64_t arg);
// tools_ps4/radv_prx_exports.c: the module's malloc family forwards here.
struct Ge64RadvHeap {
    void *(*malloc)(size_t);
    void (*free)(void *);
    void *(*calloc)(size_t, size_t);
    void *(*realloc)(void *, size_t);
    void *(*memalign)(size_t, size_t);
    char *(*getenv)(const char *);
};
extern "C" void *memalign(size_t alignment, size_t size);
static PFN_vkGetInstanceProcAddr vk_icdGetInstanceProcAddr;
static PFN_ge64_submit_flip ac_ps4_submit_flip;

// gfx_ps4.cpp: VideoOut flips pace the frames once this is set.
extern "C" int gfx_ps4_vsync_paced;

namespace {

constexpr uint32_t kImages = 3;
constexpr VkDeviceSize kAlign = VkDeviceSize(2) << 20;   // VideoOut buffers: 2 MiB aligned

struct VideoOut {
    int video = -1;
    uint32_t width = 1920, height = 1080;
    OrbisKernelEqueue flip_queue = 0;
    VkImage images[kImages] = {};
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDeviceWaitIdle wait_idle = nullptr;
    uint64_t flips_submitted = 0;
    uint32_t acquired = 0;
    uint32_t logged = 0;
};
VideoOut out;

// Create the presentation images at w x h and register them with VideoOut.
// On failure everything created here is released again.
bool create_buffers(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                    PFN_vkGetDeviceProcAddr gdpa, int video, uint32_t kWidth, uint32_t kHeight) {
    auto gipa = vk_icdGetInstanceProcAddr;
    auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties");
    auto createImage = (PFN_vkCreateImage)gdpa(device, "vkCreateImage");
    auto destroyImage = (PFN_vkDestroyImage)gdpa(device, "vkDestroyImage");
    auto getReqs = (PFN_vkGetImageMemoryRequirements)gdpa(device, "vkGetImageMemoryRequirements");
    auto allocate = (PFN_vkAllocateMemory)gdpa(device, "vkAllocateMemory");
    auto freeMemory = (PFN_vkFreeMemory)gdpa(device, "vkFreeMemory");
    auto mapMemory = (PFN_vkMapMemory)gdpa(device, "vkMapMemory");
    auto bindImage = (PFN_vkBindImageMemory)gdpa(device, "vkBindImageMemory");
    auto getLayout = (PFN_vkGetImageSubresourceLayout)gdpa(device, "vkGetImageSubresourceLayout");
    VkPhysicalDeviceMemoryProperties mp;
    getMemProps(physical, &mp);
    VkDeviceMemory mems[kImages] = {};
    auto release = [&]() {
        for (uint32_t i = 0; i < kImages; i++) {
            if (out.images[i]) destroyImage(device, out.images[i], nullptr);
            if (mems[i]) freeMemory(device, mems[i], nullptr);
            out.images[i] = VK_NULL_HANDLE;
        }
        return false;
    };
    void *addresses[kImages] = {};
    uint32_t pitch_pixels = kWidth;
    for (uint32_t i = 0; i < kImages; i++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {kWidth, kHeight, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_LINEAR;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (createImage(device, &ci, nullptr, &out.images[i]) != VK_SUCCESS) {
            sysLogPrintf(LOG_ERROR, "ps4: VideoOut image %u: vkCreateImage failed", i);
            out.images[i] = VK_NULL_HANDLE;
            return release();
        }
        VkMemoryRequirements req;
        getReqs(device, out.images[i], &req);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
            if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) {
                type = t;
                break;
            }
        }
        if (type == UINT32_MAX) {
            sysLogPrintf(LOG_ERROR, "ps4: no host-visible device memory for VideoOut images");
            return release();
        }
        // Over-allocate to bind at a 2 MiB aligned address.
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size + kAlign;
        ai.memoryTypeIndex = type;
        VkDeviceMemory &mem = mems[i];
        if (allocate(device, &ai, nullptr, &mem) != VK_SUCCESS) {
            sysLogPrintf(LOG_ERROR, "ps4: VideoOut image memory allocation failed");
            mem = VK_NULL_HANDLE;
            return release();
        }
        uint8_t *mapping = nullptr;
        if (mapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapping) != VK_SUCCESS) {
            sysLogPrintf(LOG_ERROR, "ps4: VideoOut image memory map failed");
            return release();
        }
        VkDeviceSize offset = ((uintptr_t)mapping + kAlign - 1) / kAlign * kAlign - (uintptr_t)mapping;
        offset = (offset + req.alignment - 1) / req.alignment * req.alignment;
        if (bindImage(device, out.images[i], mem, offset) != VK_SUCCESS) {
            sysLogPrintf(LOG_ERROR, "ps4: VideoOut image bind failed");
            return release();
        }
        VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout;
        getLayout(device, out.images[i], &sub, &layout);
        addresses[i] = mapping + offset + layout.offset;
        memset(addresses[i], 0, (size_t)layout.rowPitch * kHeight);   // black until the first frame
        pitch_pixels = (uint32_t)(layout.rowPitch / 4);
        sysLogPrintf(LOG_INFO, "ps4: VideoOut image %u at %p, row pitch %llu", i, addresses[i],
                     (unsigned long long)layout.rowPitch);
    }
    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,
                                  0, kWidth, kHeight, pitch_pixels);
    const int reg = sceVideoOutRegisterBuffers(video, 0, addresses, (int32_t)kImages, &attr);
    sysLogPrintf(LOG_INFO, "ps4: VideoOut %d %ux%u pitch %u, register buffers = %#x", video, kWidth, kHeight,
                 pitch_pixels, (unsigned)reg);
    if (reg < 0) return release();
    out.width = kWidth;
    out.height = kHeight;
    return true;
}

bool ps4_init(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
              PFN_vkGetDeviceProcAddr gdpa, uint32_t queue_family) {
    (void)queue_family;
    out.device = device;
    out.wait_idle = (PFN_vkDeviceWaitIdle)gdpa(device, "vkDeviceWaitIdle");

    sceUserServiceInitialize(nullptr);   // already initialized is fine
    const int video = sceVideoOutOpen(0xFF, 0, 0, nullptr);
    if (video < 0) {
        sysLogPrintf(LOG_ERROR, "ps4: sceVideoOutOpen failed: %#x", (unsigned)video);
        return false;
    }
    OrbisVideoOutResolutionStatus rs = {};
    const int r = sceVideoOutGetResolutionStatus(video, &rs);
    sysLogPrintf(LOG_INFO, "ps4: video output %ux%u (pane %ux%u, flags %#x) = %#x", rs.width, rs.height,
                 rs.paneWidth, rs.paneHeight, (unsigned)rs.flags, (unsigned)r);
    // 4K buffers on a 2160p output; 1080p ones (scaled up by the system) when
    // that is not possible.
    const bool uhd = r >= 0 && rs.width >= 3840 && rs.height >= 2160;
    if (!(uhd && create_buffers(instance, physical, device, gdpa, video, 3840, 2160)) &&
        !create_buffers(instance, physical, device, gdpa, video, 1920, 1080)) {
        return false;
    }
    sceKernelCreateEqueue(&out.flip_queue, "ge64 flip");
    sceVideoOutAddFlipEvent(out.flip_queue, video, nullptr);
    out.video = video;
    gfx_ps4_vsync_paced = 1;
    return true;
}

// The image to draw the next frame into, once it is off screen. With three
// in rotation it was shown by flip n-2 and replaced on screen by flip n-1.
VkImage ps4_acquire(uint32_t *w, uint32_t *h) {
    if (out.video < 0) return VK_NULL_HANDLE;
    if (out.flips_submitted >= 2) {
        const int64_t needed = (int64_t)(out.flips_submitted - 1);
        for (int tries = 0; tries < 40; tries++) {
            OrbisVideoOutFlipStatus status = {};
            if (sceVideoOutGetFlipStatus(out.video, &status) < 0 || status.flipArg >= needed) break;
            OrbisKernelEvent event;
            int count = 0;
            OrbisKernelUseconds timeout = 5000;
            sceKernelWaitEqueue(out.flip_queue, &event, 1, &count, &timeout);
        }
    }
    out.acquired = (uint32_t)(out.flips_submitted % kImages);
    *w = out.width;
    *h = out.height;
    return out.images[out.acquired];
}

void ps4_present(void) {
    const int64_t arg = (int64_t)(out.flips_submitted + 1);
    int r = ac_ps4_submit_flip(out.video, out.acquired, ORBIS_VIDEO_OUT_FLIP_VSYNC, arg);
    const bool gpu_flip = r == 0;
    if (!gpu_flip) {
        // Fall back to a CPU flip once the frame is drawn.
        if (out.wait_idle) out.wait_idle(out.device);
        r = sceVideoOutSubmitFlip(out.video, (int32_t)out.acquired, ORBIS_VIDEO_OUT_FLIP_VSYNC, arg);
    }
    if (out.logged < 3) {
        out.logged++;
        sysLogPrintf(LOG_INFO, "ps4: present image %u, %s flip %lld = %#x", out.acquired,
                     gpu_flip ? "GPU-side" : "CPU", (long long)arg, (unsigned)r);
    }
    if (r >= 0) out.flips_submitted++;
}

GfxVulkanPlatform s_platform;

void load_driver() {
    const char *path = "/app0/sce_module/libGE64Radv.prx";
    int result = 0;
    const int32_t handle = (int32_t)sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, &result);
    if (handle < 0) {
        sysFatalError("ps4: cannot load %s: %#x", path, (unsigned)handle);
    }
    void *gipa = nullptr, *flip = nullptr, *set_heap = nullptr;
    sceKernelDlsym(handle, "vk_icdGetInstanceProcAddr", &gipa);
    sceKernelDlsym(handle, "ge64_radv_submit_flip", &flip);
    sceKernelDlsym(handle, "ge64_radv_set_heap", &set_heap);
    sysLogPrintf(LOG_INFO, "ps4: %s loaded (handle %d, start %d): gipa %p, flip %p, heap %p", path, handle, result,
                 gipa, flip, set_heap);
    if (!gipa || !flip || !set_heap) {
        sysFatalError("ps4: %s lacks its entry points", path);
    }
    // The module allocates from our heap (the eboot's malloc family is
    // DolphinPS4's runtime heap via --wrap; these addresses are the wrapped ones).
    static const Ge64RadvHeap heap = {::malloc, ::free, ::calloc, ::realloc, ::memalign, ::getenv};
    ((void (*)(const Ge64RadvHeap *))set_heap)(&heap);
    vk_icdGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)gipa;
    ac_ps4_submit_flip = (PFN_ge64_submit_flip)flip;
}

}  // namespace

extern "C" const struct GfxVulkanPlatform *gfx_ps4_vulkan_platform(void) {
    if (!vk_icdGetInstanceProcAddr) load_driver();
    s_platform.name = "PS4 VideoOut";
    s_platform.get_instance_proc_addr = vk_icdGetInstanceProcAddr;
    s_platform.present_image_linear = true;
    s_platform.init = ps4_init;
    s_platform.acquire = ps4_acquire;
    s_platform.present = ps4_present;
    return &s_platform;
}
