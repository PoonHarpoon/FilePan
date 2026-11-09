// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#version 450

#include "common.glsl"
#include "shapes.glsl"

// The one shader every instance kind goes through, so runs of mixed commands batch into one draw.
// Output is premultiplied; colors blend in sRGB space on a UNORM target. Textures are sampled
// with explicit LOD because control flow here isn't uniform.

layout(set = 0, binding = 2) uniform sampler2DArray alphaAtlas;
layout(set = 0, binding = 3) uniform sampler2D colorAtlas;
// An image, a layer or a blur result, depending on the draw.
layout(set = 1, binding = 0) uniform sampler2D aux;

layout(location = 0) in vec2 vLocal;
layout(location = 1) flat in uint vIndex;
layout(location = 2) flat in float vScale;
layout(location = 3) in vec2 vDevice;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265358979;

vec4 premultiply(vec4 c) {
    return vec4(c.rgb * c.a, c.a);
}

vec4 stopColor(Instance inst, uint i) {
    return premultiply(unpackUnorm4x8(inst.colors[i]));
}

float stopOffset(Instance inst, uint i) {
    vec2 pair = unpackUnorm2x16(inst.offsets[i >> 1]);
    return (i & 1u) == 0u ? pair.x : pair.y;
}

// CSS gradient stops, interpolated in premultiplied sRGB.
vec4 gradient(Instance inst, float t) {
    uint count = (inst.header >> 12) & 7u;
    if (count == 0u) {
        return vec4(0.0);
    }
    vec4 color = stopColor(inst, 0u);
    float offset = stopOffset(inst, 0u);
    if (t <= offset) {
        return color;
    }
    for (uint i = 1u; i < count; ++i) {
        vec4 nextColor = stopColor(inst, i);
        float nextOffset = max(stopOffset(inst, i), offset);
        if (t <= nextOffset) {
            float f = nextOffset > offset ? (t - offset) / (nextOffset - offset) : 1.0;
            return mix(color, nextColor, f);
        }
        color = nextColor;
        offset = nextOffset;
    }
    return color;
}

// Interleaved gradient noise, +-half a code value, so subtle gradients don't band in 8 bits.
vec4 dither(vec4 color) {
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    color.rgb = clamp(color.rgb + (noise - 0.5) / 255.0, vec3(0.0), vec3(color.a));
    return color;
}

vec4 paint(Instance inst, vec2 p, float scale) {
    uint kind = (inst.header >> 8) & 0xfu;
    if (kind == PAINT_SOLID) {
        return premultiply(unpackUnorm4x8(inst.colors[0]));
    }
    if (kind == PAINT_STRIPES) {
        float angle = inst.params.x;
        float width = inst.params.y;
        float period = inst.params.z;
        float s = mod(dot(p, vec2(cos(angle), sin(angle))) - inst.params.w, period);
        // Distance inside the stripe starting at 0 or the one starting at period.
        float inside = max(min(s, width - s), min(s - period, period + width - s));
        return premultiply(unpackUnorm4x8(inst.colors[0])) * clamp(0.5 + inside * scale, 0.0, 1.0);
    }
    float t;
    if (kind == PAINT_LINEAR) {
        vec2 axis = inst.params.zw - inst.params.xy;
        t = dot(p - inst.params.xy, axis) / max(dot(axis, axis), 1e-12);
    } else if (kind == PAINT_RADIAL) {
        t = length(p - inst.params.xy) / max(inst.params.z, 1e-6);
    } else {
        vec2 d = p - inst.params.xy;
        // Clockwise on screen, since y points down.
        t = fract((atan(d.y, d.x) - inst.params.z) / (2.0 * PI));
    }
    return dither(gradient(inst, t));
}

// Evan Wallace's closed-form blurred rounded rect: exact along x, 6 samples along y.
float gaussian(float x, float sigma) {
    return exp(-(x * x) / (2.0 * sigma * sigma)) / (2.5066282746 * sigma);
}

vec2 erf2(vec2 x) {
    vec2 s = sign(x);
    vec2 a = abs(x);
    x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    x *= x;
    return s - s / (x * x);
}

float shadowRow(float x, float y, float sigma, float corner, vec2 halfSize) {
    float delta = min(halfSize.y - corner - abs(y), 0.0);
    float curved = halfSize.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    vec2 integral = 0.5 + 0.5 * erf2((x + vec2(-curved, curved)) * (sqrt(0.5) / sigma));
    return integral.y - integral.x;
}

// How much of a rounded rect blurred with sigma covers p; everything in local units.
float blurredCoverage(vec2 p, vec4 rect, vec4 radii, float sigma, float scale) {
    if (rect.z <= 0.0 || rect.w <= 0.0) {
        return 0.0;
    }
    if (sigma * scale < 0.25) {
        return coverage(sdRoundRect(p, rect, radii), scale);
    }
    vec2 halfSize = rect.zw * 0.5;
    vec2 q = p - (rect.xy + halfSize);
    float corner = q.x > 0.0 ? (q.y > 0.0 ? radii.z : radii.y) : (q.y > 0.0 ? radii.w : radii.x);
    corner = min(corner, min(halfSize.x, halfSize.y));
    float low = q.y - halfSize.y;
    float high = q.y + halfSize.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end = clamp(3.0 * sigma, low, high);
    float step = (end - start) / 6.0;
    float y = start + step * 0.5;
    float value = 0.0;
    for (int i = 0; i < 6; ++i) {
        value += shadowRow(q.x, q.y - y, sigma, corner, halfSize) * gaussian(y, sigma) * step;
        y += step;
    }
    return value;
}

