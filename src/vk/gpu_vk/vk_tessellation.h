#pragma once
// hc09: smoother player models (roadmap 13). The players' low-poly silhouettes are rounded off by Phong tessellation
// between the game's own vertex and pixel shaders: each triangle is split on the GPU, and the new points are pulled
// toward the tangent planes of its corners, so curved surfaces (helmets first) are drawn curved. No new art: the
// shape comes from the model's own vertex normals. See vk_tessellation.cpp.
#include <cstdint>
#include <string>

namespace hc09vk {

// The draws of these vertex shaders are tessellated (when the device can and HC09_TESS / hc09.ini allow).
bool TessellatedVertexShader(uint64_t vsHash);

// The three shaders a tessellated draw uses in place of the translated vertex shader `vsGlsl`: the vertex shader
// itself, ending at the guest's clip position (passed on, not transformed to the window); the control shader, which
// splits each edge by its length on the screen; the evaluation shader, which places the new points and does the
// vertex shader's window transform. False if `vsGlsl` is not as expected.
bool TessellationShaders(uint64_t vsHash, const std::string& vsGlsl, std::string& vs, std::string& tcs, std::string& tes);

}  // namespace hc09vk
