// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#version 450

#include "common.glsl"
#include "shapes.glsl"

// External images (video frames), drawn with ui.vert as KIND_IMAGE instances. The texture may
// carry an immutable YCbCr sampler, which only allows plain sampling: no texel fetches, gathers
// or offsets. params is the part of the texture to show (u0, v0, u1, v1). Texels are
// premultiplied, like images; video converts to opaque gamma-encoded RGB, which is treated as
// sRGB like everything else here.

layout(set = 1, binding = 0) uniform sampler2D frame;

layout(location = 0) in vec2 vLocal;
layout(location = 1) flat in uint vIndex;
layout(location = 2) flat in float vScale;
layout(location = 3) in vec2 vDevice;

layout(location = 0) out vec4 outColor;

void main() {
    Instance inst = instances[vIndex];
    vec2 p = vLocal;
    vec2 t = clamp((p - inst.rect.xy) / inst.rect.zw, 0.0, 1.0);
    vec2 uv = mix(inst.params.xy, inst.params.zw, t);
    vec4 color = textureLod(frame, uv, 0.0) * inst.extra.x
                 * coverage(sdRoundRect(p, inst.rect, inst.radii), vScale);
    if (inst.clip != 0u) {
        color *= clipCoverage(vDevice, inst.clip);
    }
    outColor = color;
}
