@echo off
rem Zeste Windows build (MSVC + Ninja), mirrors .ci/win-msvc.sh without vcpkg/FFmpeg.
rem Usage: tools\zeste\build_windows.cmd [configure] [nocheck]
rem After the build, tools\zeste\check.ps1 runs the unit tests and the test ROM (nocheck skips it).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0..\.."
set CHECK=1
for %%a in (%*) do if /i "%%a"=="nocheck" set CHECK=0
for %%a in (%*) do if /i "%%a"=="configure" (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_FFMPEG=OFF -DENABLE_LTO=OFF || exit /b 1
)
cmake --build build || exit /b 1
if "%CHECK%"=="1" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check.ps1" || exit /b 1
)
endlocal
