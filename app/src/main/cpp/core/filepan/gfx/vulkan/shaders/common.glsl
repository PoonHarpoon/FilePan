// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Kept in sync with GpuInstance and GpuClip in frame_plan.hpp.

#define KIND_FILL 0u
#define KIND_STROKE 1u
#define KIND_DROP_SHADOW 2u
#define KIND_INSET_SHADOW 3u
#define KIND_GLYPH 4u
#define KIND_COLOR_GLYPH 5u
#define KIND_IMAGE 6u
#define KIND_BACKDROP 7u
#define KIND_LAYER 8u

#define PAINT_SOLID 0u
#define PAINT_LINEAR 1u
#define PAINT_RADIAL 2u
#define PAINT_CONIC 3u
#define PAINT_STRIPES 4u

struct Instance {
    vec4 rect;
    vec4 radii;
    vec4 transform;
    vec2 translate;
    uint header;
    uint clip;
    vec4 params;
    uvec4 colors;
    uvec2 offsets;
    vec2 extra;
};

struct Clip {
    vec4 inverse;
    vec2 inverseTranslate;
    float scale;
    uint parent;
    vec4 rect;
    vec4 radii;
};

layout(std430, set = 0, binding = 0) readonly buffer Instances {
    Instance instances[];
};

layout(std430, set = 0, binding = 1) readonly buffer Clips {
    Clip clips[];
};

layout(push_constant) uniform PushConstants {
    vec2 targetSize;  // Logical px.
    // Quarter turns clockwise the image holds the frame rotated by: a pre-rotated swapchain image.
    uint transform;
} pc;
