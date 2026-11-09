// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#version 450

#include "blur.glsl"

// Halves the resolution: the 2x2 box under the pixel, weighted 4, plus four diagonal taps.
void main() {
    vec2 p = gl_FragCoord.xy * 2.0;
    float o = blur.offset;
    vec4 sum = tap(p) * 4.0;
    sum += tap(p + vec2(o, o)) + tap(p - vec2(o, o)) + tap(p + vec2(o, -o)) + tap(p - vec2(o, -o));
    outColor = sum / 8.0;
}
