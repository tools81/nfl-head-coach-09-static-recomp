@echo off
rem Builds thirdparty\XenonRecomp (XenonAnalyse + XenonRecomp) with clang-cl and Ninja.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;C:\Program Files\CMake\bin;%PATH%
set ROOT=%~dp0..
cmake -S "%ROOT%\thirdparty\XenonRecomp" -B "%ROOT%\build\xenonrecomp" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
cmake --build "%ROOT%\build\xenonrecomp" --target XenonAnalyse XenonRecomp || exit /b 1
