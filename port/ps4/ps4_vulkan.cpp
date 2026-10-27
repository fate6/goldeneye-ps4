// PS4 platform for the fast3d Vulkan backend: Mesa RADV (with DolphinPS4's
// GNM winsys) from sce_module/libGE64Radv.prx (tools_ps4/build-radv-prx.sh;
// a separate module keeps the eboot small, see there), and presentation
// through VideoOut, as in the x360
// port's VulkanPresenter (and DolphinPS4's swap chain):
//  - three buffers at the output resolution in host-visible device-local
//    memory at 2 MiB alignment, registered as VideoOut buffers (GPU address =
//    CPU address on the PS4): linear R8G8B8A8 images at 1920x1080, or, when
//    the system outputs 2160p (a PS4 Pro on a 4K TV), 3840x2160 buffers in
//    the display's tiled layout (VideoOut refuses linear ones that large);
//  - each frame goes into the next buffer once it is off screen (flip status
//    wait; a blit, or the backend's tiling compute pass), then queues a GPU-side flip behind the frame's work
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
    bool tiled = false;
    void *addresses[kImages] = {};
    size_t sizes[kImages] = {};
    OrbisKernelEqueue flip_queue = 0;
    VkImage images[kImages] = {};     // linear
    VkBuffer buffers[kImages] = {};   // tiled
    uint32_t micro = 0;               // GE_PS4_TILEMICRO: pixel order in a micro tile
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDeviceWaitIdle wait_idle = nullptr;
    uint64_t flips_submitted = 0;
    uint32_t acquired = 0;
    uint32_t logged = 0;
};
VideoOut out;
GfxVulkanPlatform s_platform;
VkBuffer ps4_acquire_buffer(uint32_t *w, uint32_t *h, uint32_t *micro);

