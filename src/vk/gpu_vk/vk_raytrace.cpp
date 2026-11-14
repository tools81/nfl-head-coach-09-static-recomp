// hc09: ray-traced lighting; see vk_raytrace.h.

#include "vk_raytrace.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "../dev_env.h"
#include "vk_shader_compile.h"

namespace hc09vk {

namespace {
// The captured clip positions (x, y, -, w; the draws' own clip space, before widescreen's squeeze) to view space
// (x right, y up, z forward: w), in place.
const char* kConvert = R"GLSL(#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(local_size_x = 256) in;
layout(buffer_reference, std430, buffer_reference_align = 16) buffer Positions { vec4 p[]; };
layout(push_constant) uniform Convert { uint64_t positions; uint count; float squeeze; vec4 camera; } C;   // camera: sx, sy, cx, cy
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= C.count) return;
    Positions buf = Positions(C.positions);
    vec4 clip = buf.p[i];
    float w = clip.w;
    vec3 view = vec3((clip.x * C.squeeze - C.camera.z * w) / C.camera.x, (clip.y - C.camera.w * w) / C.camera.y, w);
    if (any(isnan(view)) || any(isinf(view))) view = vec3(uintBitsToFloat(0x7FC00000u));   // NaN x: an inactive triangle
    buf.p[i] = vec4(view, 1.0);
}
)GLSL";

void Barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess)
{
    VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}
}  // namespace

bool SceneRayTracer::Wanted()
{
    static const bool on = [] {
        const char* e = Hc09Env("HC09_RAY_TRACING");
        return e && *e && std::strcmp(e, "0") != 0;
    }();
    return on;
}

VkDeviceAddress SceneRayTracer::Address(const Buffer& b) const
{
    VkBufferDeviceAddressInfo ai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    ai.buffer = b.buffer;
    return vkGetBufferDeviceAddress(dev_->device, &ai);
}

bool SceneRayTracer::Init(Device& dev)
{
    dev_ = &dev;
    if (!Wanted() || !dev.rayQuery || !vkCreateAccelerationStructureKHR || !vkCmdBuildAccelerationStructuresKHR) return false;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR ap{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &ap;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    scratchAlign_ = std::max<VkDeviceSize>(ap.minAccelerationStructureScratchOffsetAlignment, 16);
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkBufferUsageFlags input = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (!dev.CreateBuffer(VkDeviceSize(kMaxVertices) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | input, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, capture_) ||
        !dev.CreateBuffer(VkDeviceSize(kMaxIndices) * 4 * kRing, input | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hv, indices_) ||
        !dev.CreateBuffer(VkDeviceSize(kMaxIndices) * 4, input | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          deviceIndices_) ||
        !dev.CreateBuffer(sizeof(VkAccelerationStructureInstanceKHR) * kRing, input, hv, instances_))
    {
        std::fprintf(stderr, "[raytrace] buffers could not be made: ray tracing off\n");
        Destroy();
        return false;
    }
    std::vector<uint32_t> spirv;
    std::string log;
    if (!CompileGlsl(kConvert, Stage::kCompute, "raytrace_convert", spirv, &log))
    {
        std::fprintf(stderr, "[raytrace] %s\n", log.c_str());
        Destroy();
        return false;
    }
    VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = spirv.size() * 4;
    mi.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev.device, &mi, nullptr, &m);
    VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(dev.device, &pl, nullptr, &convertLayout_);
    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main" };
    ci.layout = convertLayout_;
    if (!m || !convertLayout_ || vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &convert_) != VK_SUCCESS)
    {
        if (m) vkDestroyShaderModule(dev.device, m, nullptr);
        std::fprintf(stderr, "[raytrace] the conversion pass could not be made: ray tracing off\n");
        Destroy();
        return false;
    }
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (Hc09Env("HC09_RAY_TRACING_PROF"))
    {
        VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 4 * kRing;
        vkCreateQueryPool(dev.device, &qi, nullptr, &timing_);
    }
    ready_ = true;
    std::fprintf(stderr, "[raytrace] ray-traced occlusion on (up to %u vertices and %u indices a frame)\n", kMaxVertices, kMaxIndices);
    return true;
}

