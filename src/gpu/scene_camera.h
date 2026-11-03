#pragma once

// The camera of a 3D scene, for the screen-space effects of both renderers (gpu/scene_effects.h,
// vk/gpu_vk/vk_scene_effects.h).
//
// The scene's vertex shaders transform world positions by a matrix in vertex constants c156..c159
// (the view-projection, or an object's world-view-projection). Every such matrix shares the
// projection, which CameraFromConstants recovers: how depth maps back to distance, the field of
// view, and the world's up direction in view space.

namespace hc09::gpu
{
    struct SceneCamera
    {
        bool valid = false;
        // Depth buffer value d -> view distance w: w = depthB / (d - depthA).
        float depthA = 0, depthB = 0;
        // Clip x = sx * view x + cx * w (and y): the projection's scale and centre.
        float sx = 0, sy = 0, cx = 0, cy = 0;
        float up[3]{};  // world up in view space (x right, y up, z forward)
    };

    // The camera from the four float4 constants c156..c159 (host byte order), each the clip-space
    // image of a world axis (x, y, z) and of the origin. Invalid unless they form a perspective
    // projection.
    SceneCamera CameraFromConstants(const float* constants);
}
