// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Rounded-rect coverage and clipping, shared by the fragment shaders. Needs common.glsl.

// Signed distance to a rounded rect (x, y, width, height) with radii (tl, tr, br, bl), y down.
float sdRoundRect(vec2 p, vec4 rect, vec4 radii) {
    vec2 halfSize = rect.zw * 0.5;
    p -= rect.xy + halfSize;
    float r = p.x > 0.0 ? (p.y > 0.0 ? radii.z : radii.y) : (p.y > 0.0 ? radii.w : radii.x);
    vec2 q = abs(p) - halfSize + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Pixel coverage from a local-space distance; scale is local units to pixels.
float coverage(float d, float scale) {
    return clamp(0.5 - d * scale, 0.0, 1.0);
}

float clipCoverage(vec2 device, uint index) {
    float result = 1.0;
    for (int n = 0; n < 64 && index != 0u; ++n) {
        Clip c = clips[index];
        vec2 p = mat2(c.inverse.xy, c.inverse.zw) * device + c.inverseTranslate;
        result *= coverage(sdRoundRect(p, c.rect, c.radii), c.scale);
        index = c.parent;
    }
    return result;
}
