// fast3d Vulkan rendering backend. See gfx_vulkan.h.
//
// Design (mirrors gfx_opengl.cpp's behaviour, not its structure):
//  - One uber-shader pair (shaders/gfx_vulkan.*) evaluates every colour-
//    combiner state from push constants: no per-state shader compiles, so no
//    hitches on a slow CPU (the PS4's Jaguar) and no runtime GLSL compiler.
//  - Vertices go to a per-frame storage buffer; the vertex shader reads them
//    with the per-state float layout gfx_pc.cpp emits.
//  - Pipelines differ only by blend mode, sample count and depth attachment;
//    depth test/write/compare/bias are dynamic state.
//  - Framebuffers keep OpenGL's coordinates (memory row 0 = image bottom), so
//    viewports, scissors, blits and framebuffer textures need no conversion.
//    The final blit to the platform's image flips it right way up.
//  - Texture uploads are recorded into a command buffer submitted ahead of
//    the frame's draws, always into a fresh image (draws recorded earlier in
//    the frame keep the old one until the frame retires).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "platform.h"
#include "system.h"

#include "gfx_cc.h"
#include "gfx_pc.h"
#include "gfx_rendering_api.h"
#include "gfx_vulkan.h"

// VMA: Vulkan Memory Allocator (MIT, vma/LICENSE.txt), entry points loaded at
// run time like ours.
#if defined(PLATFORM_PS4)
extern "C" void *memalign(size_t alignment, size_t size);   // the runtime's heap (not posix_memalign)
#endif
static void *ge_vk_aligned_alloc(size_t size, size_t alignment) {
    void *p = nullptr;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
#if defined(PLATFORM_PS4)
    p = memalign(alignment, size);
#else
    if (posix_memalign(&p, alignment, size) != 0) p = nullptr;
#endif
    return p;
}
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_SYSTEM_ALIGNED_MALLOC(size, alignment) ge_vk_aligned_alloc((size), (alignment))
#define VMA_SYSTEM_ALIGNED_FREE(ptr) free(ptr)
#define VMA_IMPLEMENTATION
#include "vma/vk_mem_alloc.h"

// SPIR-V, generated at build time from shaders/ (AFFINE, FOG_AFFINE variants).
#include "gfx_vulkan_vert_11.h"
#include "gfx_vulkan_frag_11.h"
#include "gfx_vulkan_vert_10.h"
#include "gfx_vulkan_frag_10.h"
#include "gfx_vulkan_vert_00.h"
#include "gfx_vulkan_frag_00.h"

extern int gfx_rdp_affine;   // gfx_pc.cpp (D540); the backend sets them at init
extern int gfx_fog_vertex;   // gfx_pc.cpp (D543)

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

#define GE_VK_INSTANCE_FNS(X)                                                  \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices)                         \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceFormatProperties)     \
    X(vkCreateDevice) X(vkGetDeviceProcAddr)                                   \
    X(vkEnumerateDeviceExtensionProperties)

#define GE_VK_DEVICE_FNS(X)                                                    \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit)                   \
    X(vkCreateCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers)   \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer)                              \
    X(vkCreateFence) X(vkWaitForFences) X(vkResetFences)                       \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler)              \
    X(vkCreateShaderModule) X(vkDestroyShaderModule)                           \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout)                   \
    X(vkCreateGraphicsPipelines)                                               \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline)           \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthTestEnable)          \
    X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp)                      \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetDepthBias) X(vkCmdPushConstants)      \
    X(vkCmdPushDescriptorSetKHR) X(vkCmdDraw) X(vkCmdPipelineBarrier)          \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage)      \
    X(vkCmdCopyImage) X(vkCmdResolveImage) X(vkCmdClearAttachments)            \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage)

#define GE_VK_DECLARE(name) static PFN_##name name;
GE_VK_INSTANCE_FNS(GE_VK_DECLARE)
GE_VK_DEVICE_FNS(GE_VK_DECLARE)
static PFN_vkCreateInstance vkCreateInstance_;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kFrames = 2;
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// Must match shaders/gfx_vulkan_common.glsl.
enum : uint32_t {
    F_TEX0 = 1u << 0, F_TEX1 = 1u << 1, F_CLAMP0S = 1u << 2, F_CLAMP0T = 1u << 3,
    F_CLAMP1S = 1u << 4, F_CLAMP1T = 1u << 5, F_FOG = 1u << 6, F_GRAYSCALE = 1u << 7,
    F_ALPHA = 1u << 8, F_TEXTURE_EDGE = 1u << 9, F_NOISE = 1u << 10, F_2CYC = 1u << 11,
    F_ALPHA_THRESHOLD = 1u << 12, F_INVISIBLE = 1u << 13, F_BLUR = 1u << 14,
    F_THREE_POINT_MODE = 1u << 15, F_THREE_POINT0 = 1u << 16, F_THREE_POINT1 = 1u << 17,
    F_FOGPIXEL = 1u << 20,
};
enum { L_TC0, L_TC1, L_CLAMP0S, L_CLAMP0T, L_CLAMP1S, L_CLAMP1T, L_FOG, L_GRAY, L_INPUTS };

struct GfxVkPush {
    uint32_t vtx_base;
    uint32_t vtx_stride;
    uint32_t layout_lo, layout_hi, layout_in;
    uint32_t flags;
    uint32_t cc[4];
    uint32_t frame_count;
    float noise_scale;
};
static_assert(sizeof(GfxVkPush) == 48, "push constants must match the shaders");

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;          // attachment / full view
    VkImageView sample_view = VK_NULL_HANDLE;   // sampling (framebuffers: alpha = 1, as GL_RGB8)
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, mips = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    uint8_t *mapped = nullptr;
    VkDeviceSize size = 0;
};

struct SamplerKey {
    uint8_t mag_linear, min_linear, mip_mode, use_mips, address_u, address_v, aniso;
    uint32_t pack() const {
        return mag_linear | (min_linear << 1) | (mip_mode << 2) | (use_mips << 3) | (address_u << 4) |
               (address_v << 8) | (uint32_t(aniso) << 12);
    }
};

struct Texture {
    Image img;
    SamplerKey sampler = {1, 1, 0, 0, 0, 0, 1};
};

struct Framebuffer {
    uint32_t width = 0, height = 0, msaa = 1;
    bool has_depth = false, invert_y = false;
    Image color, depth;
    Image resolved;   // single-sample copy of an MSAA colour buffer (blits)
    SamplerKey sampler = {1, 1, 0, 0, 0, 0, 1};
};

struct ShaderProgram_ {
    uint8_t num_inputs;
    bool used_textures[2];
    uint32_t stride;
    uint32_t layout_lo, layout_hi, layout_in;
    uint32_t flags;
    uint32_t cc[4];
};

struct Doomed {
    Image img;
    Buffer buf;
};

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer upload = VK_NULL_HANDLE, draw = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    Buffer vertices, staging;
    VkDeviceSize vertex_used = 0, staging_used = 0;
    std::vector<Doomed> doomed;
};