void SceneRayTracer::Destroy()
{
    if (!dev_ || !dev_->device) return;
    VkDevice d = dev_->device;
    for (Retired& r : retired_)
    {
        if (r.as) vkDestroyAccelerationStructureKHR(d, r.as, nullptr);
        dev_->DestroyBuffer(r.buffer);
    }
    retired_.clear();
    if (blas_) vkDestroyAccelerationStructureKHR(d, blas_, nullptr);
    if (tlas_) vkDestroyAccelerationStructureKHR(d, tlas_, nullptr);
    blas_ = tlas_ = VK_NULL_HANDLE;
    for (Buffer* b : { &capture_, &indices_, &deviceIndices_, &instances_, &blasBuffer_, &tlasBuffer_, &scratch_ })
        if (b->buffer) dev_->DestroyBuffer(*b);
    if (timing_) vkDestroyQueryPool(d, timing_, nullptr);
    timing_ = VK_NULL_HANDLE;
    if (convert_) vkDestroyPipeline(d, convert_, nullptr);
    if (convertLayout_) vkDestroyPipelineLayout(d, convertLayout_, nullptr);
    convert_ = VK_NULL_HANDLE;
    convertLayout_ = VK_NULL_HANDLE;
    ready_ = false;
}

bool SceneRayTracer::Add(uint64_t frame, const uint32_t* indices, uint32_t count, int32_t& base, bool glossy)
{
    if (!ready_ || !count) return false;
    if (frame != frame_)
    {
        frame_ = frame;
        vertexCursor_ = indexCursor_ = glossyCursor_ = 0;
        geometries_.clear();
        hash_ = 1469598103934665603ull;
    }
    uint32_t lo = indices[0], hi = indices[0];
    for (uint32_t i = 1; i < count; ++i)
    {
        lo = std::min(lo, indices[i]);
        hi = std::max(hi, indices[i]);
    }
    const uint32_t vertices = hi - lo + 1;
    if (vertices > kMaxVertices - vertexCursor_ || count > kMaxIndices - indexCursor_ - glossyCursor_) return false;
    // The glossy draws' from the region's end back: the build takes them after the others'.
    uint32_t* out = static_cast<uint32_t*>(indices_.mapped) + size_t(frame % kRing) * kMaxIndices +
                    (glossy ? kMaxIndices - glossyCursor_ - count : indexCursor_);
    const uint32_t shift = vertexCursor_ - lo;   // the frame's vertices are one array: one geometry builds fastest
    uint64_t h = (hash_ ^ (glossy ? 0x9E3779B9u : 0u)) * 1099511628211ull;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t v = indices[i] + shift;
        out[i] = v;
        h = (h ^ v) * 1099511628211ull;
    }
    hash_ = h;
    geometries_.push_back({ vertexCursor_, vertices, indexCursor_, count });
    base = int32_t(vertexCursor_) - int32_t(lo);
    vertexCursor_ += vertices;
    (glossy ? glossyCursor_ : indexCursor_) += count;
    return true;
}

bool SceneRayTracer::Grow(Buffer& buffer, VkAccelerationStructureKHR* as, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkAccelerationStructureTypeKHR type)
{
    if (buffer.buffer && buffer.size >= size) return true;
    // The old one may still be read by frames in flight: freed some frames on.
    if (buffer.buffer) retired_.push_back({ frame_, buffer, as ? *as : VK_NULL_HANDLE });
    buffer = Buffer{};
    if (as) *as = VK_NULL_HANDLE;
    size = size + size / 4;   // room to grow
    if (!dev_->CreateBuffer(size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer)) return false;
    if (as)
    {
        VkAccelerationStructureCreateInfoKHR ci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        ci.buffer = buffer.buffer;
        ci.size = size;
        ci.type = type;
        if (vkCreateAccelerationStructureKHR(dev_->device, &ci, nullptr, as) != VK_SUCCESS) return false;
    }
    return true;
}

