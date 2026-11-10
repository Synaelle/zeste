<p align="center">
  <img src="dist/zeste.png" width="128" alt="Zeste icon">
</p>

<h1 align="center">Zeste</h1>

> [!IMPORTANT]
> **Homebrew only, New 3DS first.** Commercial games aren't supported. With Zeste's options off
> they should run much like in Azahar, but turning the options on may break them. Every check is
> based on New 3DS measurements, and Zeste emulates a New 3DS by default. Its Old 3DS mode
> (`--console old3ds`) is experimental and not checked against a real Old 3DS.

> [!TIP]
> **Any contribution for Old 3DS accuracy is welcome!** The Old 3DS side mostly needs
> measurements from a real Old 3DS or 2DS, such as test programs run on the console with their
> results.

Zeste is a 3DS emulator for testing homebrew, based on
[Azahar](https://github.com/azahar-emu/azahar). Azahar runs a lot of code that would crash or
misbehave on a real console. Zeste adds checks that catch those problems on your PC, before you
try the app on hardware.

Everything is off by default. Once enabled, Zeste can

- turn bad memory accesses and stack overflows into real crashes and exception handler calls
- warn about GPU work the real PICA200 doesn't like, such as NaN vertices, values outside its
  24-bit floats, broken textures or buffers it can't reach
- warn when the CPU cache wasn't flushed before the GPU or DSP reads something
- give `.3dsx` files the 124 MB they get under the Homebrew Launcher
- shake up thread scheduling to make races show up, with a seed so they can be replayed
- warn about the `std::filesystem::remove_all` pattern, which hangs on hardware
- warn when the game's malloc heap is nearly full
- run with no window (`--headless`) and be driven by a script or over RPC, with inputs,
  screenshots, memory dumps, GPU command dumps, HOME, sleep and close, and text for the game's
  on-screen keyboard
- make scripted runs faster and steadier with save states, unlimited speed, waiting for a line
  in the game's log and quitting after a set time
- report the game's malloc heap and write a summary of each run, with the crash and its function
  names when given the `.elf`
- profile the CPU, with the hottest functions per core and thread and a flame graph file
- boot a `.cia` straight from the command line
- boot the HOME Menu headless as a New or Old 3DS, with a chosen region and country, and
  optionally no network at all (`--offline`), using the system files from your own console
  (File > Set Up System Files)
- tell you when a new Zeste release is out, in headless runs too (Help > Check for Updates)

Zeste never connects to Nintendo's online services (or Pretendo's). Name lookups and HTTP
requests for them fail as if there were no route, even with system files from your own console,
so a test run can't log in to your account or reach the eShop. Other network access works.

Settings are under Zeste > Hardware accuracy settings. The STRICT button in the status bar
(or `--strict` on the command line) turns everything on or off. `zeste --help` lists the
options.

## Downloads

Zeste is developed and tested on Linux. Each [release](https://github.com/Synaelle/zeste/releases)
has a Linux AppImage and a Windows zip. The Windows zip is built the same way but nobody tests it
before a release. Changes that only affect the Old 3DS side come out as pre-releases
(`0.5.0-pre1` and so on). They never check for updates, and normal Zeste is never offered them.

Each release also has a `SHA256SUMS` file and a GitHub build attestation for every download.
`gh attestation verify <file> --repo Synaelle/zeste` checks that a file was built by the
release workflow from this repository.

On Linux, make the AppImage executable and run it. It needs a recent distribution (Debian 13,
Ubuntu 25.04 or newer). Zeste keeps its data (settings, the emulated SD card and NAND, logs, run
reports) in `~/.local/share/zeste` and `~/.config/zeste`. If a `user` folder exists in the
folder Zeste is started from, it uses that folder instead, which keeps a test setup apart from
everything else. Headless runs need no display. Start them with `QT_QPA_PLATFORM=offscreen`
and `--renderer software` on a machine without a GPU.

On Windows, Zeste is portable. Everything it writes goes into a `user` folder next to
`zeste.exe`, created on the first start, so the whole install is that one folder. Only if that
folder can't be written (under Program Files, for example) does Zeste use `%APPDATA%\Zeste`
instead. Keep the Zeste folder in a short path (under about 100 characters, like `C:\Zeste` or
your Downloads folder). The emulated SD card has deep folders, and Windows refuses paths longer
than 260 characters, so installing a game fails in a folder buried too deep.

## Building

On Linux (Ubuntu 26.04 here), install the build tools and Qt 6.

```
sudo apt install build-essential cmake ninja-build git pkg-config ccache qt6-base-dev \
  qt6-base-private-dev qt6-multimedia-dev qt6-tools-dev libgl-dev libegl-dev libx11-dev \
  libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev libxss-dev \
  libxkbcommon-dev libwayland-dev libpulse-dev libasound2-dev libssl-dev
git clone --recursive https://github.com/Synaelle/zeste.git
cd zeste
tools/zeste/build_linux.sh configure
```

The binary ends up in `build/bin/Release/zeste`. After the first time, just run
`tools/zeste/build_linux.sh`. Each build ends with the unit tests.

On Windows, Zeste needs Visual Studio 2022 (or the Build Tools) with the C++ workload. Run
`tools\zeste\build_windows.cmd configure` in the clone, then `tools\zeste\build_windows.cmd`
after that. The exe ends up in `build\bin\Release\zeste.exe`.

macOS and Android aren't supported. Azahar's code for them is still there, so they may or may
not build, but nothing is tested on them.

## Changes

[CHANGELOG.md](CHANGELOG.md) lists every release and what it changed.

## Contributing

Bug reports, fixes and tools for testing homebrew are welcome. Anything that changes how Zeste
emulates the console (a new check, a timing, a memory or GPU behaviour) needs data from real
hardware first, such as a test program run on a New 3DS with its results. Zeste only models
what has been measured on the console, so an accuracy change without hardware data won't be
merged.

## Credits

All the actual emulator work is from the Azahar and Citra developers. Zeste just adds a
testing layer on top. Same license as Azahar (GPLv2 or later, see `license.txt`).

Not affiliated with Nintendo.
