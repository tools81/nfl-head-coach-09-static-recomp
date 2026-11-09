#include "../dev_env.h"
#include "vk_shader_compile.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

// This project compiles GLSL with glslang's C interface (its release package in thirdparty/vulkan/glslang, which
// setup can download) instead of the Vulkan SDK's shaderc. Same target (Vulkan 1.2, SPIR-V 1.5) and the same
// optimization switch.
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

namespace hc09vk {

bool CompileGlsl(const std::string& source, Stage stage, const char* name, std::vector<uint32_t>& spirv, std::string* log)
{
    static std::mutex mu;
    static bool initialized = false;
    std::lock_guard<std::mutex> lk(mu);
    if (!initialized)
        initialized = glslang_initialize_process() != 0;
    // No spirv-opt by default: the driver optimizes anyway, and spirv-opt took
    // ~0.3 s per translated shader (30 of the 37 s of an in-game GPU replay)
    // for an identical image. HC09_SHADER_OPT=1 turns it on.
    static const bool optimize = [] { const char* e = Hc09Env("HC09_SHADER_OPT"); return e && strcmp(e, "0"); }();
    const glslang_stage_t kind = stage == Stage::kVertex ? GLSLANG_STAGE_VERTEX
                               : stage == Stage::kGeometry ? GLSLANG_STAGE_GEOMETRY
                               : stage == Stage::kFragment ? GLSLANG_STAGE_FRAGMENT
                               : stage == Stage::kTessControl ? GLSLANG_STAGE_TESSCONTROL
                               : stage == Stage::kTessEval ? GLSLANG_STAGE_TESSEVALUATION : GLSLANG_STAGE_COMPUTE;
    glslang_input_t input{};
    input.language = GLSLANG_SOURCE_GLSL;
    input.stage = kind;
    input.client = GLSLANG_CLIENT_VULKAN;
    input.client_version = GLSLANG_TARGET_VULKAN_1_2;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_5;
    input.code = source.c_str();
    input.default_version = 450;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.messages = GLSLANG_MSG_DEFAULT_BIT;
    input.resource = glslang_default_resource();

    glslang_shader_t* shader = glslang_shader_create(&input);
    bool ok = glslang_shader_preprocess(shader, &input) && glslang_shader_parse(shader, &input);
    std::string messages = glslang_shader_get_info_log(shader);
    glslang_program_t* program = nullptr;
    if (ok)
    {
        program = glslang_program_create();
        glslang_program_add_shader(program, shader);
        ok = glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT) != 0;
        if (!ok)
            messages += glslang_program_get_info_log(program);
    }
    if (ok)
    {
        glslang_spv_options_t options{};
        options.disable_optimizer = !optimize;
        options.optimize_performance = optimize;
        glslang_program_SPIRV_generate_with_options(program, kind, &options);
        spirv.resize(glslang_program_SPIRV_get_size(program));
        glslang_program_SPIRV_get(program, spirv.data());
        if (const char* spirvMessages = glslang_program_SPIRV_get_messages(program))
            messages += spirvMessages;
        ok = !spirv.empty();
    }
    if (log)  // empty when there is nothing to report, as shaderc's message is
        *log = messages.empty() || !name ? messages : std::string(name) + ": " + messages;
    if (program)
        glslang_program_delete(program);
    glslang_shader_delete(shader);
    return ok;
}

}  // namespace hc09vk
