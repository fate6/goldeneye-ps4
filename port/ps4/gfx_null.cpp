// PS4 bring-up renderer: implements the fast3d rendering API and draws
// nothing, so the game and the software RSP run headless (Phase 1). Replaced
// by the Vulkan backend. Also provides the gfx_opengl_* helpers video.c calls.

#include <map>
#include <stdint.h>
#include <utility>

#include "../fast3d/gfx_cc.h"
#include "../fast3d/gfx_rendering_api.h"

struct ShaderProgram {
    uint8_t num_inputs;
    bool used_textures[2];
};

static std::map<std::pair<uint64_t, uint32_t>, ShaderProgram> s_shaders;
static uint32_t s_next_texture = 1;
static int s_next_framebuffer = 1;
static FilteringMode s_filter = FILTER_LINEAR;

static const char *null_get_name(void) { return "null (PS4 bring-up)"; }
static int null_get_max_texture_size(void) { return 8192; }
static GfxClipParameters null_get_clip_parameters(void) { return {false, false}; }
static void null_unload_shader(ShaderProgram *) {}
static void null_load_shader(ShaderProgram *) {}

static ShaderProgram *null_create_and_load_new_shader(uint64_t id0, uint32_t id1) {
    CCFeatures cc;
    gfx_cc_get_features(id0, id1, &cc);
    ShaderProgram &prg = s_shaders[{id0, id1}];
    prg.num_inputs = (uint8_t)cc.num_inputs;
    prg.used_textures[0] = cc.used_textures[0];
    prg.used_textures[1] = cc.used_textures[1];
    return &prg;
}

static ShaderProgram *null_lookup_shader(uint64_t id0, uint32_t id1) {
    auto it = s_shaders.find({id0, id1});
    return it == s_shaders.end() ? nullptr : &it->second;
}

static void null_shader_get_info(ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static void null_clear_shaders(void) { s_shaders.clear(); }
static uint32_t null_new_texture(void) { return s_next_texture++; }
static void null_select_texture(int, uint32_t, bool) {}
static void null_upload_texture(const uint8_t *, uint32_t, uint32_t, bool) {}
static void null_set_sampler_parameters(int, bool, uint32_t, uint32_t, bool) {}
static void null_set_depth_mode(bool, bool, bool, bool, uint16_t) {}
static void null_set_depth_range(float, float) {}
static void null_set_viewport(int, int, int, int) {}
static void null_set_scissor(int, int, int, int) {}
static void null_set_use_alpha(bool, bool) {}
static void null_draw_triangles(float[], size_t, size_t) {}
static void null_init(void) {}
static void null_on_resize(void) {}
static void null_start_frame(void) {}
static void null_end_frame(void) {}
static void null_finish_render(void) {}
static int null_create_framebuffer(void) { return s_next_framebuffer++; }
static void null_update_framebuffer_parameters(int, uint32_t, uint32_t, uint32_t, bool, bool, bool, bool) {}
static bool null_start_draw_to_framebuffer(int, float) { return true; }
static void null_copy_framebuffer(int, int, int, int, bool, bool) {}
static void null_clear_framebuffer(bool, bool) {}
static void null_resolve_msaa_color_buffer(int, int) {}
static void *null_get_framebuffer_texture_id(int fb_id) { return (void *)(uintptr_t)fb_id; }
static void null_select_texture_fb(int) {}
static void null_delete_texture(uint32_t) {}
static void null_set_texture_filter(FilteringMode mode) { s_filter = mode; }
static FilteringMode null_get_texture_filter(void) { return s_filter; }
static void null_set_mipmap_filter(MipmapFilteringMode) {}
static void null_set_anisotropy_level(int) {}
static int null_get_max_anisotropy_level(void) { return 16; }

struct GfxRenderingAPI gfx_null_api = {
    null_get_name,
    null_get_max_texture_size,
    null_get_clip_parameters,
    null_unload_shader,
    null_load_shader,
    null_create_and_load_new_shader,
    null_lookup_shader,
    null_shader_get_info,
    null_clear_shaders,
    null_new_texture,
    null_select_texture,
    null_upload_texture,
    null_set_sampler_parameters,
    null_set_depth_mode,
    null_set_depth_range,
    null_set_viewport,
    null_set_scissor,
    null_set_use_alpha,
    null_draw_triangles,
    null_init,
    null_on_resize,
    null_start_frame,
    null_end_frame,
    null_finish_render,
    null_create_framebuffer,
    null_update_framebuffer_parameters,
    null_start_draw_to_framebuffer,
    null_copy_framebuffer,
    null_clear_framebuffer,
    null_resolve_msaa_color_buffer,
    null_get_framebuffer_texture_id,
    null_select_texture_fb,
    null_delete_texture,
    null_set_texture_filter,
    null_get_texture_filter,
    null_set_mipmap_filter,
    null_set_anisotropy_level,
    null_get_max_anisotropy_level,
};

// video.c's OpenGL-only helpers (low-end detection, frame dumps).
extern "C" const char *gfx_opengl_renderer_string(void) { return ""; }
extern "C" bool gfx_opengl_pcdump_enabled(void) { return false; }
extern "C" bool gfx_opengl_dump_bound_fbo(uint32_t, uint32_t, const char *) { return false; }
