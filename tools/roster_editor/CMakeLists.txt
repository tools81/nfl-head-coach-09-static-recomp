# HC09RosterEditor.exe: edits roster saves (teams, players, coaches, portraits) and, for HC27, the draft picks
# and special skills new careers start with. It contains no game code, so it builds without ppc/.
enable_language(RC)

set(HC09_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(HC09RosterEditor WIN32
    main.cpp
    tdb.cpp
    gamedata.cpp
    roster.cpp
    picture.cpp
    disc_images.cpp
    editor.rc
    ${HC09_ROOT}/tools/installer/disc.cpp)
target_include_directories(HC09RosterEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${HC09_ROOT}/tools/installer)
target_compile_definitions(HC09RosterEditor PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(HC09RosterEditor PRIVATE comctl32 comdlg32 shell32 ole32 windowscodecs)
set_target_properties(HC09RosterEditor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${HC09_ROOT}/build/roster_editor
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${HC09_ROOT}/build/roster_editor)
