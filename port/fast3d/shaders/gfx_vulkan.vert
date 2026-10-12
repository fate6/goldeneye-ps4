#version 450
#extension GL_GOOGLE_include_directive : require
// fast3d Vulkan backend: one vertex shader for every colour-combiner state.
// gfx_pc.cpp emits a per-state float layout (position, then the optional
// texcoords/clamps/fog/grayscale/inputs, see gfx_vulkan.cpp); the vertex is
// read from a storage buffer using the offsets in the push constants, so no
// pipeline depends on the layout. Mirrors gfx_opengl.cpp's generated shaders.
//
// AFFINE (build-time define): RDP-style screen-affine shade colour and fog
// (gfx_rdp_affine, D540/D543), as the OpenGL backend's noperspective.

#include "gfx_vulkan_common.glsl"

layout(std430, set = 0, binding = 0) readonly buffer Vertices {
    float v[];
};

layout(location = 0) out vec2 vTexCoord0;
layout(location = 1) out vec2 vTexCoord1;
layout(location = 2) out vec4 vTexClamp;      // s0, t0, s1, t1
layout(location = 3) FOG_Q out vec4 vFog;
layout(location = 4) out vec4 vGrayscaleColor;
layout(location = 5) SHADE_Q out vec4 vInput1;
layout(location = 6) SHADE_Q out vec4 vInput2;
layout(location = 7) SHADE_Q out vec4 vInput3;
layout(location = 8) SHADE_Q out vec4 vInput4;

vec4 inputAt(uint base, uint off, bool alpha) {
    return vec4(v[base + off], v[base + off + 1], v[base + off + 2], alpha ? v[base + off + 3] : 1.0);
}

void main() {
    uint base = pc.vtx_base + uint(gl_VertexIndex) * pc.vtx_stride;
    gl_Position = vec4(v[base], v[base + 1], v[base + 2], v[base + 3]);

    vTexCoord0 = vec2(0.0);
    vTexCoord1 = vec2(0.0);
    vTexClamp = vec4(1.0);
    if (hasFlag(F_TEX0)) {
        uint o = layoutOffset(L_TC0);
        vTexCoord0 = vec2(v[base + o], v[base + o + 1]);
        if (hasFlag(F_CLAMP0S)) vTexClamp.x = v[base + layoutOffset(L_CLAMP0S)];
        if (hasFlag(F_CLAMP0T)) vTexClamp.y = v[base + layoutOffset(L_CLAMP0T)];
    }
    if (hasFlag(F_TEX1)) {
        uint o = layoutOffset(L_TC1);
        vTexCoord1 = vec2(v[base + o], v[base + o + 1]);
        if (hasFlag(F_CLAMP1S)) vTexClamp.z = v[base + layoutOffset(L_CLAMP1S)];
        if (hasFlag(F_CLAMP1T)) vTexClamp.w = v[base + layoutOffset(L_CLAMP1T)];
    }
    vFog = hasFlag(F_FOG) ? inputAt(base, layoutOffset(L_FOG), true) : vec4(0.0);
    vGrayscaleColor = hasFlag(F_GRAYSCALE) ? inputAt(base, layoutOffset(L_GRAY), true) : vec4(0.0);

    uint n = numInputs();
    bool alpha = hasFlag(F_ALPHA);
    uint isz = alpha ? 4u : 3u;
    uint io = layoutOffset(L_INPUTS);
    vInput1 = n >= 1u ? inputAt(base, io, alpha) : vec4(0.0);
    vInput2 = n >= 2u ? inputAt(base, io + isz, alpha) : vec4(0.0);
    vInput3 = n >= 3u ? inputAt(base, io + 2u * isz, alpha) : vec4(0.0);
    vInput4 = n >= 4u ? inputAt(base, io + 3u * isz, alpha) : vec4(0.0);
}
