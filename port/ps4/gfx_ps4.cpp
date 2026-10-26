// PS4 window-manager backend for fast3d: the TV as a fullscreen "window" at
// the render resolution, frame pacing, and the gfx_sdl_* helpers video.c calls
// (no GL context here). Presentation (VideoOut flips, scaled to the 1920x1080
// output) is in ps4_vulkan.cpp; swap_buffers paces (unless the flips do) and
// counts frames.

#include <stdint.h>
#include <time.h>

#include "platform.h"
#include "system.h"
#include "ps4_platform.h"

#include "../fast3d/gfx_window_manager_api.h"

// Video.RenderResolution (video.c): 0 1080p, 1 1440p, 2 2160p. Read every
// frame, so a change applies at once.
extern "C" int gfx_ps4_render_res = 0;
static const uint32_t kRenderW[3] = {1920, 2560, 3840};
static const uint32_t kRenderH[3] = {1080, 1440, 2160};

static int render_res(void) {
    return gfx_ps4_render_res >= 0 && gfx_ps4_render_res <= 2 ? gfx_ps4_render_res : 0;
}

extern "C" void (*gfx_pre_swap_hook)(void) = NULL;   // video.c: frame capture
// Set by ps4_vulkan.cpp: VideoOut flips (vsync) pace the frames, no sleep here.
extern "C" int gfx_ps4_vsync_paced = 0;

static int target_fps = 0;
static uint64_t next_frame_us = 0;
static uint64_t frames = 0, frames_logged_at_us = 0;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static void ps4_init(const GfxWindowInitSettings *) {
    sysLogPrintf(LOG_INFO, "gfx_ps4: render %ux%u", kRenderW[render_res()], kRenderH[render_res()]);
}
static void ps4_close(void) {}
static int ps4_get_display_mode(int modenum, int *w, int *h) {
    if (modenum != 0) return 0;
    *w = (int)kRenderW[render_res()];
    *h = (int)kRenderH[render_res()];
    return 1;
}
static int ps4_get_current_display_mode(int *w, int *h) { return ps4_get_display_mode(0, w, h); }
static int ps4_get_num_display_modes(void) { return 1; }
static int32_t ps4_get_fullscreen_state(void) { return 1; }
static void ps4_set_fullscreen_changed_callback(void (*)(bool)) {}
static void ps4_set_fullscreen(bool) {}
static void ps4_set_fullscreen_exclusive(bool) {}
static void ps4_set_fullscreen_flag(int32_t) {}
static int32_t ps4_get_fullscreen_flag_mode(void) { return 0; }
static int32_t ps4_get_maximized_state(void) { return 0; }
static void ps4_set_maximize(bool) {}
static void ps4_get_active_window_refresh_rate(uint32_t *rate) { *rate = 60; }
static void ps4_set_cursor_visibility(bool) {}
static void ps4_set_closest_resolution(int32_t, int32_t, bool) {}
static void ps4_set_dimensions(uint32_t, uint32_t, int32_t, int32_t) {}
static void ps4_get_dimensions(uint32_t *w, uint32_t *h, int32_t *x, int32_t *y) {
    *w = kRenderW[render_res()];
    *h = kRenderH[render_res()];
    *x = 0;
    *y = 0;
}
static void ps4_get_centered_positions(int32_t, int32_t, int32_t *x, int32_t *y) {
    *x = 0;
    *y = 0;
}
static void ps4_handle_events(void) {}

static void pace(int fps) {
    const uint64_t interval = 1000000ull / (uint64_t)fps;
    uint64_t t = now_us();
    if (next_frame_us == 0 || t > next_frame_us + interval) next_frame_us = t;
    if (next_frame_us > t) sysSleep((uint32_t)(next_frame_us - t));
    next_frame_us += interval;
}

// While the system UI (PS button) covers the game, draw nothing and leave the
// GPU to it: the TV keeps the last frame under the menu. No flips then, so
// pace the dropped frames here to keep the game's rate.
static bool ps4_start_frame(void) {
    if (!ps4SystemUiOverlaid()) return true;
    pace(target_fps > 0 ? target_fps : 60);
    return false;
}

static void ps4_swap_buffers_begin(void) {
    if (target_fps > 0 && !gfx_ps4_vsync_paced) pace(target_fps);
    if (gfx_pre_swap_hook) gfx_pre_swap_hook();
    frames++;
    const uint64_t t = now_us();
    if (frames_logged_at_us == 0) frames_logged_at_us = t;
    if (t - frames_logged_at_us >= 5000000ull) {
        sysLogPrintf(LOG_INFO, "gfx_ps4: %llu frames", (unsigned long long)frames);
        frames_logged_at_us = t;
    }
}
static void ps4_swap_buffers_end(void) {}
static double ps4_get_time(void) { return now_us() / 1e6; }
static int32_t ps4_get_target_fps(void) { return target_fps; }
static void ps4_set_target_fps(int fps) { target_fps = fps; }
static bool ps4_can_disable_vsync(void) { return false; }
static void *ps4_get_window_handle(void) { return nullptr; }
static void ps4_set_window_title(const char *) {}
static int ps4_get_swap_interval(void) { return 1; }
static bool ps4_set_swap_interval(int) { return true; }

struct GfxWindowManagerAPI gfx_ps4 = {
    ps4_init,
    ps4_close,
    ps4_get_display_mode,
    ps4_get_current_display_mode,
    ps4_get_num_display_modes,
    ps4_get_fullscreen_state,
    ps4_set_fullscreen_changed_callback,
    ps4_set_fullscreen,
    ps4_set_fullscreen_exclusive,
    ps4_set_fullscreen_flag,
    ps4_get_fullscreen_flag_mode,
    ps4_get_maximized_state,
    ps4_set_maximize,
    ps4_get_active_window_refresh_rate,
    ps4_set_cursor_visibility,
    ps4_set_closest_resolution,
    ps4_set_dimensions,
    ps4_get_dimensions,
    ps4_get_centered_positions,
    ps4_handle_events,
    ps4_start_frame,
    ps4_swap_buffers_begin,
    ps4_swap_buffers_end,
    ps4_get_time,
    ps4_get_target_fps,
    ps4_set_target_fps,
    ps4_can_disable_vsync,
    ps4_get_window_handle,
    ps4_set_window_title,
    ps4_get_swap_interval,
    ps4_set_swap_interval,
};

// video.c calls these SDL/GL-context helpers; nothing to bind on the PS4.
extern "C" void gfx_sdl_make_context_current(void) {}
extern "C" void gfx_sdl_release_context(void) {}
extern "C" void gfx_sdl_park_for_exit(void) {}
extern "C" void gfx_sdl_update_cached_size(void) {}