// CSS saturate(), applied to premultiplied color (the matrix rows sum to one).
vec4 saturate(vec4 c, float s) {
    mat3 m = mat3(0.213 + 0.787 * s, 0.213 - 0.213 * s, 0.213 - 0.213 * s,
                  0.715 - 0.715 * s, 0.715 + 0.285 * s, 0.715 - 0.715 * s,
                  0.072 - 0.072 * s, 0.072 - 0.072 * s, 0.072 + 0.928 * s);
    return vec4(clamp(m * c.rgb, vec3(0.0), vec3(c.a)), c.a);
}

// The last dual-Kawase upsampling step, from the half-size blur result to the target.
vec4 backdropTap(vec2 pos, vec4 clampRect, vec2 invSize) {
    return textureLod(aux, clamp(pos, clampRect.xy, clampRect.zw) * invSize, 0.0);
}

vec4 backdrop(Instance inst) {
    vec2 p = vDevice * 0.5;
    float o = inst.params.y;
    vec2 invSize = inst.params.zw;
    vec4 clampRect = vec4(float(inst.offsets.x & 0xffffu), float(inst.offsets.x >> 16),
                          float(inst.offsets.y & 0xffffu), float(inst.offsets.y >> 16))
                     + vec4(0.5, 0.5, -0.5, -0.5);
    float h = 0.5 * o;
    vec4 sum = backdropTap(p + vec2(-o, 0.0), clampRect, invSize)
               + backdropTap(p + vec2(o, 0.0), clampRect, invSize)
               + backdropTap(p + vec2(0.0, -o), clampRect, invSize)
               + backdropTap(p + vec2(0.0, o), clampRect, invSize);
    sum += 2.0 * (backdropTap(p + vec2(-h, -h), clampRect, invSize)
                  + backdropTap(p + vec2(h, -h), clampRect, invSize)
                  + backdropTap(p + vec2(-h, h), clampRect, invSize)
                  + backdropTap(p + vec2(h, h), clampRect, invSize));
    return saturate(sum / 12.0, inst.params.x);
}

void main() {
    Instance inst = instances[vIndex];
    uint kind = inst.header & 0xffu;
    vec2 p = vLocal;
    float scale = vScale;
    vec4 color;

    if (kind == KIND_FILL) {
        color = paint(inst, p, scale) * coverage(sdRoundRect(p, inst.rect, inst.radii), scale);
    } else if (kind == KIND_STROKE) {
        // Inside-aligned, like CSS borders: the inner edge's radii shrink by the width.
        float w = inst.extra.x;
        float outer = coverage(sdRoundRect(p, inst.rect, inst.radii), scale);
        vec4 innerRect = vec4(inst.rect.xy + w, inst.rect.zw - 2.0 * w);
        float inner = innerRect.z > 0.0 && innerRect.w > 0.0
                ? coverage(sdRoundRect(p, innerRect, max(inst.radii - w, vec4(0.0))), scale)
                : 0.0;
        color = paint(inst, p, scale) * max(outer - inner, 0.0);
    } else if (kind == KIND_DROP_SHADOW) {
        // CSS box-shadow: the shape offset and spread, blurred, and only outside the shape.
        float spread = inst.params.w;
        vec4 rect = vec4(inst.rect.xy + inst.params.xy - spread, inst.rect.zw + 2.0 * spread);
        vec4 radii = mix(vec4(0.0), max(inst.radii + spread, vec4(0.0)),
                         greaterThan(inst.radii, vec4(0.0)));
        float shadow = blurredCoverage(p, rect, radii, inst.params.z, scale);
        float shape = coverage(sdRoundRect(p, inst.rect, inst.radii), scale);
        color = premultiply(unpackUnorm4x8(inst.colors[0])) * shadow * (1.0 - shape);
    } else if (kind == KIND_INSET_SHADOW) {
        // The shadow of everything outside the shape offset and shrunk, inside the shape.
        float spread = inst.params.w;
        vec4 rect = vec4(inst.rect.xy + inst.params.xy + spread, inst.rect.zw - 2.0 * spread);
        vec4 radii = max(inst.radii - spread, vec4(0.0));
        float shadow = 1.0 - blurredCoverage(p, rect, radii, inst.params.z, scale);
        float shape = coverage(sdRoundRect(p, inst.rect, inst.radii), scale);
        color = premultiply(unpackUnorm4x8(inst.colors[0])) * shadow * shape;
    } else if (kind == KIND_GLYPH || kind == KIND_COLOR_GLYPH) {
        vec2 uv = mix(inst.radii.xy, inst.radii.zw, (p - inst.rect.xy) / inst.rect.zw);
        vec4 tint = premultiply(unpackUnorm4x8(inst.colors[0]));
        if (kind == KIND_GLYPH) {
            color = tint * textureLod(alphaAtlas, vec3(uv, inst.params.x), 0.0).r;
        } else {
            color = tint * textureLod(colorAtlas, uv, 0.0);
        }
    } else if (kind == KIND_IMAGE) {
        vec2 uv = (p - inst.rect.xy) / inst.rect.zw;
        color = textureLod(aux, uv, 0.0) * inst.extra.x
                * coverage(sdRoundRect(p, inst.rect, inst.radii), scale);
    } else if (kind == KIND_BACKDROP) {
        color = backdrop(inst) * inst.extra.x
                * coverage(sdRoundRect(p, inst.rect, inst.radii), scale);
    } else {
        color = texelFetch(aux, ivec2(vDevice), 0) * inst.extra.x;
    }

    if (inst.clip != 0u) {
        color *= clipCoverage(vDevice, inst.clip);
    }
    outColor = color;
}
