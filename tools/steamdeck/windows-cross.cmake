# CMake toolchain for building the Windows games on Linux (Steam Deck), which then run under Proton.
#
#   cmake -S . -B build/hc09 -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=tools/steamdeck/windows-cross.cmake \
#         -DHC09_LLVM=<LLVM release folder> -DHC09_WINSYSROOT=<xwin splat folder>
#
# clang-cl and lld-link from an LLVM release; the MSVC runtime and Windows SDK headers and libraries from
# `xwin splat --use-winsysroot-style --preserve-ms-arch-notation` (xwin also adds the lower-case names that
# includes such as <windows.h> need on a case-sensitive file system). tools/steamdeck/hc09-steamdeck.sh sets it
# all up. Nothing here needs the host's own headers, which SteamOS does not have.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(HC09_LLVM "$ENV{HC09_LLVM}" CACHE PATH "LLVM release folder (bin/clang-cl, bin/lld-link, bin/llvm-rc)")
set(HC09_WINSYSROOT "$ENV{HC09_WINSYSROOT}" CACHE PATH "MSVC and Windows SDK folder in the /winsysroot layout (xwin)")
if(NOT HC09_LLVM OR NOT HC09_WINSYSROOT)
    message(FATAL_ERROR "Set HC09_LLVM and HC09_WINSYSROOT (see tools/steamdeck/windows-cross.cmake)")
endif()
# try_compile projects get the same settings.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES HC09_LLVM HC09_WINSYSROOT HC09_MT HC09_LINKER)

# (".exe" when this is tried out on Windows, with Visual Studio's LLVM.)
if(CMAKE_HOST_WIN32)
    set(HC09_EXE .exe)
else()
    set(HC09_EXE "")
endif()
set(CMAKE_C_COMPILER ${HC09_LLVM}/bin/clang-cl${HC09_EXE})
set(CMAKE_CXX_COMPILER ${HC09_LLVM}/bin/clang-cl${HC09_EXE})
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
# HC09_LINKER: another lld-link (the LLVM release's needs libxml2.so.2, which newer distributions have renamed;
# hc09-steamdeck.sh then uses the one in zig, which needs no shared libraries).
set(HC09_LINKER ${HC09_LLVM}/bin/lld-link${HC09_EXE} CACHE FILEPATH "lld-link")
set(CMAKE_LINKER ${HC09_LINKER})
set(CMAKE_AR ${HC09_LLVM}/bin/llvm-lib${HC09_EXE})
set(CMAKE_RC_COMPILER ${HC09_LLVM}/bin/llvm-rc${HC09_EXE})
# The manifest tool: llvm-mt needs libxml2, which the LLVM release may not have been built with, so the
# executables are linked without manifests (/MANIFEST:NO below) and this stand-in is never asked to merge one.
set(HC09_MT /bin/true CACHE FILEPATH "Manifest tool (never asked to merge a manifest)")
set(CMAKE_MT ${HC09_MT})

# The newest MSVC and Windows SDK versions in the sysroot.
file(GLOB HC09_MSVC_VERSIONS LIST_DIRECTORIES true "${HC09_WINSYSROOT}/VC/Tools/MSVC/*")
file(GLOB HC09_SDK_VERSIONS LIST_DIRECTORIES true "${HC09_WINSYSROOT}/Windows Kits/10/Include/*")
list(SORT HC09_MSVC_VERSIONS COMPARE NATURAL ORDER DESCENDING)
list(SORT HC09_SDK_VERSIONS COMPARE NATURAL ORDER DESCENDING)
list(GET HC09_MSVC_VERSIONS 0 HC09_MSVC_DIR)
list(GET HC09_SDK_VERSIONS 0 HC09_SDK_INCLUDE)
if(NOT EXISTS "${HC09_MSVC_DIR}/include" OR NOT EXISTS "${HC09_SDK_INCLUDE}/um")
    message(FATAL_ERROR "${HC09_WINSYSROOT} does not hold the MSVC runtime and Windows SDK (run xwin splat)")
endif()

# clang-cl finds the headers and lld-link the libraries of both through /winsysroot. (On Linux a clang-cl option
# may start with '-' as well as '/'; '-' keeps it apart from file paths.)
set(HC09_CROSS_FLAGS "-winsysroot \"${HC09_WINSYSROOT}\" -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")
set(CMAKE_C_FLAGS_INIT "${HC09_CROSS_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${HC09_CROSS_FLAGS}")
foreach(kind EXE SHARED MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_INIT "/winsysroot:\"${HC09_WINSYSROOT}\" /MANIFEST:NO")
endforeach()
# llvm-rc has no /winsysroot: the SDK's and runtime's include folders.
set(CMAKE_RC_FLAGS_INIT "-I \"${HC09_SDK_INCLUDE}/um\" -I \"${HC09_SDK_INCLUDE}/shared\" -I \"${HC09_SDK_INCLUDE}/ucrt\" -I \"${HC09_MSVC_DIR}/include\"")

set(CMAKE_FIND_ROOT_PATH ${HC09_WINSYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
