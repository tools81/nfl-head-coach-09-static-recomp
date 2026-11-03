#pragma once
// GLSL -> SPIR-V through shaderc (Vulkan SDK's shaderc_combined).
#include <cstdint>
#include <string>
#include <vector>

namespace hc09vk {

enum class Stage { kVertex, kGeometry, kFragment, kCompute };

// Returns false with the compiler log in `log` on failure.
bool CompileGlsl(const std::string& source, Stage stage, const char* name, std::vector<uint32_t>& spirv, std::string* log);

}  // namespace hc09vk
