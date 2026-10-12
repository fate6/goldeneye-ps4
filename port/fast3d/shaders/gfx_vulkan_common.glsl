// Shared by gfx_vulkan.vert/.frag. Keep in sync with struct GfxVkPush in
// gfx_vulkan.cpp.

// Interpolation qualifiers (build-time variants, see gfx_vulkan.cpp):
// AFFINE = shade colour interpolated screen-affine like the RDP;
// FOG_AFFINE = the per-vertex fog value too (default), else perspective
// (GE_FOGPIXEL, fog * w divided by w per pixel).
#if AFFINE
#define SHADE_Q noperspective
#else
#define SHADE_Q
#endif
#if FOG_AFFINE
#define FOG_Q noperspective
#else
#define FOG_Q
#endif

layout(push_constant) uniform Push {
    uint vtx_base;       // first float of this draw's vertices in the buffer
    uint vtx_stride;     // floats per vertex
    uint layout_lo;      // 8-bit float offsets: tc0, tc1, clamp0s, clamp0t
    uint layout_hi;      // clamp1s, clamp1t, fog, grayscale
    uint layout_in;      // inputs (bits 0-7)
    uint flags;          // F_* below; bits 24-26 = number of inputs
    uint cc[4];          // combiner slots: c[cycle][rgb, alpha][a, b, c, d], 4 bits each
    uint frame_count;
    float noise_scale;
} pc;

const uint F_TEX0 = 1u << 0;
const uint F_TEX1 = 1u << 1;
const uint F_CLAMP0S = 1u << 2;
const uint F_CLAMP0T = 1u << 3;
const uint F_CLAMP1S = 1u << 4;
const uint F_CLAMP1T = 1u << 5;
const uint F_FOG = 1u << 6;
const uint F_GRAYSCALE = 1u << 7;
const uint F_ALPHA = 1u << 8;
const uint F_TEXTURE_EDGE = 1u << 9;
const uint F_NOISE = 1u << 10;
const uint F_2CYC = 1u << 11;
const uint F_ALPHA_THRESHOLD = 1u << 12;
const uint F_INVISIBLE = 1u << 13;
const uint F_BLUR = 1u << 14;
const uint F_THREE_POINT_MODE = 1u << 15;   // texture filter mode is 3-point
const uint F_THREE_POINT0 = 1u << 16;       // per-tile linear filter flag (3-point applies)
const uint F_THREE_POINT1 = 1u << 17;
const uint F_FOGPIXEL = 1u << 20;

const uint L_TC0 = 0u, L_TC1 = 1u, L_CLAMP0S = 2u, L_CLAMP0T = 3u;
const uint L_CLAMP1S = 4u, L_CLAMP1T = 5u, L_FOG = 6u, L_GRAY = 7u, L_INPUTS = 8u;

bool hasFlag(uint f) {
    return (pc.flags & f) != 0u;
}

uint numInputs() {
    return (pc.flags >> 24) & 7u;
}

uint layoutOffset(uint which) {
    const uint word = which < 4u ? pc.layout_lo : (which < 8u ? pc.layout_hi : pc.layout_in);
    return (word >> ((which & 3u) * 8u)) & 0xffu;
}
