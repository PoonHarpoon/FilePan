// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#version 450

#include "blur.glsl"

// Doubles the resolution with eight taps in a diamond; ui.frag does the same for the last step.
void main() {
    vec2 p = gl_FragCoord.xy * 0.5;
    float o = blur.offset;
    float h = 0.5 * o;
    vec4 sum = tap(p + vec2(-o, 0.0)) + tap(p + vec2(o, 0.0)) + tap(p + vec2(0.0, -o))
               + tap(p + vec2(0.0, o));
    sum += 2.0 * (tap(p + vec2(-h, -h)) + tap(p + vec2(h, -h)) + tap(p + vec2(-h, h))
                  + tap(p + vec2(h, h)));
    outColor = sum / 12.0;
}