struct Bound {
    uint32_t texture = 0;
    int fb = -1;   // >= 0: framebuffer colour (select_texture_fb)
};

struct State {
    const GfxVulkanPlatform *platform = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VmaAllocator vma = nullptr;
    VkPhysicalDeviceProperties props = {};
    bool depth_clamp = false, anisotropy = false, linear_blit = false;
    VkSampleCountFlags sample_counts = VK_SAMPLE_COUNT_1_BIT;

    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkPipeline> pipelines;
    std::unordered_map<uint32_t, VkSampler> samplers;

    Frame frames[kFrames];
    uint32_t frame_slot = 0;
    bool recording = false;
    uint64_t frame_count = 0;
    VkCommandPool misc_pool = VK_NULL_HANDLE;
    VkCommandBuffer misc_cmd = VK_NULL_HANDLE;
    VkFence misc_fence = VK_NULL_HANDLE;

    std::map<std::pair<uint64_t, uint32_t>, ShaderProgram_> programs;
    ShaderProgram_ *program = nullptr;

    std::unordered_map<uint32_t, Texture> textures;
    uint32_t next_texture = 1;
    Image white;
    Bound bound[2];
    bool linear_filter[2] = {false, false};
    int active_unit = 0;
    FilteringMode filter_mode = FILTER_LINEAR;
    MipmapFilteringMode mipmap_mode = MIPMAP_LINEAR;
    int anisotropy_level = 4;

    std::vector<Framebuffer> fbs;
    size_t current_fb = 0;
    bool pass_active = false;
    float noise_scale = 1.0f;

    // Current fixed-function state (OpenGL semantics) and what the command
    // buffer has.
    bool depth_test = false, depth_write = false, depth_bias = false;
    VkCompareOp depth_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    float znear = 0.0f, zfar = 1.0f;
    int vp[4] = {0, 0, 1, 1}, sc[4] = {0, 0, 1, 1};
    uint32_t blend = 0;   // 0 off, 1 alpha, 2 modulate
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    bool dyn_dirty = true, desc_dirty = true;
    VkBuffer desc_vertex_buffer = VK_NULL_HANDLE;
    VkImageView desc_views[2] = {};
    VkSampler desc_samplers[2] = {};
};

State g;

// GE_VKDEBUG (bring-up experiments, env.txt on the PS4): 1 = skip draws,
// 2 = skip image clears, 4 = log the first draws and clears.
enum { VKDBG_NODRAW = 1, VKDBG_NOCLEAR = 2, VKDBG_LOG = 4 };
int vk_debug() {
    static int mask = -1;
    if (mask < 0) {
        const char *e = getenv("GE_VKDEBUG");
        mask = e ? atoi(e) : 0;
        if (mask) sysLogPrintf(LOG_WARNING, "vulkan: GE_VKDEBUG=%d", mask);
    }
    return mask;
}
int g_logged_draws = 0;

void vk_check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        sysFatalError("Vulkan: %s failed (%d)", what, (int)r);
    }
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

void barrier(VkCommandBuffer cmd, Image &img, VkImageLayout to) {
    if (img.layout == to) return;
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = img.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    img.layout = to;
}

Image create_image(uint32_t w, uint32_t h, uint32_t mips, VkFormat format, VkSampleCountFlagBits samples,
                   VkImageUsageFlags usage, bool rgb_only_sampling) {
    Image img;
    img.width = w;
    img.height = h;
    img.mips = mips;
    img.samples = samples;
    img.aspect = format == kDepthFormat ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vk_check(vmaCreateImage(g.vma, &ci, &ai, &img.image, &img.alloc, nullptr), "vmaCreateImage");

    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {img.aspect, 0, mips, 0, 1};
    vk_check(vkCreateImageView(g.device, &vi, nullptr, &img.view), "vkCreateImageView");
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        if (rgb_only_sampling) {
            vi.components.a = VK_COMPONENT_SWIZZLE_ONE;
        }
        vk_check(vkCreateImageView(g.device, &vi, nullptr, &img.sample_view), "vkCreateImageView");
    }
    return img;
}

Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool readback = false) {
    Buffer buf;
    buf.size = size;
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
               (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                         : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    VmaAllocationInfo info;
    vk_check(vmaCreateBuffer(g.vma, &ci, &ai, &buf.buffer, &buf.alloc, &info), "vmaCreateBuffer");
    buf.mapped = (uint8_t *)info.pMappedData;
    return buf;
}

void destroy_now(Doomed &d) {
    if (d.img.sample_view) vkDestroyImageView(g.device, d.img.sample_view, nullptr);
    if (d.img.view) vkDestroyImageView(g.device, d.img.view, nullptr);
    if (d.img.image) vmaDestroyImage(g.vma, d.img.image, d.img.alloc);
    if (d.buf.buffer) vmaDestroyBuffer(g.vma, d.buf.buffer, d.buf.alloc);
}

// Destroyed once the frame being recorded (which may still use it) retires.
void doom(Image &img) {
    if (img.image) {
        Doomed d;
        d.img = img;
        g.frames[g.frame_slot].doomed.push_back(d);
    }
    img = Image();
}

void doom(Buffer &buf) {
    if (buf.buffer) {
        Doomed d;
        d.buf = buf;
        g.frames[g.frame_slot].doomed.push_back(d);
    }
    buf = Buffer();
}

VkSampler get_sampler(const SamplerKey &k, uint32_t max_mip) {
    const uint32_t key = k.pack() ^ (max_mip << 24);
    auto it = g.samplers.find(key);
    if (it != g.samplers.end()) return it->second;
    static const VkSamplerAddressMode modes[4] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,                 // G_TX_NOMIRROR | G_TX_WRAP
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,        // G_TX_MIRROR | G_TX_WRAP
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,          // G_TX_NOMIRROR | G_TX_CLAMP
        VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,   // G_TX_MIRROR | G_TX_CLAMP
    };
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = k.mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = k.min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = k.mip_mode ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = modes[k.address_u & 3];
    ci.addressModeV = modes[k.address_v & 3];
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.anisotropyEnable = g.anisotropy && k.use_mips && k.aniso > 1;
    ci.maxAnisotropy = ci.anisotropyEnable ? std::min((float)k.aniso, g.props.limits.maxSamplerAnisotropy) : 1.0f;
    ci.minLod = 0.0f;
    ci.maxLod = k.use_mips ? (float)max_mip : 0.0f;
    VkSampler s;
    vk_check(vkCreateSampler(g.device, &ci, nullptr, &s), "vkCreateSampler");
    g.samplers[key] = s;
    return s;
}

// ---------------------------------------------------------------------------
// Frames and command recording
// ---------------------------------------------------------------------------

