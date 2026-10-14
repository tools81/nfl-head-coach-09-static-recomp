@echo off
rem Configures and builds the HC09 host executable with clang-cl and Ninja.
rem Usage: tools\build.bat [cmake --build arguments, e.g. --target HC09RecompLib]
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;C:\Program Files\CMake\bin;%PATH%
set ROOT=%~dp0..
if not exist "%ROOT%\build\hc09\build.ninja" (
    cmake -S "%ROOT%" -B "%ROOT%\build\hc09" -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
)
cmake --build "%ROOT%\build\hc09" %* || exit /b 1
