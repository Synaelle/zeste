<p align="center">
  <img src="dist/zeste.png" width="128" alt="Zeste icon">
</p>

<h1 align="center">Zeste</h1>

Zeste is a fork of [Azahar](https://github.com/azahar-emu/azahar) that I use to test 3DS
homebrew. Azahar is very forgiving, so things that crash or break on a real New 3DS often
work fine in it. Zeste tries to catch those cases. Warning it's vibecoded but is VERY useful for my 3DS ports.

When enabled (everything is off by default), it can:

- turn bad memory accesses and stack overflows into real crashes / exception handler calls
- warn about GPU stuff the real PICA200 doesn't like (NaN vertices, values outside its 24-bit
  floats, broken textures, buffers it can't reach)
- warn when the CPU cache wasn't flushed before the GPU or DSP reads something
- use a memory layout closer to the real 178 MB mode
- shake up thread scheduling to make races show up (with a seed, so they can be replayed)
- warn about the `std::filesystem::remove_all` pattern, which hangs on hardware
- run with no window (`--headless`) and be driven by a script or over RPC: inputs,
  screenshots, memory dumps, GPU command dumps with the vertices of chosen draws,
  HOME / sleep / close
- boot a `.cia` straight from the command line

The rule is to only add what's known about the real console. Checks that rested on guessed
numbers (GPU/CPU/SD timing models, a fixed-point "wrap" model for big triangles) were removed
once that became clear; things get measured on the console before Zeste models them.

Settings are under Zeste > Hardware accuracy settings, or `--strict` on the command line
to turn everything on. `zeste.exe --help` lists the options.

## Downloads

Releases are Windows only (`zeste.exe` and its DLLs in a zip). On Linux, build it yourself
(below). Zeste keeps its own data, separate from Azahar's: `%APPDATA%\Zeste` on Windows,
`~/.local/share/zeste` and `~/.config/zeste` on Linux.

## Building (Windows)

Needs Visual Studio 2022 (or the Build Tools) with the C++ workload.

```
git clone --recursive <this repo>
cd Zeste
tools\zeste\build_windows.cmd configure
```

The exe ends up in `build\bin\Release\zeste.exe`. After the first time, just run
`tools\zeste\build_windows.cmd`.

## Building (Linux)

Tested on Debian 13 (GCC 14). Other distros need the same packages under their own names.

```
sudo apt install build-essential cmake ninja-build git pkg-config python3 \
    qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-tools-dev qt6-tools-dev-tools \
    qt6-l10n-tools libgl-dev libegl-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
    libxi-dev libxinerama-dev libxss-dev libxkbcommon-dev libwayland-dev libpulse-dev \
    libasound2-dev libusb-1.0-0-dev libssl-dev
git clone --recursive --depth 1 --shallow-submodules https://github.com/Synaelle/zeste.git
cd zeste
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_FFMPEG=OFF -DENABLE_LTO=OFF
ninja -C build
```

The binary is `build/bin/Release/zeste`. `--headless` still needs a display server; on a
machine without one, install `xvfb` and run it as `xvfb-run -a build/bin/Release/zeste
--headless ...`. The clone pulls a lot of submodules and the first build has about 2,500 steps,
so give both some time.

macOS and Android are untouched from Azahar and may or may not build.

## Credits

All the actual emulator work is from the Azahar and Citra developers. Zeste just adds a
testing layer on top. Same license as Azahar (GPLv2 or later, see `license.txt`).

Not affiliated with Nintendo.