bool SceneRayTracer::Build(VkCommandBuffer cb, uint64_t frame, const hc09::gpu::SceneCamera& camera, float squeeze)
{
    if (!ready_ || frame != frame_ || geometries_.empty() || !camera.valid) return false;
    // Retired structures no frame in flight can still use.
    for (auto it = retired_.begin(); it != retired_.end();)
        if (it->frame + 2 * kRing < frame)
        {
            if (it->as) vkDestroyAccelerationStructureKHR(dev_->device, it->as, nullptr);
            dev_->DestroyBuffer(it->buffer);
            it = retired_.erase(it);
        }
        else
            ++it;

    const uint32_t slot = uint32_t(frame % kRing);
    auto stamp = [&](uint32_t i) {
        if (timing_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, timing_, slot * 4 + i);
    };
    if (timing_)
    {
        // This slot's last timings (kRing frames ago: done by now, or skipped).
        uint64_t t[4];
        if (timedFrame_[slot] &&
            vkGetQueryPoolResults(dev_->device, timing_, slot * 4, 4, sizeof(t), t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            const double ns = dev_->props.limits.timestampPeriod * 1e-6;
            for (int i = 0; i < 3; ++i) timeSum_[i] += double(t[i + 1] - t[i]) * ns;
            if (++timeCount_ == 120)
            {
                std::fprintf(stderr, "[raytrace] GPU ms: convert %.3f, bottom level %.3f, top level %.3f\n", timeSum_[0] / timeCount_,
                             timeSum_[1] / timeCount_, timeSum_[2] / timeCount_);
                timeSum_[0] = timeSum_[1] = timeSum_[2] = 0;
                timeCount_ = 0;
            }
        }
        vkCmdResetQueryPool(cb, timing_, slot * 4, 4);
        timedFrame_[slot] = frame;
    }

    // The draws' clip positions to view space.
    Barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    stamp(0);
    struct
    {
        VkDeviceAddress positions;
        uint32_t count;
        float squeeze;
        float camera[4];
    } pc{ Address(capture_), vertexCursor_, squeeze, { camera.sx, camera.sy, camera.cx, camera.cy } };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, convert_);
    vkCmdPushConstants(cb, convertLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, (vertexCursor_ + 255) / 256, 1, 1);
    stamp(1);
    Barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    // The bottom level: the frame's draws as one geometry (their indices into one array of vertices; one geometry per
    // draw, 1700 in a frame, took several milliseconds to build).
    const VkDeviceAddress positions = Address(capture_);
    // The indices to device memory (the build reads them more than once).
    VkBufferCopy copies[2];
    uint32_t copyCount = 0;
    const VkDeviceSize region = VkDeviceSize(frame % kRing) * kMaxIndices * 4;
    if (indexCursor_) copies[copyCount++] = { region, 0, VkDeviceSize(indexCursor_) * 4 };
    if (glossyCursor_)   // the glossy draws' after the others'
        copies[copyCount++] = { region + VkDeviceSize(kMaxIndices - glossyCursor_) * 4, VkDeviceSize(indexCursor_) * 4,
                                VkDeviceSize(glossyCursor_) * 4 };
    Barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);   // last build's reads
    vkCmdCopyBuffer(cb, indices_.buffer, deviceIndices_.buffer, copyCount, copies);
    Barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    const VkDeviceAddress indexBase = Address(deviceIndices_);
    const size_t n = geometries_.size();
    std::vector<VkAccelerationStructureGeometryKHR> geometries(1);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(1);
    std::vector<uint32_t> primitives(1);
    {
        VkAccelerationStructureGeometryKHR& geo = geometries[0];
        geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& t = geo.geometry.triangles;
        t = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
        t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t.vertexData.deviceAddress = positions;
        t.vertexStride = 16;
        t.maxVertex = vertexCursor_ - 1;
        t.indexType = VK_INDEX_TYPE_UINT32;
        t.indexData.deviceAddress = indexBase;
        primitives[0] = indexCursor_ / 3 + glossyCursor_ / 3;
        ranges[0] = { primitives[0], 0, 0, 0 };
    }
    VkAccelerationStructureBuildGeometryInfoKHR bi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = geometries.data();
    VkAccelerationStructureBuildSizesInfoKHR bs{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(dev_->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, primitives.data(), &bs);

    // The top level: one instance of it.
    VkAccelerationStructureGeometryKHR top{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    top.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    top.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    top.geometry.instances = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
    top.geometry.instances.data.deviceAddress = Address(instances_) + VkDeviceAddress(frame % kRing) * sizeof(VkAccelerationStructureInstanceKHR);
    VkAccelerationStructureBuildGeometryInfoKHR ti{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    ti.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    ti.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    ti.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    ti.geometryCount = 1;
    ti.pGeometries = &top;
    const uint32_t one = 1;
    VkAccelerationStructureBuildSizesInfoKHR ts{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(dev_->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ti, &one, &ts);

    const VkDeviceSize scratchSize = std::max({ bs.buildScratchSize, bs.updateScratchSize, ts.buildScratchSize }) + scratchAlign_;
    const VkAccelerationStructureKHR oldBottom = blas_;
    if (!Grow(blasBuffer_, &blas_, bs.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
              VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR) ||
        !Grow(tlasBuffer_, &tlas_, ts.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
              VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR) ||
        !Grow(scratch_, nullptr, scratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR))
    {
        std::fprintf(stderr, "[raytrace] acceleration structure memory could not be had: ray tracing off\n");
        ready_ = false;
        return false;
    }
    const VkDeviceAddress scratch = (Address(scratch_) + scratchAlign_ - 1) & ~(scratchAlign_ - 1);

    if (blas_ != oldBottom) built_ = false;   // a new, larger structure: built afresh
    const bool refit = built_ && hash_ == builtHash_ && vertexCursor_ == builtVertices_ && indexCursor_ == builtIndices_ && glossyCursor_ == builtGlossy_ &&
                       refits_ < kRefits;
    if (refit)
    {
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        bi.srcAccelerationStructure = blas_;
        ++refits_;
    }
    else
    {
        builtHash_ = hash_;
        builtVertices_ = vertexCursor_;
        builtIndices_ = indexCursor_;
        builtGlossy_ = glossyCursor_;
        refits_ = 0;
    }
    bi.dstAccelerationStructure = blas_;
    bi.scratchData.deviceAddress = scratch;
    const VkAccelerationStructureBuildRangeInfoKHR* bottomRanges = ranges.data();
    vkCmdBuildAccelerationStructuresKHR(cb, 1, &bi, &bottomRanges);
    built_ = true;
    stamp(2);
    Barrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_SHADER_WRITE_BIT);

    VkAccelerationStructureDeviceAddressInfoKHR ai{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    ai.accelerationStructure = blas_;
    VkAccelerationStructureInstanceKHR inst{};
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
    inst.mask = 0xFF;
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(dev_->device, &ai);
    std::memcpy(static_cast<uint8_t*>(instances_.mapped) + size_t(frame % kRing) * sizeof(inst), &inst, sizeof(inst));
    ti.dstAccelerationStructure = tlas_;
    ti.scratchData.deviceAddress = scratch;
    VkAccelerationStructureBuildRangeInfoKHR topRange{ 1, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* topRanges = &topRange;
    vkCmdBuildAccelerationStructuresKHR(cb, 1, &ti, &topRanges);
    stamp(3);
    Barrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT);

    if (logged_ < 3 || frame % 600 == 0)
    {
        ++logged_;
        uint64_t triangles = 0;
        for (uint32_t p : primitives) triangles += p;
        std::fprintf(stderr, "[raytrace] frame %llu: %zu draws, %u vertices, %llu triangles, structure %llu KiB\n", (unsigned long long)frame, n,
                     vertexCursor_, (unsigned long long)triangles, (unsigned long long)(bs.accelerationStructureSize >> 10));
    }
    return true;
}

}  // namespace hc09vk
