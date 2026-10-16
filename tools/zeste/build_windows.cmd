@echo off
rem Zeste Windows build (MSVC + Ninja), mirrors .ci/win-msvc.sh without vcpkg/FFmpeg.
rem Usage: tools\zeste\build_windows.cmd [configure]
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0..\.."
if "%1"=="configure" (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_FFMPEG=OFF -DENABLE_LTO=OFF || exit /b 1
)
cmake --build build || exit /b 1
endlocal
