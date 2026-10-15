# HC09Setup.exe: the installer/updater wizard. It contains no game code, so it builds without ppc/
# and with either MSVC or clang-cl: standalone (cmake -S tools/installer) for a release download,
# or as part of the main project (which is how updates rebuild it).
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PROJECT_NAME)
    # Standalone: take the version from the main project.
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt HC09_ROOT_CMAKE)
    string(REGEX MATCH "project\\(HC09Recomp VERSION ([0-9.]+)" HC09_VERSION_MATCH "${HC09_ROOT_CMAKE}")
    project(HC09Setup VERSION ${CMAKE_MATCH_1} LANGUAGES CXX RC)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
else()
    enable_language(RC)
endif()

set(HC09_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(HC09Setup WIN32
    wizard.cpp
    pipeline.cpp
    disc.cpp
    xex.cpp
    installer.rc
    ${HC09_ROOT}/src/common/settings.cpp
    ${HC09_ROOT}/src/common/updates.cpp)
target_include_directories(HC09Setup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${HC09_ROOT}/src)
target_compile_definitions(HC09Setup PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS
    HC09_VERSION="${PROJECT_VERSION}" HC09_VERSION_W=L"${PROJECT_VERSION}")
target_link_libraries(HC09Setup PRIVATE comctl32 shell32 ole32 uuid winhttp bcrypt)
# Static runtime: setup runs before Visual Studio (and its runtime) is installed.
set_property(TARGET HC09Setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
# HC09.exe starts build/installer/HC09Setup.exe --update when an update is accepted.
set_target_properties(HC09Setup PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${HC09_ROOT}/build/installer
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${HC09_ROOT}/build/installer)
