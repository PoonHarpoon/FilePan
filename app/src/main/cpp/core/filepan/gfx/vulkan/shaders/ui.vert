// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#version 450

#include "common.glsl"

// Each instance is a quad, drawn as a 4-vertex strip without vertex buffers.
layout(location = 0) out vec2 vLocal;
layout(location = 1) flat out uint vIndex;
layout(location = 2) flat out float vScale;
// Target pixels, for what fragments look up by position (clips, layers, backdrops): what
// gl_FragCoord.xy is, but in the target's logical orientation however its image is rotated.
layout(location = 3) out vec2 vDevice;

// The image's clip space from the frame's. Turning content a quarter clockwise takes the frame's
// (x, y) to (height - y, x) in the image, which is (-y, x) in clip coordinates.
vec2 toImage(vec2 n) {
    if (pc.transform == 1u) {
        return vec2(-n.y, n.x);
    }
    if (pc.transform == 2u) {
        return -n;
    }
    if (pc.transform == 3u) {
        return vec2(n.y, -n.x);
    }
    return n;
}

void main() {
    Instance inst = instances[gl_InstanceIndex];
    uint kind = inst.header & 0xffu;
    float scale = sqrt(abs(inst.transform.x * inst.transform.w - inst.transform.y * inst.transform.z));
    // One pixel of margin for antialiasing, in local units.
    float aa = 1.0 / max(scale, 1e-6);

    vec2 lo = inst.rect.xy;
    vec2 hi = inst.rect.xy + inst.rect.zw;
    if (kind == KIND_DROP_SHADOW) {
        // Offset, then grow by the spread and three standard deviations of blur.
        float grow = inst.params.w + 3.0 * inst.params.z + aa;
        lo += inst.params.xy - grow;
        hi += inst.params.xy + grow;
    } else if (kind != KIND_GLYPH && kind != KIND_COLOR_GLYPH && kind != KIND_LAYER) {
        lo -= aa;
        hi += aa;
    }

    vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec2 local = mix(lo, hi, corner);
    vec2 device = mat2(inst.transform.xy, inst.transform.zw) * local + inst.translate;
    gl_Position = vec4(toImage(device / pc.targetSize * 2.0 - 1.0), 0.0, 1.0);
    vLocal = local;
    vIndex = uint(gl_InstanceIndex);
    vScale = scale;
    vDevice = device;
}
