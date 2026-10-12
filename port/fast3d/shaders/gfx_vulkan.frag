#version 450
#extension GL_GOOGLE_include_directive : require
// fast3d Vulkan backend: one fragment shader for every colour-combiner state.
// The combiner (gfx_cc.cpp's CCFeatures) arrives in push constants and is
// evaluated here; the result matches the GLSL that gfx_opengl.cpp generates
// per state (same formulas, same wrap/clamp steps, same discard rules).

#include "gfx_vulkan_common.glsl"

layout(set = 0, binding = 1) uniform sampler2D uTex0;
layout(set = 0, binding = 2) uniform sampler2D uTex1;

layout(location = 0) in vec2 vTexCoord0;
layout(location = 1) in vec2 vTexCoord1;
layout(location = 2) in vec4 vTexClamp;
layout(location = 3) FOG_Q in vec4 vFog;
layout(location = 4) in vec4 vGrayscaleColor;
layout(location = 5) SHADE_Q in vec4 vInput1;
layout(location = 6) SHADE_Q in vec4 vInput2;
layout(location = 7) SHADE_Q in vec4 vInput3;
layout(location = 8) SHADE_Q in vec4 vInput4;

layout(location = 0) out vec4 outColor;

// gfx_cc.h SHADER_* items
const uint S_0 = 0u, S_INPUT_1 = 1u, S_INPUT_2 = 2u, S_INPUT_3 = 3u, S_INPUT_4 = 4u;
const uint S_TEXEL0 = 8u, S_TEXEL0A = 9u, S_TEXEL1 = 10u, S_TEXEL1A = 11u;
const uint S_1 = 12u, S_COMBINED = 13u, S_NOISE = 14u;

#define WRAP(x, low, high) (mod((x) - (low), (high) - (low)) + (low))

float random(in vec3 value) {
    float r = dot(sin(value), vec3(12.9898, 78.233, 37.719));
    return fract(sin(r) * 143758.5453);
}

float randNoise() {
    return (random(vec3(floor(gl_FragCoord.xy * pc.noise_scale), float(pc.frame_count))) + 1.0) / 2.0;
}

vec4 sampleTex(uint t, vec2 uv) {
    return t == 0u ? texture(uTex0, uv) : texture(uTex1, uv);
}

vec4 filter3point(uint t, vec2 texCoord, vec2 texSize) {
    vec2 offset = fract(texCoord * texSize - vec2(0.5));
    offset -= step(1.0, offset.x + offset.y);
    vec4 c0 = sampleTex(t, texCoord - offset / texSize);
    vec4 c1 = sampleTex(t, texCoord - vec2(offset.x - sign(offset.x), offset.y) / texSize);
    vec4 c2 = sampleTex(t, texCoord - vec2(offset.x, offset.y - sign(offset.y)) / texSize);
    return c0 + abs(offset.x) * (c1 - c0) + abs(offset.y) * (c2 - c0);
}

vec4 filteredTex(uint t, vec2 uv, vec2 texSize) {
    bool threePoint = hasFlag(F_THREE_POINT_MODE) &&
                            hasFlag(t == 0u ? F_THREE_POINT0 : F_THREE_POINT1);
    return threePoint ? filter3point(t, uv, texSize) : sampleTex(t, uv);
}

vec4 hookTexture2D(uint t, vec2 uv, vec2 texSize) {
    if (!hasFlag(F_BLUR)) {
        return filteredTex(t, uv, texSize);
    }
    // Blur filter, used for menu backgrounds.
    vec4 cw = vec4(0.0);
    for (int i = 0; i < 16; ++i) {
        vec2 xy = vec2(float(i & 3), float(i >> 2));
        float w = 0.009947 - length(xy) * 0.001;
        vec2 scaled_uv = uv + (vec2(-1.5) + xy) / texSize;
        vec4 tex = filteredTex(t, scaled_uv, texSize);
        cw += vec4(tex.rgb * w, w);
    }
    return vec4(cw.rgb / cw.a, 1.0);
}

vec4 texVal(uint t) {
    vec2 texSize = vec2(t == 0u ? textureSize(uTex0, 0) : textureSize(uTex1, 0));
    vec2 uv = t == 0u ? vTexCoord0 : vTexCoord1;
    bool cs = hasFlag(t == 0u ? F_CLAMP0S : F_CLAMP1S);
    bool ct = hasFlag(t == 0u ? F_CLAMP0T : F_CLAMP1T);
    vec2 clampTo = t == 0u ? vTexClamp.xy : vTexClamp.zw;
    if (cs) uv.x = clamp(uv.x, 0.5 / texSize.x, clampTo.x);
    if (ct) uv.y = clamp(uv.y, 0.5 / texSize.y, clampTo.y);
    return hookTexture2D(t, uv, texSize);
}