// Create the presentation buffers at w x h and register them with VideoOut.
// VideoOut takes linear buffers up to 1920x1080 only; larger ones must be
// tiled (GE_PS4_4KPROBE, 2026-10-26). Linear: VkImages (the backend blits or
// copies into them). Tiled: VkBuffers the backend writes in the display's
// tiled layout with a compute pass (shaders/gfx_vulkan_tile.comp): the layout
// the display reads was measured, while RADV's own tiling of an image differs
// from it in Neo mode. On failure everything created here is released again.
bool create_buffers(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                    PFN_vkGetDeviceProcAddr gdpa, int video, uint32_t kWidth, uint32_t kHeight, bool tiled) {
    auto gipa = vk_icdGetInstanceProcAddr;
    auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties");
    auto createImage = (PFN_vkCreateImage)gdpa(device, "vkCreateImage");
    auto destroyImage = (PFN_vkDestroyImage)gdpa(device, "vkDestroyImage");
    auto getReqs = (PFN_vkGetImageMemoryRequirements)gdpa(device, "vkGetImageMemoryRequirements");
    auto createBuffer = (PFN_vkCreateBuffer)gdpa(device, "vkCreateBuffer");
    auto destroyBuffer = (PFN_vkDestroyBuffer)gdpa(device, "vkDestroyBuffer");
    auto getBufReqs = (PFN_vkGetBufferMemoryRequirements)gdpa(device, "vkGetBufferMemoryRequirements");
    auto allocate = (PFN_vkAllocateMemory)gdpa(device, "vkAllocateMemory");
    auto freeMemory = (PFN_vkFreeMemory)gdpa(device, "vkFreeMemory");
    auto mapMemory = (PFN_vkMapMemory)gdpa(device, "vkMapMemory");
    auto bindImage = (PFN_vkBindImageMemory)gdpa(device, "vkBindImageMemory");
    auto bindBuffer = (PFN_vkBindBufferMemory)gdpa(device, "vkBindBufferMemory");
    auto getLayout = (PFN_vkGetImageSubresourceLayout)gdpa(device, "vkGetImageSubresourceLayout");
    VkPhysicalDeviceMemoryProperties mp;
    getMemProps(physical, &mp);
    VkDeviceMemory mems[kImages] = {};
    auto release = [&]() {
        for (uint32_t i = 0; i < kImages; i++) {
            if (out.images[i]) destroyImage(device, out.images[i], nullptr);
            if (out.buffers[i]) destroyBuffer(device, out.buffers[i], nullptr);
            if (mems[i]) freeMemory(device, mems[i], nullptr);
            out.images[i] = VK_NULL_HANDLE;
            out.buffers[i] = VK_NULL_HANDLE;
        }
        return false;
    };
    // Tiled: 128x128 macro tiles of 64 KiB, the height padded to whole tiles.
    const VkDeviceSize tiled_size = VkDeviceSize((kWidth + 127) / 128) * ((kHeight + 127) / 128) * 65536;
    void *addresses[kImages] = {};
    uint32_t pitch_pixels = tiled ? (kWidth + 127) / 128 * 128 : kWidth;
    for (uint32_t i = 0; i < kImages; i++) {
        VkMemoryRequirements req;
        if (tiled) {
            VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = tiled_size;
            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (createBuffer(device, &bi, nullptr, &out.buffers[i]) != VK_SUCCESS) {
                sysLogPrintf(LOG_ERROR, "ps4: VideoOut buffer %u: vkCreateBuffer failed", i);
                out.buffers[i] = VK_NULL_HANDLE;
                return release();
            }
            getBufReqs(device, out.buffers[i], &req);
        } else {
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
            getReqs(device, out.images[i], &req);
        }
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
            sysLogPrintf(LOG_ERROR, "ps4: no host-visible device memory for VideoOut buffers");
            return release();
        }
        // Over-allocate to bind at a 2 MiB aligned address.
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size + kAlign;
        ai.memoryTypeIndex = type;
        VkDeviceMemory &mem = mems[i];
        if (allocate(device, &ai, nullptr, &mem) != VK_SUCCESS) {
            sysLogPrintf(LOG_ERROR, "ps4: VideoOut buffer memory allocation failed");
            mem = VK_NULL_HANDLE;
            return release();
        }
        uint8_t *mapping = nullptr;
        if (mapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapping) != VK_SUCCESS) {
            sysLogPrintf(LOG_ERROR, "ps4: VideoOut buffer memory map failed");
            return release();
        }
        VkDeviceSize offset = ((uintptr_t)mapping + kAlign - 1) / kAlign * kAlign - (uintptr_t)mapping;
        offset = (offset + req.alignment - 1) / req.alignment * req.alignment;
        const bool bound = tiled ? bindBuffer(device, out.buffers[i], mem, offset) == VK_SUCCESS
                                 : bindImage(device, out.images[i], mem, offset) == VK_SUCCESS;
        if (!bound) {
            sysLogPrintf(LOG_ERROR, "ps4: VideoOut buffer bind failed");
            return release();
        }
        size_t bytes = (size_t)tiled_size;
        if (!tiled) {
            VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
            VkSubresourceLayout layout;
            getLayout(device, out.images[i], &sub, &layout);
            offset += layout.offset;
            pitch_pixels = (uint32_t)(layout.rowPitch / 4);
            bytes = (size_t)layout.rowPitch * kHeight;
        }
        addresses[i] = mapping + offset;
        memset(addresses[i], 0, bytes);   // black until the first frame
        out.addresses[i] = addresses[i];
        out.sizes[i] = bytes;
        sysLogPrintf(LOG_INFO, "ps4: VideoOut %s buffer %u at %p, %zu bytes", tiled ? "tiled" : "linear", i,
                     addresses[i], bytes);
    }
    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                  tiled ? ORBIS_VIDEO_OUT_TILING_MODE_TILE : ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0,
                                  kWidth, kHeight, pitch_pixels);
    const int reg = sceVideoOutRegisterBuffers(video, 0, addresses, (int32_t)kImages, &attr);
    sysLogPrintf(LOG_INFO, "ps4: VideoOut %d %ux%u %s pitch %u, register buffers = %#x", video, kWidth, kHeight,
                 tiled ? "tiled" : "linear", pitch_pixels, (unsigned)reg);
    if (reg < 0) return release();
    out.width = kWidth;
    out.height = kHeight;
    out.tiled = tiled;
    return true;
}

