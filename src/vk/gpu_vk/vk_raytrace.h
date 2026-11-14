#pragma once
// hc09: ray-traced lighting (roadmap 14): ambient occlusion and contact shadows traced against the 3D scene itself.
//
// The game skins and places its models in its vertex shaders, so the scene's geometry exists only on the GPU, once
// per frame. Each opaque 3D draw of the scene's targets also writes its vertices' clip-space positions to a capture
// buffer (the translated vertex shader, xe_glsl.cpp XE_CAPTURE; DrawConsts clip.y/z). At the scene's first colour
// resolve, a compute pass takes them to view space with the camera's projection (every draw of the scene shares it,
// gpu/scene_camera.h), the same space the screen-space effects rebuild from the depth buffer; then one bottom-level
// acceleration structure is built from all of the frame's draws (one geometry each, their triangle lists rebased to
// their own vertices) under a one-instance top level. The effects' occlusion pass (vk_scene_effects.cpp) traces rays
// against it with ray queries.
//
// Settings: hc09.ini [Graphics] RayTracing=on (off by default), as HC09_RAY_TRACING. Needs VK_KHR_ray_query (the
// device turns it on; Device::rayQuery). Without it, the screen-space occlusion stays.
#include <cstdint>
#include <vector>

#include "gpu/scene_camera.h"
#include "vk_device.h"

namespace hc09vk {

class SceneRayTracer
{
public:
    // The setting (HC09_RAY_TRACING).
    static bool Wanted();
    bool Init(Device& dev);
    bool Ready() const { return ready_; }
    void Destroy();

    // The capture buffer (the draws' descriptor binding 9): vec4 per vertex.
    VkBuffer CaptureBuffer() const { return capture_.buffer; }
    // A draw of this frame's 3D scene, its triangle list `indices` (`count` values, the vertex shader's
    // gl_VertexIndex values): room for its vertices in the capture buffer. `base`: where its vertex shader writes
    // vertex i (base + i). False: no room left this frame (the draw is not captured).
    // `glossy`: a mirror-like surface (the helmets' shells): its triangles are the scene's last (GlossyFrom).
    bool Add(uint64_t frame, const uint32_t* indices, uint32_t count, int32_t& base, bool glossy = false);
    // After Build: the scene's triangles from this one on are glossy (none: the triangle count).
    uint32_t GlossyFrom() const { return indexCursor_ / 3; }
    bool HasGlossy() const { return glossyCursor_ != 0; }
    // At the scene's colour resolve (outside rendering): this frame's captured draws to view space with the camera's
    // projection (`squeeze`: widescreen's factor on the effects' clip x, gpu/scene_camera.h), then built. False:
    // nothing to trace this frame.
    bool Build(VkCommandBuffer cb, uint64_t frame, const hc09::gpu::SceneCamera& camera, float squeeze);
    VkAccelerationStructureKHR Scene() const { return tlas_; }

private:
    struct Geometry
    {
        uint32_t firstVertex, vertexCount, firstIndex, indexCount;
    };
    struct Retired
    {
        uint64_t frame;
        Buffer buffer;
        VkAccelerationStructureKHR as;
    };
    bool Grow(Buffer& buffer, VkAccelerationStructureKHR* as, VkDeviceSize size, VkBufferUsageFlags usage, VkAccelerationStructureTypeKHR type);
    VkDeviceAddress Address(const Buffer& b) const;

    static constexpr uint32_t kRing = 4;               // frames whose indices may still be read by the GPU
    static constexpr uint32_t kMaxVertices = 4u << 20;  // a frame's captured vertices
    static constexpr uint32_t kMaxIndices = 6u << 20;   // and indices

    Device* dev_ = nullptr;
    bool ready_ = false;
    Buffer capture_;                  // device-local: clip positions, then view positions
    Buffer indices_;                  // host-visible: kRing regions of kMaxIndices rebased indices
    Buffer deviceIndices_;            // device-local: the frame's, copied for the build
    Buffer instances_;                // host-visible: kRing top-level instances
    Buffer blasBuffer_, tlasBuffer_, scratch_;
    VkAccelerationStructureKHR blas_ = VK_NULL_HANDLE, tlas_ = VK_NULL_HANDLE;
    std::vector<Retired> retired_;
    VkPipelineLayout convertLayout_ = VK_NULL_HANDLE;
    VkPipeline convert_ = VK_NULL_HANDLE;
    VkDeviceSize scratchAlign_ = 256;

    uint64_t frame_ = ~0ull;
    uint32_t vertexCursor_ = 0, indexCursor_ = 0;
    uint32_t glossyCursor_ = 0;       // the glossy draws' indices, kept at the end of the frame's region
    uint32_t builtGlossy_ = 0;
    std::vector<Geometry> geometries_;
    uint64_t logged_ = 0;
    // Refits: a frame whose index stream is the last built one's (its hash and counts) updates the bottom level in place
    // instead of building it again (the players and camera move; their triangles stay); rebuilt every kRefits frames.
    static constexpr uint32_t kRefits = 30;
    uint64_t hash_ = 0, builtHash_ = 0;
    uint32_t builtVertices_ = 0, builtIndices_ = 0, refits_ = 0;
    bool built_ = false;
    // HC09_RAY_TRACING_PROF=1 (dev): GPU timestamps around the build's steps, averaged in the log.
    VkQueryPool timing_ = VK_NULL_HANDLE;
    uint64_t timedFrame_[kRing] = {};
    double timeSum_[3] = {};
    uint32_t timeCount_ = 0;
};

}  // namespace hc09vk
