// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Shared by the dual-Kawase passes. Positions are in source texels; taps are clamped to the
// region being blurred, which acts like clamp-to-edge for that region.

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform BlurConstants {
    vec4 clampRect;  // Source texels: min x, min y, max x, max y, inset by half a texel.
    vec2 invSourceSize;
    float offset;
} blur;

layout(location = 0) out vec4 outColor;

vec4 tap(vec2 pos) {
    return textureLod(source, clamp(pos, blur.clampRect.xy, blur.clampRect.zw) * blur.invSourceSize,
                      0.0);
}