// GE_PS4_4KPROBE=1 (env.txt): try many 3840x2160 registrations on buffers of
// direct memory (as the OpenOrbis samples allocate them) and log which ones
// VideoOut accepts; each accepted group is unregistered again. Finds what the
// 4K refusal of the Vulkan-memory buffers depends on.
void probe_4k(int video) {
    constexpr size_t kBuf = 72u << 20;   // 4096 px pitch at 8 bytes per pixel, 2 MiB multiple
    constexpr int kWcGarlic = 3, kProt = 0x33;   // CPU read/write, GPU read/write
    void *addrs[3] = {};
    for (int i = 0; i < 3; i++) {
        off_t phys = 0;
        int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), kBuf, 2u << 20, kWcGarlic, &phys);
        if (r == 0) r = sceKernelMapDirectMemory(&addrs[i], kBuf, kProt, 0, phys, 2u << 20);
        sysLogPrintf(LOG_INFO, "ps4: 4k probe buffer %d at %p = %#x", i, addrs[i], (unsigned)r);
        if (r != 0) return;
    }
    struct Fmt { uint32_t code; const char *name; };
    static const Fmt fmts[] = {
        {0x80002200, "A8B8G8R8_SRGB"}, {0x80000000, "A8R8G8B8_SRGB"}, {0x88060000, "A2R10G10B10"},
        {0x88000000, "A2R10G10B10_SRGB"}, {0x88740000, "A2R10G10B10_BT2020_PQ"}, {0xC1060000, "A16R16G16B16_FLOAT"},
    };
    auto attempt = [&](const Fmt &f, uint32_t tiling, uint32_t w, uint32_t h, uint32_t pitch, int count) {
        OrbisVideoOutBufferAttribute attr;
        sceVideoOutSetBufferAttribute(&attr, f.code, tiling, 0, w, h, pitch);
        const int g = sceVideoOutRegisterBuffers(video, 0, addrs, count, &attr);
        sysLogPrintf(LOG_INFO, "ps4: 4k probe %ux%u %s %s pitch %u x%d = %#x", w, h, f.name,
                     tiling ? "linear" : "tiled", pitch, count, (unsigned)g);
        if (g >= 0) sceVideoOutUnregisterBuffers(video, g);
    };
    attempt(fmts[0], ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 1920, 1080, 1920, 3);   // control
    for (const Fmt &f : fmts) {
        for (uint32_t tiling = 0; tiling < 2; tiling++) {   // linear, then tiled
            for (uint32_t pitch = 3840; pitch <= 4096; pitch += 256) {
                for (int count = 3; count > 0; count -= 2) attempt(f, 1 - tiling, 3840, 2160, pitch, count);
            }
        }
    }
    attempt(fmts[0], ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 2560, 1440, 2560, 3);
    attempt(fmts[0], ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 3200, 1800, 3200, 3);
    // Left mapped: a probe build, and the game's own buffers come after.
}

// GE_PS4_TILEPAT=1 (env.txt): show buffer 0 filled with a pattern for 30 s
// before the game starts drawing. Each 256-byte block (an 8x8 micro tile)
// is one solid colour that encodes its block index, 5 bits per channel
// (value n * 8 + 4: R bits 0-4, G 5-9, B 10-14), so a screenshot shows which
// memory block the display reads at each place on screen.
void show_tile_pattern() {
    uint32_t *p = (uint32_t *)out.addresses[0];
    const size_t words = out.sizes[0] / 4;
    for (size_t w = 0; w < words; w++) {
        const uint32_t b = (uint32_t)(w / 64);
        const uint32_t r = (b & 31) * 8 + 4, g = ((b >> 5) & 31) * 8 + 4, bl = ((b >> 10) & 31) * 8 + 4;
        p[w] = r | g << 8 | bl << 16 | 0xFFu << 24;
    }
    const int f = sceVideoOutSubmitFlip(out.video, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
    sysLogPrintf(LOG_INFO, "ps4: tile pattern in buffer 0 (%zu bytes), flip = %#x; holding 30 s", out.sizes[0],
                 (unsigned)f);
    sceKernelUsleep(30u * 1000 * 1000);
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
    // Tiled 4K buffers on a 2160p output (GE_PS4_4K=0: off); linear 1080p ones
    // (scaled up by the system) otherwise or when that fails.
    const char *uhd_env = getenv("GE_PS4_4K");
    const bool uhd = r >= 0 && rs.width >= 3840 && rs.height >= 2160 && !(uhd_env && uhd_env[0] == '0');
    const char *probe = getenv("GE_PS4_4KPROBE");
    if (probe && probe[0] == '1') probe_4k(video);
    if (!(uhd && create_buffers(instance, physical, device, gdpa, video, 3840, 2160, true)) &&
        !create_buffers(instance, physical, device, gdpa, video, 1920, 1080, false)) {
        return false;
    }
    sceKernelCreateEqueue(&out.flip_queue, "ge64 flip");
    sceVideoOutAddFlipEvent(out.flip_queue, video, nullptr);
    out.video = video;
    if (out.tiled) {
        s_platform.acquire_buffer = ps4_acquire_buffer;
        const char *m = getenv("GE_PS4_TILEMICRO");
        out.micro = m ? (uint32_t)atoi(m) : 0;
    }
    gfx_ps4_vsync_paced = 1;
    const char *pat = getenv("GE_PS4_TILEPAT");
    if (pat && pat[0] == '1') show_tile_pattern();
    return true;
}

// The image to draw the next frame into, once it is off screen. With three
// in rotation it was shown by flip n-2 and replaced on screen by flip n-1.
void wait_buffer_free() {
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
}

VkImage ps4_acquire(uint32_t *w, uint32_t *h) {
    if (out.video < 0) return VK_NULL_HANDLE;
    wait_buffer_free();
    *w = out.width;
    *h = out.height;
    return out.images[out.acquired];
}

VkBuffer ps4_acquire_buffer(uint32_t *w, uint32_t *h, uint32_t *micro) {
    if (out.video < 0) return VK_NULL_HANDLE;
    wait_buffer_free();
    *w = out.width;
    *h = out.height;
    *micro = out.micro;
    return out.buffers[out.acquired];
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
