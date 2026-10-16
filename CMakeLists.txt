cmake_minimum_required(VERSION 3.20)
project(HC09Recomp VERSION 0.6.1 LANGUAGES C CXX)

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

add_executable(HC09
    src/main.cpp
    src/common/settings.cpp
    src/common/updates.cpp
    src/memory.cpp
    src/thread.cpp
    src/debug/hooks.cpp
    src/debug/stack_dump.cpp
    src/gpu/command_processor.cpp
    src/gpu/d3d12_context.cpp
    src/gpu/hlsl_translator.cpp
    src/gpu/texture_cache.cpp
    src/gpu/renderer.cpp
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
target_include_directories(HC09 PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp/thirdparty/xxHash)
target_compile_definitions(HC09 PRIVATE HC09_VERSION="${PROJECT_VERSION}" NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(HC09 PRIVATE /arch:AVX /clang:-fno-strict-aliasing -gline-tables-only)
target_link_options(HC09 PRIVATE /DEBUG)
target_link_libraries(HC09 PRIVATE HC09RecompLib dbghelp xinput d3d12 dxgi d3dcompiler ole32 winmm winhttp shell32)

# FFmpeg (LGPL shared build in thirdparty/ffmpeg) for XMA audio decoding.
set(HC09_FFMPEG ${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg)
target_include_directories(HC09 PRIVATE ${HC09_FFMPEG}/include)
target_link_libraries(HC09 PRIVATE ${HC09_FFMPEG}/lib/avcodec.lib ${HC09_FFMPEG}/lib/avutil.lib)
add_custom_command(TARGET HC09 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${HC09_FFMPEG}/bin/avcodec-62.dll ${HC09_FFMPEG}/bin/avutil-60.dll ${HC09_FFMPEG}/bin/swresample-6.dll
        $<TARGET_FILE_DIR:HC09>)

# Standalone Xenos shader disassembler for dumped microcode (HC09_DUMP_SHADERS).
add_executable(XenosDisasm tools/xenos_disasm.cpp src/gpu/ucode.cpp src/gpu/hlsl_translator.cpp)
target_link_libraries(XenosDisasm PRIVATE d3dcompiler)
target_include_directories(XenosDisasm PRIVATE ${CMAKE_SOURCE_DIR}/src)

# XMA decoder experiment tool (decodes HC09_XMA_DUMP output under several configurations).
add_executable(XmaTest tools/xma_test.cpp)
target_include_directories(XmaTest PRIVATE ${HC09_FFMPEG}/include)
target_link_libraries(XmaTest PRIVATE ${HC09_FFMPEG}/lib/avcodec.lib ${HC09_FFMPEG}/lib/avutil.lib)