void begin_recording() {
    if (g.recording) return;
    Frame &f = g.frames[g.frame_slot];
    if (f.submitted) {
        vk_check(vkWaitForFences(g.device, 1, &f.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        f.submitted = false;
    }
    vkResetFences(g.device, 1, &f.fence);
    for (Doomed &d : f.doomed) destroy_now(d);
    f.doomed.clear();
    vkResetCommandPool(g.device, f.pool, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.upload, &bi);
    vkBeginCommandBuffer(f.draw, &bi);
    f.vertex_used = 0;
    f.staging_used = 0;
    g.recording = true;
    g.bound_pipeline = VK_NULL_HANDLE;
    g.dyn_dirty = true;
    g.desc_dirty = true;
}

Frame &frame() {
    begin_recording();
    return g.frames[g.frame_slot];
}

// Room for `size` bytes in a per-frame host buffer, growing it when full.
VkDeviceSize ring_alloc(Buffer &buf, VkDeviceSize &used, VkDeviceSize size, VkDeviceSize align,
                        VkBufferUsageFlags usage) {
    VkDeviceSize off = (used + align - 1) & ~(align - 1);
    if (!buf.buffer || off + size > buf.size) {
        VkDeviceSize grow = std::max<VkDeviceSize>(buf.size ? buf.size * 2 : (8u << 20), size * 2);
        doom(buf);   // still referenced by commands recorded earlier this frame
        buf = create_buffer(grow, usage);
        off = 0;
        if (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) g.desc_dirty = true;
    }
    used = off + size;
    return off;
}

Framebuffer &cur_fb() {
    return g.fbs[g.current_fb];
}

void end_pass() {
    if (!g.pass_active) return;
    vkCmdEndRendering(frame().draw);
    g.pass_active = false;
}

void begin_pass() {
    if (g.pass_active) return;
    VkCommandBuffer cmd = frame().draw;
    Framebuffer &fb = cur_fb();
    barrier(cmd, fb.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = fb.color.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (fb.has_depth) {
        barrier(cmd, fb.depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        depth.imageView = fb.depth.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {fb.width, fb.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = fb.has_depth ? &depth : nullptr;
    vkCmdBeginRendering(cmd, &ri);
    g.pass_active = true;
    g.bound_pipeline = VK_NULL_HANDLE;
    g.dyn_dirty = true;
}

VkPipeline get_pipeline(uint32_t blend, VkSampleCountFlagBits samples, bool has_depth) {
    const uint32_t key = blend | ((uint32_t)samples << 4) | (has_depth ? 1u << 12 : 0);
    auto it = g.pipelines.find(key);
    if (it != g.pipelines.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g.vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g.fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = g.depth_clamp;   // as GL_DEPTH_CLAMP in gfx_opengl.cpp
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;       // gfx_pc culls on the CPU
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att = {};
    att.colorWriteMask = 0xf;
    if (blend == 1) {   // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att.dstColorBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    } else if (blend == 2) {   // glBlendFunc(GL_DST_COLOR, GL_ZERO)
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        att.dstColorBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,  VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof(dyn) / sizeof(dyn[0]);
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rinfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat color_format = kColorFormat;
    rinfo.colorAttachmentCount = 1;
    rinfo.pColorAttachmentFormats = &color_format;
    rinfo.depthAttachmentFormat = has_depth ? kDepthFormat : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rinfo;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vin;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = g.pipeline_layout;
    VkPipeline p;
    vk_check(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines");
    sysLogPrintf(LOG_INFO, "vulkan: pipeline blend %u, %u samples, depth %d", blend, (unsigned)samples,
                 has_depth ? 1 : 0);
    g.pipelines[key] = p;
    return p;
}

// ---------------------------------------------------------------------------
// Device setup
// ---------------------------------------------------------------------------

bool has_device_extension(const char *name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(g.physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(g.physical, nullptr, &n, exts.data());
    for (auto &e : exts) {
        if (!strcmp(e.extensionName, name)) return true;
    }
    return false;
}

VkShaderModule make_module(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m;
    vk_check(vkCreateShaderModule(g.device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

void upload_white() {
    g.white = create_image(1, 1, 1, kColorFormat, VK_SAMPLE_COUNT_1_BIT,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false);
    VkCommandBuffer cmd = frame().upload;
    barrier(cmd, g.white, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue white = {{1.0f, 1.0f, 1.0f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, g.white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
    barrier(cmd, g.white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void device_init() {
    if (!g.platform) {
        sysFatalError("Vulkan: no platform set (gfx_vulkan_set_platform)");
    }
    PFN_vkGetInstanceProcAddr gipa = g.platform->get_instance_proc_addr;
    vkCreateInstance_ = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!vkCreateInstance_) sysFatalError("Vulkan: no vkCreateInstance");

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "GoldenEye 007";
    app.pEngineName = "fast3d";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    const char *validation = "VK_LAYER_KHRONOS_validation";
    if (getenv("GE_VKVALIDATE")) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &validation;
    }
    vk_check(vkCreateInstance_(&ici, nullptr, &g.instance), "vkCreateInstance");
#define GE_VK_LOAD_I(name) name = (PFN_##name)gipa(g.instance, #name);
    GE_VK_INSTANCE_FNS(GE_VK_LOAD_I)

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(g.instance, &n, devices.data());
    if (!n) sysFatalError("Vulkan: no devices");
    const char *pick = getenv("GE_VKDEVICE");
    g.physical = devices[pick ? std::min<uint32_t>(atoi(pick), n - 1) : 0];
    vkGetPhysicalDeviceProperties(g.physical, &g.props);
    sysLogPrintf(LOG_INFO, "vulkan: %s (API %u.%u.%u, %s platform)", g.props.deviceName,
                 VK_API_VERSION_MAJOR(g.props.apiVersion), VK_API_VERSION_MINOR(g.props.apiVersion),
                 VK_API_VERSION_PATCH(g.props.apiVersion), g.platform->name);
    if (g.props.apiVersion < VK_API_VERSION_1_3) sysFatalError("Vulkan: device is not Vulkan 1.3");
    if (!has_device_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        sysFatalError("Vulkan: VK_KHR_push_descriptor is missing");
    }

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &qn, qf.data());
    g.queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g.queue_family = i;
            break;
        }
    }
    if (g.queue_family == UINT32_MAX) sysFatalError("Vulkan: no graphics queue");

    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(g.physical, &f2);
    if (!f13.dynamicRendering) sysFatalError("Vulkan: dynamicRendering unsupported");
    if (!f12.samplerMirrorClampToEdge) {
        sysLogPrintf(LOG_WARNING, "vulkan: no samplerMirrorClampToEdge; mirror-clamp uses mirrored repeat");
    }
    g.depth_clamp = f2.features.depthClamp;
    g.anisotropy = f2.features.samplerAnisotropy;

    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.pNext = &e13;
    e12.samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge;
    VkPhysicalDeviceFeatures2 e2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    e2.pNext = &e12;
    e2.features.depthClamp = g.depth_clamp;
    e2.features.samplerAnisotropy = g.anisotropy;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *exts[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &e2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    vk_check(vkCreateDevice(g.physical, &dci, nullptr, &g.device), "vkCreateDevice");
#define GE_VK_LOAD_D(name) name = (PFN_##name)vkGetDeviceProcAddr(g.device, #name);
    GE_VK_DEVICE_FNS(GE_VK_LOAD_D)
    vkGetDeviceQueue(g.device, g.queue_family, 0, &g.queue);

    VmaVulkanFunctions vf = {};
    vf.vkGetInstanceProcAddr = gipa;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci = {};
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.physicalDevice = g.physical;
    aci.device = g.device;
    aci.instance = g.instance;
    aci.pVulkanFunctions = &vf;
    vk_check(vmaCreateAllocator(&aci, &g.vma), "vmaCreateAllocator");

    g.sample_counts = g.props.limits.framebufferColorSampleCounts & g.props.limits.framebufferDepthSampleCounts;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.physical, kColorFormat, &fp);
    g.linear_blit = (fp.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;

    // Shader variant: the interpolation qualifiers are compile-time (D540/D543).
    const char *sp = getenv("GE_SHADEPERSP"), *fpx = getenv("GE_FOGPIXEL");
    gfx_rdp_affine = !(sp && sp[0] == '1');
    gfx_fog_vertex = !(fpx && fpx[0] == '1');
    if (gfx_rdp_affine && gfx_fog_vertex) {
        g.vs = make_module(gfx_vulkan_vert_11, sizeof(gfx_vulkan_vert_11));
        g.fs = make_module(gfx_vulkan_frag_11, sizeof(gfx_vulkan_frag_11));
    } else if (gfx_rdp_affine) {
        g.vs = make_module(gfx_vulkan_vert_10, sizeof(gfx_vulkan_vert_10));
        g.fs = make_module(gfx_vulkan_frag_10, sizeof(gfx_vulkan_frag_10));
    } else {
        g.vs = make_module(gfx_vulkan_vert_00, sizeof(gfx_vulkan_vert_00));
        g.fs = make_module(gfx_vulkan_frag_00, sizeof(gfx_vulkan_frag_00));
    }
    sysLogPrintf(LOG_NOTE, "vulkan: shade %s, fog %s", gfx_rdp_affine ? "affine (RDP)" : "perspective",
                 gfx_fog_vertex ? "per-vertex" : "per-pixel");

    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 3;
    dl.pBindings = bindings;
    vk_check(vkCreateDescriptorSetLayout(g.device, &dl, nullptr, &g.set_layout), "vkCreateDescriptorSetLayout");
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GfxVkPush)};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    vk_check(vkCreatePipelineLayout(g.device, &pl, nullptr, &g.pipeline_layout), "vkCreatePipelineLayout");

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = g.queue_family;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (Frame &f : g.frames) {
        vk_check(vkCreateCommandPool(g.device, &cpi, nullptr, &f.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 2;
        VkCommandBuffer cmds[2];
        vk_check(vkAllocateCommandBuffers(g.device, &cai, cmds), "vkAllocateCommandBuffers");
        f.upload = cmds[0];
        f.draw = cmds[1];
        vk_check(vkCreateFence(g.device, &fci, nullptr, &f.fence), "vkCreateFence");
    }
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vk_check(vkCreateCommandPool(g.device, &cpi, nullptr, &g.misc_pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.misc_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vk_check(vkAllocateCommandBuffers(g.device, &cai, &g.misc_cmd), "vkAllocateCommandBuffers");
    vk_check(vkCreateFence(g.device, &fci, nullptr, &g.misc_fence), "vkCreateFence");

    if (g.platform->init &&
        !g.platform->init(g.instance, g.physical, g.device, vkGetDeviceProcAddr, g.queue_family)) {
        sysFatalError("Vulkan: platform presentation setup failed");
    }

    g.fbs.resize(1);   // fb 0: the screen
    upload_white();
}

// ---------------------------------------------------------------------------
// Framebuffers
// ---------------------------------------------------------------------------

void clear_now(Framebuffer &fb, bool color, bool depth) {
    if (vk_debug() & VKDBG_NOCLEAR) return;
    if ((vk_debug() & VKDBG_LOG) && g_logged_draws < 40) {
        g_logged_draws++;
        sysLogPrintf(LOG_INFO, "vkdbg: clear %ux%u x%u color %d depth %d", fb.width, fb.height, fb.msaa, color, depth);
    }
    VkCommandBuffer cmd = frame().draw;
    if (color && fb.color.image) {
        barrier(cmd, fb.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, fb.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }
    if (depth && fb.has_depth && fb.depth.image) {
        barrier(cmd, fb.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue one = {1.0f, 0};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, fb.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
    }
}

// The single-sample image to read framebuffer `fb`'s colour from.
Image &readable_color(int fb_id) {
    Framebuffer &fb = g.fbs[fb_id];
    if (fb.color.samples == VK_SAMPLE_COUNT_1_BIT) return fb.color;
    VkCommandBuffer cmd = frame().draw;
    if (!fb.resolved.image || fb.resolved.width != fb.width || fb.resolved.height != fb.height) {
        doom(fb.resolved);
        fb.resolved = create_image(fb.width, fb.height, 1, kColorFormat, VK_SAMPLE_COUNT_1_BIT,
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT,
                                   true);
    }
    barrier(cmd, fb.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cmd, fb.resolved, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageResolve r = {};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {fb.width, fb.height, 1};
    vkCmdResolveImage(cmd, fb.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fb.resolved.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    return fb.resolved;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

struct TexBinding {
    VkImageView view;
    VkSampler sampler;
};

TexBinding texture_binding(int unit) {
    const Bound &b = g.bound[unit];
    if (b.fb >= 0 && b.fb < (int)g.fbs.size() && g.fbs[b.fb].color.image) {
        Image &img = readable_color(b.fb);
        barrier(frame().draw, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return {img.sample_view, get_sampler(g.fbs[b.fb].sampler, 0)};
    }
    auto it = g.textures.find(b.texture);
    if (it == g.textures.end() || !it->second.img.image) {
        return {g.white.sample_view, get_sampler(SamplerKey{0, 0, 0, 0, 0, 0, 1}, 0)};
    }
    return {it->second.img.sample_view, get_sampler(it->second.sampler, it->second.img.mips - 1)};
}

void draw(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    if (!g.program || !num_tris || !cur_fb().color.image) return;
    if (vk_debug() & VKDBG_NODRAW) return;
    if ((vk_debug() & VKDBG_LOG) && g_logged_draws < 40) {
        g_logged_draws++;
        sysLogPrintf(LOG_INFO, "vkdbg: draw %zu tris, %zu floats, stride %u, flags %#x, fb %zu %ux%u x%u, blend %u",
                     num_tris, buf_vbo_len, g.program->stride, g.program->flags, g.current_fb, cur_fb().width,
                     cur_fb().height, cur_fb().msaa, g.blend);
    }
    Frame &f = frame();
    VkCommandBuffer cmd = f.draw;
    ShaderProgram_ &prg = *g.program;

    // Textures first: binding a framebuffer may need commands outside the pass.
    TexBinding tex[2];
    bool needs_outside = false;
    for (int t = 0; t < 2; t++) {
        const Bound &b = g.bound[t];
        if (prg.used_textures[t] && b.fb >= 0 && b.fb < (int)g.fbs.size()) {
            needs_outside = true;
        }
    }
    if (needs_outside) end_pass();
    for (int t = 0; t < 2; t++) {
        tex[t] = prg.used_textures[t] ? texture_binding(t) : TexBinding{g.white.sample_view,
                                                                      get_sampler(SamplerKey{0, 0, 0, 0, 0, 0, 1}, 0)};
    }
    begin_pass();

    const VkDeviceSize bytes = buf_vbo_len * sizeof(float);
    const VkDeviceSize off = ring_alloc(f.vertices, f.vertex_used, bytes, 16,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(f.vertices.mapped + off, buf_vbo, bytes);

    Framebuffer &fb = cur_fb();
    VkPipeline pipe = get_pipeline(g.blend, fb.color.samples, fb.has_depth);
    if (pipe != g.bound_pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        g.bound_pipeline = pipe;
        g.dyn_dirty = true;
        g.desc_dirty = true;
    }
    if (g.dyn_dirty) {
        VkViewport vp = {(float)g.vp[0], (float)g.vp[1], (float)std::max(g.vp[2], 1),
                         (float)std::max(g.vp[3], 1), g.znear, g.zfar};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        int x = std::max(g.sc[0], 0), y = std::max(g.sc[1], 0);
        int x1 = std::min(g.sc[0] + g.sc[2], (int)fb.width), y1 = std::min(g.sc[1] + g.sc[3], (int)fb.height);
        VkRect2D sc = {{x, y}, {(uint32_t)std::max(x1 - x, 0), (uint32_t)std::max(y1 - y, 0)}};
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdSetDepthTestEnable(cmd, g.depth_test);
        vkCmdSetDepthWriteEnable(cmd, g.depth_test && g.depth_write);
        vkCmdSetDepthCompareOp(cmd, g.depth_op);
        vkCmdSetDepthBiasEnable(cmd, g.depth_bias);
        vkCmdSetDepthBias(cmd, g.depth_bias ? -2.0f : 0.0f, 0.0f, g.depth_bias ? -2.0f : 0.0f);
        g.dyn_dirty = false;
    }
    if (g.desc_dirty || g.desc_vertex_buffer != f.vertices.buffer || tex[0].view != g.desc_views[0] ||
        tex[1].view != g.desc_views[1] || tex[0].sampler != g.desc_samplers[0] ||
        tex[1].sampler != g.desc_samplers[1]) {
        VkDescriptorBufferInfo bi = {f.vertices.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo ii[2] = {
            {tex[0].sampler, tex[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {tex[1].sampler, tex[1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkWriteDescriptorSet w[3] = {};
        for (int i = 0; i < 3; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1].descriptorType = w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &ii[0];
        w[2].pImageInfo = &ii[1];
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline_layout, 0, 3, w);
        g.desc_vertex_buffer = f.vertices.buffer;
        g.desc_views[0] = tex[0].view;
        g.desc_views[1] = tex[1].view;
        g.desc_samplers[0] = tex[0].sampler;
        g.desc_samplers[1] = tex[1].sampler;
        g.desc_dirty = false;
    }

    GfxVkPush pc;
    pc.vtx_base = (uint32_t)(off / sizeof(float));
    pc.vtx_stride = prg.stride;
    pc.layout_lo = prg.layout_lo;
    pc.layout_hi = prg.layout_hi;
    pc.layout_in = prg.layout_in;
    pc.flags = prg.flags;
    if (g.filter_mode == FILTER_THREE_POINT) {
        pc.flags |= F_THREE_POINT_MODE;
        if (g.linear_filter[0]) pc.flags |= F_THREE_POINT0;
        if (g.linear_filter[1]) pc.flags |= F_THREE_POINT1;
    }
    if (!gfx_fog_vertex) pc.flags |= F_FOGPIXEL;
    memcpy(pc.cc, prg.cc, sizeof(pc.cc));
    pc.frame_count = (uint32_t)g.frame_count;
    pc.noise_scale = g.noise_scale;
    vkCmdPushConstants(cmd, g.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(pc), &pc);
    vkCmdDraw(cmd, (uint32_t)(3 * num_tris), 1, 0, 0);
}

}  // namespace

// ---------------------------------------------------------------------------
// GfxRenderingAPI
// ---------------------------------------------------------------------------

static const char *vk_get_name(void) {
    return "Vulkan";
}

static int vk_get_max_texture_size(void) {
    return (int)g.props.limits.maxImageDimension2D;
}

static GfxClipParameters vk_get_clip_parameters(void) {
    // Depth 0..1; y as OpenGL (see the file comment), incl. its per-fb inversion.
    return {true, g.fbs[g.current_fb].invert_y};
}

static void vk_unload_shader(ShaderProgram *) {
}

static void vk_load_shader(ShaderProgram *prg) {
    g.program = (ShaderProgram_ *)prg;
}

static ShaderProgram *vk_create_and_load_new_shader(uint64_t id0, uint32_t id1) {
    CCFeatures cc = {};
    gfx_cc_get_features(id0, id1, &cc);
    ShaderProgram_ &p = g.programs[{id0, id1}];
    p = ShaderProgram_();
    p.num_inputs = (uint8_t)cc.num_inputs;
    p.used_textures[0] = cc.used_textures[0];
    p.used_textures[1] = cc.used_textures[1];
    if (cc.num_inputs > 4) {
        sysLogPrintf(LOG_ERROR, "vulkan: combiner %llx/%x uses %d inputs (max 4)", (unsigned long long)id0, id1,
                     cc.num_inputs);
    }

    // The float layout gfx_pc.cpp emits for this state (cf. gfx_opengl.cpp).
    uint8_t offs[9] = {};
    uint32_t pos = 4;
    uint32_t flags = 0;
    for (int t = 0; t < 2; t++) {
        if (!cc.used_textures[t]) continue;
        flags |= t == 0 ? F_TEX0 : F_TEX1;
        offs[t == 0 ? L_TC0 : L_TC1] = (uint8_t)pos;
        pos += 2;
        if (cc.clamp[t][0]) {
            flags |= t == 0 ? F_CLAMP0S : F_CLAMP1S;
            offs[t == 0 ? L_CLAMP0S : L_CLAMP1S] = (uint8_t)pos++;
        }
        if (cc.clamp[t][1]) {
            flags |= t == 0 ? F_CLAMP0T : F_CLAMP1T;
            offs[t == 0 ? L_CLAMP0T : L_CLAMP1T] = (uint8_t)pos++;
        }
    }
    if (cc.opt_fog) {
        flags |= F_FOG;
        offs[L_FOG] = (uint8_t)pos;
        pos += 4;
    }
    if (cc.opt_grayscale) {
        flags |= F_GRAYSCALE;
        offs[L_GRAY] = (uint8_t)pos;
        pos += 4;
    }
    offs[L_INPUTS] = (uint8_t)pos;
    pos += (uint32_t)cc.num_inputs * (cc.opt_alpha ? 4 : 3);
    p.stride = pos;
    p.layout_lo = offs[0] | (offs[1] << 8) | (offs[2] << 16) | ((uint32_t)offs[3] << 24);
    p.layout_hi = offs[4] | (offs[5] << 8) | (offs[6] << 16) | ((uint32_t)offs[7] << 24);
    p.layout_in = offs[8];

    if (cc.opt_alpha) flags |= F_ALPHA;
    if (cc.opt_texture_edge) flags |= F_TEXTURE_EDGE;
    if (cc.opt_noise) flags |= F_NOISE;
    if (cc.opt_2cyc) flags |= F_2CYC;
    if (cc.opt_alpha_threshold) flags |= F_ALPHA_THRESHOLD;
    if (cc.opt_invisible) flags |= F_INVISIBLE;
    if (cc.opt_blur) flags |= F_BLUR;
    flags |= (uint32_t)std::min(cc.num_inputs, 4) << 24;
    p.flags = flags;
    p.cc[0] = (uint32_t)id0;
    p.cc[1] = (uint32_t)(id0 >> 32);
    p.cc[2] = p.cc[3] = 0;
    g.program = &p;
    return (ShaderProgram *)&p;
}

static ShaderProgram *vk_lookup_shader(uint64_t id0, uint32_t id1) {
    auto it = g.programs.find({id0, id1});
    return it == g.programs.end() ? nullptr : (ShaderProgram *)&it->second;
}

static void vk_shader_get_info(ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    ShaderProgram_ *p = (ShaderProgram_ *)prg;
    *num_inputs = p->num_inputs;
    used_textures[0] = p->used_textures[0];
    used_textures[1] = p->used_textures[1];
}

static void vk_clear_shaders(void) {
    g.programs.clear();
    g.program = nullptr;
}

static uint32_t vk_new_texture(void) {
    const uint32_t id = g.next_texture++;
    g.textures[id];
    return id;
}

static void vk_select_texture(int tile, uint32_t id, bool linear_filter) {
    g.bound[tile] = {id, -1};
    g.linear_filter[tile] = linear_filter;
    g.active_unit = tile;
}

static void vk_upload_texture(const uint8_t *rgba, uint32_t w, uint32_t h, bool gen_mipmaps) {
    if (!w || !h) return;
    const Bound &b = g.bound[g.active_unit];
    if (b.fb >= 0) return;
    Texture &tex = g.textures[b.texture];
    // As gfx_opengl.cpp: 3-point and trilinear always sample mips.
    const bool mips = gen_mipmaps || g.filter_mode == FILTER_THREE_POINT || g.filter_mode == FILTER_TRILINEAR;
    uint32_t levels = 1;
    if (mips) {
        for (uint32_t m = std::max(w, h); m > 1; m >>= 1) levels++;
    }
    doom(tex.img);   // draws recorded earlier this frame keep the old image
    tex.img = create_image(w, h, levels, kColorFormat, VK_SAMPLE_COUNT_1_BIT,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           false);
    Frame &f = frame();
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    const VkDeviceSize off = ring_alloc(f.staging, f.staging_used, bytes, 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(f.staging.mapped + off, rgba, bytes);
    VkCommandBuffer cmd = f.upload;
    barrier(cmd, tex.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy = {};
    copy.bufferOffset = off;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, f.staging.buffer, tex.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    // Mip chain by successive linear blits (glGenerateMipmap).
    int32_t mw = (int32_t)w, mh = (int32_t)h;
    for (uint32_t level = 1; level < levels; level++) {
        VkImageMemoryBarrier mb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image = tex.img.image;
        mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &mb);
        const int32_t nw = std::max(mw / 2, 1), nh = std::max(mh / 2, 1);
        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {mw, mh, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cmd, tex.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex.img.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        mw = nw;
        mh = nh;
    }
    if (levels > 1) {
        // Levels 0..n-2 are TRANSFER_SRC, n-1 TRANSFER_DST: make all one layout.
        VkImageMemoryBarrier mb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image = tex.img.image;
        mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels - 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &mb);
    }
    barrier(cmd, tex.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g.desc_dirty = true;
}

static void vk_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps) {
    g.active_unit = tile;
    // gfx_opengl.cpp's min/mag filter table, as Vulkan filter + mipmap mode.
    if (g.filter_mode == FILTER_THREE_POINT || g.filter_mode == FILTER_TRILINEAR) mipmaps = true;
    mipmaps = mipmaps && g.mipmap_mode != MIPMAP_DISABLED;
    SamplerKey k = {};
    k.use_mips = mipmaps;
    k.mip_mode = g.mipmap_mode == MIPMAP_LINEAR || g.filter_mode == FILTER_TRILINEAR;
    if (!linear_filter) {
        k.min_linear = 0;
        k.use_mips = 0;   // GL_NEAREST: no mip sampling
    } else if (g.filter_mode == FILTER_NONE) {
        k.min_linear = 0;
    } else if (g.filter_mode == FILTER_THREE_POINT) {
        k.min_linear = mipmaps ? 1 : 0;
    } else {
        k.min_linear = 1;
    }
    k.mag_linear = linear_filter && (g.filter_mode == FILTER_LINEAR || g.filter_mode == FILTER_TRILINEAR);
    // gbi.h: G_TX_MIRROR = 1, G_TX_CLAMP = 2 -> index into get_sampler()'s modes
    k.address_u = (uint8_t)(cms & 3);
    k.address_v = (uint8_t)(cmt & 3);
    k.aniso = (uint8_t)std::max(1, std::min(g.anisotropy_level, 16));
    const Bound &b = g.bound[tile];
    if (b.fb >= 0 && b.fb < (int)g.fbs.size()) {
        g.fbs[b.fb].sampler = k;
    } else {
        g.textures[b.texture].sampler = k;
    }
    g.desc_dirty = true;
}

static void vk_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim,
                              uint16_t zmode) {
    g.depth_test = depth_test;
    g.depth_write = depth_update;
    g.depth_bias = false;
    if (!depth_compare) {
        g.depth_op = VK_COMPARE_OP_ALWAYS;
    } else if (zmode == ZMODE_OPA || zmode == ZMODE_XLU) {
        g.depth_op = depth_source_prim ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
    } else {
        g.depth_op = VK_COMPARE_OP_LESS_OR_EQUAL;   // ZMODE_INTER, ZMODE_DEC
        g.depth_bias = zmode == ZMODE_DEC;           // glPolygonOffset(-2, -2)
    }
    g.dyn_dirty = true;
}

static void vk_set_depth_range(float znear, float zfar) {
    g.znear = znear;
    g.zfar = zfar;
    g.dyn_dirty = true;
}

static void vk_set_viewport(int x, int y, int w, int h) {
    g.vp[0] = x;
    g.vp[1] = y;
    g.vp[2] = w;
    g.vp[3] = h;
    g.dyn_dirty = true;
}

static void vk_set_scissor(int x, int y, int w, int h) {
    g.sc[0] = x;
    g.sc[1] = y;
    g.sc[2] = w;
    g.sc[3] = h;
    g.dyn_dirty = true;
}

static void vk_set_use_alpha(bool use_alpha, bool modulate) {
    g.blend = !use_alpha ? 0 : (modulate ? 2 : 1);
}

static void vk_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    draw(buf_vbo, buf_vbo_len, num_tris);
}

static void vk_init(void) {
    device_init();
}

static void vk_on_resize(void) {
}

static void vk_start_frame(void) {
    g.frame_count++;
    begin_recording();
}

static void vk_end_frame(void) {
    if (!g.recording) return;
    end_pass();
    Frame &f = g.frames[g.frame_slot];
    Framebuffer &screen = g.fbs[0];
    uint32_t pw = 0, ph = 0;
    VkImage present = g.platform->acquire && screen.color.image ? g.platform->acquire(&pw, &ph) : VK_NULL_HANDLE;
    if (present) {
        Image &src = readable_color(0);
        barrier(f.draw, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Image dst;
        dst.image = present;
        dst.width = pw;
        dst.height = ph;
        barrier(f.draw, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);   // from UNDEFINED: contents replaced
        const bool same_size = pw == src.width && ph == src.height;
        if (!g.platform->present_image_linear || g.linear_blit) {
            // Flip: the screen image is bottom row first (OpenGL), the display top first.
            VkImageBlit blit = {};
            blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {0, (int32_t)src.height, 0};
            blit.srcOffsets[1] = {(int32_t)src.width, 0, 1};
            blit.dstOffsets[1] = {(int32_t)pw, (int32_t)ph, 1};
            vkCmdBlitImage(f.draw, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, present,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           same_size ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        } else if (same_size) {
            // No blits into linear images: one copy region per row, flipped.
            std::vector<VkImageCopy> rows(ph);
            for (uint32_t y = 0; y < ph; y++) {
                VkImageCopy &c = rows[y];
                c = {};
                c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.srcOffset = {0, (int32_t)(ph - 1 - y), 0};
                c.dstOffset = {0, (int32_t)y, 0};
                c.extent = {pw, 1, 1};
            }
            vkCmdCopyImage(f.draw, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, present,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ph, rows.data());
        }
        barrier(f.draw, dst, VK_IMAGE_LAYOUT_GENERAL);
    }
    vkEndCommandBuffer(f.upload);
    vkEndCommandBuffer(f.draw);
    VkCommandBuffer cmds[2] = {f.upload, f.draw};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 2;
    si.pCommandBuffers = cmds;
    vk_check(vkQueueSubmit(g.queue, 1, &si, f.fence), "vkQueueSubmit");
    f.submitted = true;
    g.recording = false;
    if (present && g.platform->present) {
        g.platform->present();
    }
    g.frame_slot = (g.frame_slot + 1) % kFrames;
}

static void vk_finish_render(void) {
}

static int vk_create_framebuffer(void) {
    g.fbs.emplace_back();
    return (int)g.fbs.size() - 1;
}

static void vk_update_framebuffer_parameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                             bool opengl_invert_y, bool render_target, bool has_depth_buffer,
                                             bool can_extract_depth) {
    (void)render_target;
    (void)can_extract_depth;
    Framebuffer &fb = g.fbs[fb_id];
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    uint32_t samples = 1;
    for (uint32_t s = 16; s > 1; s >>= 1) {
        if (msaa_level >= s && (g.sample_counts & s)) {
            samples = s;
            break;
        }
    }
    if (!gfx_framebuffers_enabled && fb_id != 0) has_depth_buffer = false;
    const bool resize = fb.width != width || fb.height != height || fb.msaa != samples || !fb.color.image;
    if (resize || (has_depth_buffer && !fb.depth.image)) {
        if ((size_t)fb_id == g.current_fb) end_pass();
        if (resize) {
            doom(fb.color);
            doom(fb.resolved);
            fb.color = create_image(width, height, 1, kColorFormat, (VkSampleCountFlagBits)samples,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        (samples == 1 ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                                    true);
            doom(fb.depth);
        }
        if (has_depth_buffer && !fb.depth.image) {
            fb.depth = create_image(width, height, 1, kDepthFormat, (VkSampleCountFlagBits)samples,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    false);
        }
        // New images start cleared, as gfx_pc clears before drawing anyway.
        clear_now(fb, true, true);
        g.desc_dirty = true;
    }
    if ((size_t)fb_id == g.current_fb && fb.has_depth != has_depth_buffer) end_pass();
    fb.width = width;
    fb.height = height;
    fb.msaa = samples;
    fb.has_depth = has_depth_buffer;
    fb.invert_y = opengl_invert_y;
}

static bool vk_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    if (fb_id < 0 || fb_id >= (int)g.fbs.size() || (fb_id != 0 && !gfx_framebuffers_enabled)) return false;
    if (noise_scale != 0.0f) g.noise_scale = 1.0f / noise_scale;
    if ((size_t)fb_id != g.current_fb) {
        end_pass();
        g.current_fb = (size_t)fb_id;
        g.dyn_dirty = true;
    }
    return true;
}

static void vk_clear_framebuffer(bool clear_color, bool clear_depth) {
    Framebuffer &fb = cur_fb();
    if (!fb.color.image) return;
    if (!g.pass_active) {
        clear_now(fb, clear_color, clear_depth);
        return;
    }
    // Inside a pass: the whole framebuffer (GL clears with the scissor off).
    VkClearAttachment att[2];
    uint32_t n = 0;
    if (clear_color) {
        att[n] = {};
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[n].colorAttachment = 0;
        att[n].clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        n++;
    }
    if (clear_depth && fb.has_depth) {
        att[n] = {};
        att[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        att[n].clearValue.depthStencil = {1.0f, 0};
        n++;
    }
    if (!n) return;
    VkClearRect rect = {{{0, 0}, {fb.width, fb.height}}, 0, 1};
    vkCmdClearAttachments(frame().draw, n, att, 1, &rect);
}

static void vk_resolve_msaa_color_buffer(int fb_dst, int fb_src) {
    if (!gfx_framebuffers_enabled) return;
    Framebuffer &dst = g.fbs[fb_dst];
    Framebuffer &src = g.fbs[fb_src];
    if (!dst.color.image || !src.color.image) return;
    end_pass();
    VkCommandBuffer cmd = frame().draw;
    barrier(cmd, src.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cmd, dst.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (src.color.samples != VK_SAMPLE_COUNT_1_BIT && dst.color.samples == VK_SAMPLE_COUNT_1_BIT &&
        src.width == dst.width && src.height == dst.height) {
        VkImageResolve r = {};
        r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.extent = {src.width, src.height, 1};
        vkCmdResolveImage(cmd, src.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.color.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    } else {
        Image &readable = readable_color(fb_src);
        barrier(cmd, readable, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit blit = {};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t)src.width, (int32_t)src.height, 1};
        blit.dstOffsets[1] = {(int32_t)dst.width, (int32_t)dst.height, 1};
        vkCmdBlitImage(cmd, readable.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.color.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }
}

static void *vk_get_framebuffer_texture_id(int fb_id) {
    return (void *)(uintptr_t)fb_id;
}

static void vk_select_texture_fb(int fb_id) {
    g.bound[0] = {0, fb_id};
    g.linear_filter[0] = true;
    g.active_unit = 0;
    g.desc_dirty = true;
}

static void vk_copy_framebuffer(int fb_dst, int fb_src, int left, int top, bool flip_y, bool use_back) {
    (void)use_back;   // fb 0 is the frame being drawn ("back"); the shown one is gone
    if (!gfx_framebuffers_enabled || fb_dst >= (int)g.fbs.size() || fb_src >= (int)g.fbs.size()) return;
    Framebuffer &dst = g.fbs[fb_dst];
    Framebuffer &src = g.fbs[fb_src];
    if (!dst.color.image || !src.color.image || dst.color.samples != VK_SAMPLE_COUNT_1_BIT) return;
    // gfx_opengl.cpp's glBlitFramebuffer rectangles (same coordinates here).
    int sx0, sy0, sx1, sy1;
    int dy0 = 0, dy1 = (int)dst.height;
    if (left >= 0 && top >= 0) {
        sx0 = left;
        sy0 = top;
        sx1 = left + (int)dst.width;
        sy1 = top + (int)dst.height;
    } else {
        sx0 = 0;
        sy0 = 0;
        sx1 = (int)src.width;
        sy1 = (int)src.height;
    }
    if (flip_y) std::swap(dy0, dy1);
    // Vulkan needs the source inside the image (GL leaves the rest untouched).
    sx0 = std::clamp(sx0, 0, (int)src.width);
    sx1 = std::clamp(sx1, 0, (int)src.width);
    sy0 = std::clamp(sy0, 0, (int)src.height);
    sy1 = std::clamp(sy1, 0, (int)src.height);
    if (sx0 == sx1 || sy0 == sy1) return;
    end_pass();
    VkCommandBuffer cmd = frame().draw;
    Image &readable = readable_color(fb_src);
    barrier(cmd, readable, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cmd, dst.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {sx0, sy0, 0};
    blit.srcOffsets[1] = {sx1, sy1, 1};
    blit.dstOffsets[0] = {0, dy0, 0};
    blit.dstOffsets[1] = {(int32_t)dst.width, dy1, 1};
    vkCmdBlitImage(cmd, readable.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.color.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
}

static void vk_delete_texture(uint32_t id) {
    auto it = g.textures.find(id);
    if (it == g.textures.end()) return;
    doom(it->second.img);
    g.textures.erase(it);
}

static void vk_set_texture_filter(FilteringMode mode) {
    g.filter_mode = mode;
}

static FilteringMode vk_get_texture_filter(void) {
    return g.filter_mode;
}

static void vk_set_mipmap_filter(MipmapFilteringMode mode) {
    g.mipmap_mode = mode;
}

static void vk_set_anisotropy_level(int level) {
    g.anisotropy_level = level;
}

static int vk_get_max_anisotropy_level(void) {
    return g.anisotropy ? (int)g.props.limits.maxSamplerAnisotropy : 1;
}

struct GfxRenderingAPI gfx_vulkan_api = {
    vk_get_name,
    vk_get_max_texture_size,
    vk_get_clip_parameters,
    vk_unload_shader,
    vk_load_shader,
    vk_create_and_load_new_shader,
    vk_lookup_shader,
    vk_shader_get_info,
    vk_clear_shaders,
    vk_new_texture,
    vk_select_texture,
    vk_upload_texture,
    vk_set_sampler_parameters,
    vk_set_depth_mode,
    vk_set_depth_range,
    vk_set_viewport,
    vk_set_scissor,
    vk_set_use_alpha,
    vk_draw_triangles,
    vk_init,
    vk_on_resize,
    vk_start_frame,
    vk_end_frame,
    vk_finish_render,
    vk_create_framebuffer,
    vk_update_framebuffer_parameters,
    vk_start_draw_to_framebuffer,
    vk_copy_framebuffer,
    vk_clear_framebuffer,
    vk_resolve_msaa_color_buffer,
    vk_get_framebuffer_texture_id,
    vk_select_texture_fb,
    vk_delete_texture,
    vk_set_texture_filter,
    vk_get_texture_filter,
    vk_set_mipmap_filter,
    vk_set_anisotropy_level,
    vk_get_max_anisotropy_level,
};

extern "C" void gfx_vulkan_set_platform(const struct GfxVulkanPlatform *platform) {
    g.platform = platform;
}

extern "C" bool gfx_vulkan_dump_screen(const char *path) {
    if (g.fbs.empty() || !g.fbs[0].color.image || g.recording) return false;
    Framebuffer &screen = g.fbs[0];
    vkDeviceWaitIdle(g.device);
    const uint32_t w = screen.width, h = screen.height;
    Buffer rb = create_buffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandPool(g.device, g.misc_pool, 0);
    vkBeginCommandBuffer(g.misc_cmd, &bi);
    Image &src = screen.color;   // fb 0 is single-sample
    barrier(g.misc_cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(g.misc_cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &copy);
    vkEndCommandBuffer(g.misc_cmd);
    vkResetFences(g.device, 1, &g.misc_fence);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.misc_cmd;
    vk_check(vkQueueSubmit(g.queue, 1, &si, g.misc_fence), "vkQueueSubmit");
    vkWaitForFences(g.device, 1, &g.misc_fence, VK_TRUE, UINT64_MAX);
    vmaInvalidateAllocation(g.vma, rb.alloc, 0, VK_WHOLE_SIZE);
    bool ok = false;
    if (FILE *f = fopen(path, "wb")) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        std::vector<uint8_t> row(w * 3);
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *p = rb.mapped + (size_t)(h - 1 - y) * w * 4;   // bottom row first in memory
            for (uint32_t x = 0; x < w; x++) {
                row[x * 3 + 0] = p[x * 4 + 0];
                row[x * 3 + 1] = p[x * 4 + 1];
                row[x * 3 + 2] = p[x * 4 + 2];
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
        ok = true;
    }
    vmaDestroyBuffer(g.vma, rb.buffer, rb.alloc);
    return ok;
}
