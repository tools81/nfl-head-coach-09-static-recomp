cmake_minimum_required(VERSION 3.20)
project(HC09Recomp VERSION 1.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The installer wizard (HC09Setup.exe) contains no game code and builds without ppc/.
add_subdirectory(tools/installer)

# Recompiled PowerPC code, generated into ppc/ by XenonRecomp (never committed).
file(GLOB HC09_PPC_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/ppc/*.cpp)
if(NOT HC09_PPC_SOURCES)
    message(WARNING "ppc/ is empty: only HC09Setup is configured (run XenonRecomp on config/HC09.toml for the game)")
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "The recompiled code requires Clang (use clang-cl on Windows)")
endif()

set(SIMDE_DIR ${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp/thirdparty/simde)

add_library(HC09RecompLib STATIC ${HC09_PPC_SOURCES})
target_include_directories(HC09RecompLib PUBLIC ${CMAKE_SOURCE_DIR}/ppc ${SIMDE_DIR})
# Line tables give every recompiled sub_<address> a symbol for stack dumps.
target_compile_options(HC09RecompLib PRIVATE
    /arch:AVX
    -gline-tables-only
    /clang:-fno-strict-aliasing
    -Wno-unused-label
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function)

# Host sources, built twice: HC09.exe (the original game) and HC27.exe (the same game with the
# modern roster and coaches from tools/roster_import; see src/common/edition.h).
set(HC09_HOST_SOURCES
    src/main.cpp
    src/common/font_hd.cpp
    src/common/broadcast_state.cpp
    src/common/draft_picks.cpp
    src/common/edition.cpp
    src/common/fifth_year_option.cpp
    src/common/rookie_scale.cpp
    src/common/settings.cpp
    src/common/updates.cpp
    src/memory.cpp
    src/thread.cpp
    src/debug/hooks.cpp
    src/game_fixes.cpp
    src/debug/stack_dump.cpp
    src/gpu/command_processor.cpp
    src/gpu/d3d12_context.cpp
    src/gpu/hlsl_translator.cpp
    src/gpu/swappable_images.cpp
    src/gpu/texture_cache.cpp
    src/gpu/renderer.cpp
    src/gpu/scene_effects.cpp
    src/gpu/shared_memory.cpp
    src/gpu/shader_compiler.cpp
    src/gpu/shaders.cpp
    src/gpu/ucode.cpp
    src/kernel/format.cpp
    src/kernel/heap.cpp
    src/kernel/xboxkrnl_audio.cpp
    src/kernel/host_audio.cpp
    src/kernel/xboxkrnl_xma.cpp
    src/kernel/import_stubs.cpp
    src/kernel/objects.cpp
    src/kernel/variables.cpp
    src/kernel/vfs.cpp
    src/kernel/xboxkrnl_io.cpp
    src/kernel/xboxkrnl_memory.cpp
    src/kernel/xboxkrnl_misc.cpp
    src/kernel/xboxkrnl_rtl.cpp
    src/kernel/xboxkrnl_strings.cpp
    src/kernel/xboxkrnl_threading.cpp
    src/kernel/xboxkrnl_video.cpp
    src/kernel/xam_content.cpp
    src/kernel/xam_input.cpp
    src/kernel/xam_misc.cpp
    src/kernel/xam_net.cpp
    src/kernel/xam_notify.cpp
    src/kernel/xam_user.cpp
    src/kernel/xex.cpp)

# DXC (dxcompiler.dll and dxil.dll) from the newest Windows SDK that has them, copied next to the
# games: shaders compile up to 30 times faster than with D3DCompile, which the games use instead
# when the DLLs are missing (see src/gpu/shader_compiler.h).
set(HC09_DXC_DIR "")
if(DEFINED ENV{WindowsSdkDir})
    file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" HC09_WINDOWS_KITS)
else()
    set(HC09_WINDOWS_KITS "C:/Program Files (x86)/Windows Kits/10")
endif()
file(GLOB HC09_DXC_CANDIDATES "${HC09_WINDOWS_KITS}/bin/10.*/x64/dxcompiler.dll")
list(SORT HC09_DXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
foreach(candidate ${HC09_DXC_CANDIDATES})
    get_filename_component(candidate_dir "${candidate}" DIRECTORY)
    if(EXISTS "${candidate_dir}/dxil.dll")
        set(HC09_DXC_DIR "${candidate_dir}")
        break()
    endif()
endforeach()
if(HC09_DXC_DIR)
    message(STATUS "DXC: ${HC09_DXC_DIR}")
else()
    message(STATUS "DXC: not found in the Windows SDK; the games compile shaders with D3DCompile")
endif()

# FFmpeg (LGPL shared build in thirdparty/ffmpeg) for XMA audio decoding.
set(HC09_FFMPEG ${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg)

# The native Vulkan renderer (src/vk: from NFL Head Coach 09 for PC, MIT, see src/vk/LICENSE), built when its
# dependencies are in thirdparty/vulkan: Vulkan-Headers, volk (loads vulkan-1.dll at run time) and glslang's
# release package (GLSL to SPIR-V). Its sources compile into the games themselves: they replace recompiled
# functions (sub_<address>), which only works for objects the linker always takes.
set(HC09_VULKAN_DIR ${CMAKE_SOURCE_DIR}/thirdparty/vulkan)
# Missing ones are downloaded here at configure time (HC09_FETCH_VULKAN=OFF: never; the games then use D3D12 only).
option(HC09_FETCH_VULKAN "Download the Vulkan renderer's dependencies into thirdparty/vulkan when missing" ON)
function(hc09_fetch_vulkan_dep name url marker)
    if(EXISTS ${HC09_VULKAN_DIR}/${name}/${marker})
        return()
    endif()
    message(STATUS "Downloading ${name} (${url})")
    set(archive ${CMAKE_BINARY_DIR}/vulkan-deps/${name}.zip)
    set(staging ${CMAKE_BINARY_DIR}/vulkan-deps/${name})
    file(DOWNLOAD ${url} ${archive} STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(WARNING "Downloading ${name} failed (${status}): the Vulkan renderer is left out")
        return()
    endif()
    file(REMOVE_RECURSE ${staging})
    file(ARCHIVE_EXTRACT INPUT ${archive} DESTINATION ${staging})
    # GitHub's source archives hold one top-level folder; glslang's release package does not.
    file(GLOB entries ${staging}/*)
    list(LENGTH entries count)
    if(count EQUAL 1 AND IS_DIRECTORY "${entries}")
        set(staging ${entries})
    endif()
    file(REMOVE_RECURSE ${HC09_VULKAN_DIR}/${name})
    file(MAKE_DIRECTORY ${HC09_VULKAN_DIR})
    file(RENAME ${staging} ${HC09_VULKAN_DIR}/${name})
endfunction()
if(HC09_FETCH_VULKAN)
    set(HC09_VULKAN_TAG vulkan-sdk-1.4.350.1)
    hc09_fetch_vulkan_dep(Vulkan-Headers
        https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${HC09_VULKAN_TAG}.zip include/vulkan/vulkan.h)
    hc09_fetch_vulkan_dep(volk https://github.com/zeux/volk/archive/refs/tags/${HC09_VULKAN_TAG}.zip volk.c)
    hc09_fetch_vulkan_dep(glslang
        https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip lib/glslang.lib)
endif()
# AMD FidelityFX FSR 3.1 (FidelityFX SDK v1.1.4, MIT: its LICENSE.txt), for the Vulkan renderer's temporal upscaling
# (hc09.ini TemporalUpscaling): the SDK's signed Vulkan DLL (SHA-256 pinned) and its ffx-api headers, downloaded into
# thirdparty/fidelityfx when missing. The renderer loads the DLL at run time; without it, temporal upscaling is off.
set(HC09_FFX_DIR ${CMAKE_SOURCE_DIR}/thirdparty/fidelityfx)
set(HC09_FFX_DLL_SHA256 a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78)
if(HC09_FETCH_VULKAN AND NOT EXISTS ${HC09_FFX_DIR}/amd_fidelityfx_vk.dll)
    set(base https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4)
    message(STATUS "Downloading AMD FidelityFX FSR 3.1 (${base})")
    set(ok TRUE)
    foreach(file ffx-api/include/ffx_api/ffx_api.h ffx-api/include/ffx_api/ffx_api_types.h ffx-api/include/ffx_api/ffx_upscale.h
                 ffx-api/include/ffx_api/vk/ffx_api_vk.h LICENSE.txt PrebuiltSignedDLL/amd_fidelityfx_vk.dll)
        string(REGEX REPLACE "^ffx-api/|^PrebuiltSignedDLL/" "" dest ${file})
        if(file MATCHES "\\.dll$")
            file(DOWNLOAD ${base}/${file} ${HC09_FFX_DIR}/${dest} STATUS status TLS_VERIFY ON EXPECTED_HASH SHA256=${HC09_FFX_DLL_SHA256})
        else()
            file(DOWNLOAD ${base}/${file} ${HC09_FFX_DIR}/${dest} STATUS status TLS_VERIFY ON)
        endif()
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            set(ok FALSE)
        endif()
    endforeach()
    if(NOT ok)
        message(WARNING "Downloading FSR 3.1 failed: temporal upscaling is left out")
        file(REMOVE ${HC09_FFX_DIR}/amd_fidelityfx_vk.dll)
    endif()
endif()

if(EXISTS ${HC09_VULKAN_DIR}/Vulkan-Headers/include/vulkan/vulkan.h AND EXISTS ${HC09_VULKAN_DIR}/volk/volk.c
        AND EXISTS ${HC09_VULKAN_DIR}/glslang/lib/glslang.lib)
    set(HC09_VULKAN ON)
    message(STATUS "Vulkan renderer: on (${HC09_VULKAN_DIR})")
    add_library(HC09Volk STATIC ${HC09_VULKAN_DIR}/volk/volk.c)
    target_include_directories(HC09Volk PUBLIC ${HC09_VULKAN_DIR}/Vulkan-Headers/include ${HC09_VULKAN_DIR}/volk)
    target_compile_definitions(HC09Volk PUBLIC VK_NO_PROTOTYPES PRIVATE VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX)
    file(GLOB HC09_VK_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/vk/*.cpp ${CMAKE_SOURCE_DIR}/src/vk/xenos/*.cpp
        ${CMAKE_SOURCE_DIR}/src/vk/gpu_vk/*.cpp)
    set(HC09_GLSLANG_LIBS)
    foreach(lib glslang glslang-default-resource-limits SPIRV MachineIndependent GenericCodeGen OSDependent SPIRV-Tools-opt SPIRV-Tools)
        list(APPEND HC09_GLSLANG_LIBS ${HC09_VULKAN_DIR}/glslang/lib/${lib}.lib)
    endforeach()
else()
    set(HC09_VULKAN OFF)
    message(STATUS "Vulkan renderer: off (thirdparty/vulkan is missing; the games use the D3D12 renderer)")
endif()

foreach(game HC09 HC27)
    add_executable(${game} ${HC09_HOST_SOURCES})
    target_include_directories(${game} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp/thirdparty/xxHash
        ${HC09_FFMPEG}/include)
    target_compile_definitions(${game} PRIVATE HC09_VERSION="${PROJECT_VERSION}" NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${game} PRIVATE /arch:AVX /clang:-fno-strict-aliasing -gline-tables-only)
    target_link_options(${game} PRIVATE /DEBUG)
    target_link_libraries(${game} PRIVATE HC09RecompLib dbghelp xinput d3d12 dxgi d3dcompiler ole32 windowscodecs winmm winhttp shell32
        ${HC09_FFMPEG}/lib/avcodec.lib ${HC09_FFMPEG}/lib/avutil.lib)
    add_custom_command(TARGET ${game} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${HC09_FFMPEG}/bin/avcodec-62.dll ${HC09_FFMPEG}/bin/avutil-60.dll ${HC09_FFMPEG}/bin/swresample-6.dll
            $<TARGET_FILE_DIR:${game}>)
    if(HC09_VULKAN)
        target_sources(${game} PRIVATE ${HC09_VK_SOURCES})
        target_include_directories(${game} PRIVATE ${CMAKE_SOURCE_DIR}/src/vk ${CMAKE_SOURCE_DIR}/src/vk/xenos
            ${CMAKE_SOURCE_DIR}/src/vk/gpu_vk ${HC09_VULKAN_DIR}/glslang/include ${CMAKE_SOURCE_DIR}/thirdparty/stb)
        target_compile_definitions(${game} PRIVATE HC09_HAVE_VULKAN)
        target_link_libraries(${game} PRIVATE HC09Volk ${HC09_GLSLANG_LIBS})
        if(EXISTS ${HC09_FFX_DIR}/amd_fidelityfx_vk.dll)
            # FSR 3.1: its headers, and its DLL next to the game (loaded at run time).
            target_include_directories(${game} PRIVATE ${HC09_FFX_DIR}/include)
            target_compile_definitions(${game} PRIVATE HC09_HAVE_FFX)
            add_custom_command(TARGET ${game} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HC09_FFX_DIR}/amd_fidelityfx_vk.dll $<TARGET_FILE_DIR:${game}>)
        endif()
    endif()
    if(HC09_DXC_DIR)
        add_custom_command(TARGET ${game} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HC09_DXC_DIR}/dxcompiler.dll" "${HC09_DXC_DIR}/dxil.dll"
                $<TARGET_FILE_DIR:${game}>)
    endif()
endforeach()
target_compile_definitions(HC27 PRIVATE HC09_EDITION_HC27)

# Standalone Xenos shader disassembler for dumped microcode (HC09_DUMP_SHADERS).
add_executable(XenosDisasm tools/xenos_disasm.cpp src/gpu/ucode.cpp src/gpu/hlsl_translator.cpp)
target_link_libraries(XenosDisasm PRIVATE d3dcompiler)
target_include_directories(XenosDisasm PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The HD texture pack maker (hc09.ini HDTextures=on; the game starts it when it closes).
add_executable(HC09HDPack tools/hdpack/hdpack.cpp)
target_compile_definitions(HC09HDPack PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(HC09HDPack PRIVATE windowscodecs ole32 bcrypt)

# XMA decoder experiment tool (decodes HC09_XMA_DUMP output under several configurations).
add_executable(XmaTest tools/xma_test.cpp)
target_include_directories(XmaTest PRIVATE ${HC09_FFMPEG}/include)
target_link_libraries(XmaTest PRIVATE ${HC09_FFMPEG}/lib/avcodec.lib ${HC09_FFMPEG}/lib/avutil.lib)