vec4 item(uint s, vec4 in1, vec4 in2, vec4 in3, vec4 in4, vec4 tex0, vec4 tex1, vec4 combined) {
    switch (s) {
        case S_0: return vec4(0.0);
        case S_1: return vec4(1.0);
        case S_INPUT_1: return in1;
        case S_INPUT_2: return in2;
        case S_INPUT_3: return in3;
        case S_INPUT_4: return in4;
        case S_TEXEL0: return tex0;
        case S_TEXEL0A: return vec4(tex0.a);
        case S_TEXEL1: return tex1;
        case S_TEXEL1A: return vec4(tex1.a);
        case S_COMBINED: return combined;
        case S_NOISE: return vec4(randNoise());
    }
    return vec4(0.0);
}

uint slot(uint cyc, uint alpha, uint k) {
    uint shift = (cyc * 32u + alpha * 16u + k * 4u);
    return (pc.cc[shift >> 5u] >> (shift & 31u)) & 0xfu;   // pc.cc[0..1] = shader_id0
}

// One combiner equation (gfx_opengl.cpp append_formula, same precedence:
// single (c == 0), multiply (b == 0 && d == 0), mix (b == d), general).
vec4 formula(uint cyc, uint alpha, vec4 in1, vec4 in2, vec4 in3, vec4 in4, vec4 tex0, vec4 tex1,
             vec4 combined) {
    uint sa = slot(cyc, alpha, 0u), sb = slot(cyc, alpha, 1u);
    uint sc = slot(cyc, alpha, 2u), sd = slot(cyc, alpha, 3u);
    vec4 a = item(sa, in1, in2, in3, in4, tex0, tex1, combined);
    vec4 b = item(sb, in1, in2, in3, in4, tex0, tex1, combined);
    vec4 c = item(sc, in1, in2, in3, in4, tex0, tex1, combined);
    vec4 d = item(sd, in1, in2, in3, in4, tex0, tex1, combined);
    if (sc == S_0) return d;
    if (sb == S_0 && sd == S_0) return a * c;
    if (sb == sd) return mix(b, a, c);
    return (a - b) * c + d;
}

void main() {
    // D548: combiner inputs are 8-bit on the N64: never above 1.0.
    vec4 in1 = clamp(vInput1, 0.0, 1.0);
    vec4 in2 = clamp(vInput2, 0.0, 1.0);
    vec4 in3 = clamp(vInput3, 0.0, 1.0);
    vec4 in4 = clamp(vInput4, 0.0, 1.0);
    vec4 tex0 = hasFlag(F_TEX0) ? texVal(0u) : vec4(0.0);
    vec4 tex1 = hasFlag(F_TEX1) ? texVal(1u) : vec4(0.0);
    bool optAlpha = hasFlag(F_ALPHA);

    vec4 texel = vec4(0.0);
    uint cycles = hasFlag(F_2CYC) ? 2u : 1u;
    for (uint cyc = 0u; cyc < cycles; ++cyc) {
        bool colorAlphaSame =
            ((pc.cc[cyc] & 0xffffu) == (pc.cc[cyc] >> 16u));
        vec4 rgb = formula(cyc, 0u, in1, in2, in3, in4, tex0, tex1, texel);
        if (optAlpha && !colorAlphaSame) {
            texel = vec4(rgb.rgb, formula(cyc, 1u, in1, in2, in3, in4, tex0, tex1, texel).a);
        } else {
            texel = rgb;   // alpha unused unless optAlpha (then the alpha slots equal these)
        }
        if (cyc == 0u) {
            texel = WRAP(texel, -1.01, 1.01);
        }
    }
    texel = WRAP(texel, -0.51, 1.51);
    texel = clamp(texel, 0.0, 1.0);

    if (hasFlag(F_FOG)) {
        // D543: per-vertex RSP fog; GE_FOGPIXEL (D540): fog * w divided by w here.
        float fogA = clamp(hasFlag(F_FOGPIXEL) ? vFog.a * gl_FragCoord.w : vFog.a, 0.0, 1.0);
        texel.rgb = mix(texel.rgb, vFog.rgb, fogA);
    }

    // D266: CVG_X_ALPHA (texture edge) keyed on the alphacompare mode.
    if (hasFlag(F_TEXTURE_EDGE) && optAlpha) {
        if (hasFlag(F_NOISE)) {
            if (texel.a < randNoise()) discard;
        } else if (hasFlag(F_ALPHA_THRESHOLD)) {
            if (texel.a < 1.0 / 256.0) discard;
        } else {
            if (texel.a < 0.5) discard;
        }
    }
    if (optAlpha && hasFlag(F_NOISE) && !hasFlag(F_TEXTURE_EDGE)) {
        texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * pc.noise_scale), float(pc.frame_count))) +
                               texel.a, 0.0, 1.0));
    }

    if (hasFlag(F_GRAYSCALE)) {
        float intensity = (texel.r + texel.g + texel.b) / 3.0;
        vec3 new_texel = vGrayscaleColor.rgb * intensity;
        texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);
    }

    if (optAlpha) {
        if (hasFlag(F_ALPHA_THRESHOLD) && texel.a < 8.0 / 256.0) discard;
        if (hasFlag(F_INVISIBLE)) texel.a = 0.0;
        outColor = texel;
    } else {
        outColor = vec4(texel.rgb, 1.0);
    }
}
