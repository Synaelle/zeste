# Changelog

Every Zeste release, newest first. Downloads are on the
[releases page](https://github.com/Synaelle/zeste/releases).

## Unreleased

- Zeste is now developed and tested on Linux. Each release has a Linux AppImage next to the
  Windows zip, which is no longer tested.
- Each release has a `SHA256SUMS` file and GitHub build attestations for its downloads.
- Fixed a crash on Linux when checking for updates.

## 0.5.4 (2026-11-03)

- The update check reads the releases on GitHub, where Zeste now lives.
- Merged Azahar's latest changes, including an optional on-screen software keyboard, a Turbo
  (Hold) hotkey, a configurable battery state, RPC requests for input and save states, and fixes
  for the Thumb `ADD PC` instruction, file append mode and DSP sleep.

## 0.5.3 (2026-10-22)

- Fixed the hidden Enhancements tab (and the per-game Cheats tab) showing behind the settings.

## 0.5.2 (2026-10-22)

- New status bar look, with no frame around each item, a faint divider between the Zeste buttons
  and the performance figures, even spacing and a soft hover on the buttons.
- No confirmation when quitting while a game runs. The option is still in Configure > General.
- `--offline` also turns the console's Wi-Fi state to "not connected", so the HOME Menu no longer
  shows "Internet" and `osGetWifiStrength()` returns 0.
- With `--rpc-port 0`, the RPC port picked at start is kept when a save state load restarts the
  RPC server, so clients keep reaching it.
- The update check reads the releases on GitLab.
- The console (New 3DS or Old 3DS) is chosen in its own group at the top of Configure > Hardware
  accuracy, with what each mode means. The System tab no longer has the New 3DS checkbox.
- Memory warnings. When the game's malloc heap is 85% and then 95% full, a warning goes to the
  log, the console output and `run.txt`, and the peak is in `summary.md`. They are on by default
  and can be turned off in Configure > Hardware accuracy > Reporting or with
  `--zeste no_memory_warnings`.
- Fixed a crash on exit when a script `quit` ran right after a `wait-log` timeout (exit code
  0xC0000005 instead of 3). The first quit now wins.
- In the HOME Menu, titles without a manual (most homebrew CIAs) no longer get a Manual button.
  The manual's content index was given for every installed title, and the button led to an error.
- Zeste is lighter, without what homebrew testing never uses.
  - Removed the Vulkan renderer (OpenGL and software stay, and a config or `--renderer vulkan`
    from an older version gets OpenGL), translations, `azahar-room.exe`, the public room list
    and web service, GameCube adapter support, Azahar's compatibility list, four of the six
    themes (System and Dark stay), the ROM compress and bundle tools, and DLLs nothing loaded
    (FFmpeg, the DirectX shader compiler, Qt SVG and some Qt plugins).
  - Hid the Multiplayer menu and status, Tools > Movie, the per-game Cheats tab, the
    Enhancements and Network tabs, the Large Screen, Hybrid and Custom layouts, and OpenAL in
    the audio outputs.

## 0.5.1 (2026-10-22)

- RPC commands can wait until they have run and return their result
  (`ZesteRPC.command(text, wait=True)`).
- The state can be queried over RPC (`ZesteRPC.state()`), with the frame, pause, title, console,
  keyboard, pending `wait-log`, exit reason and run folder.
- The game's debug output is written live to `game_output.txt` in the run folder and can be read
  over RPC from any line (`ZesteRPC.game_output()`).
- `--rpc-port N` lets several instances run with RPC at once. `0` picks a free port, printed as
  `Zeste RPC port`.
- New `wait-cancel`, and `wait-log on-timeout=continue` to go on after a timeout.
- Script and RPC arguments in double quotes can contain spaces.

## 0.5.0 (2026-10-22)

- Nintendo's online services (and Pretendo's) are never reached. Name lookups and HTTP requests
  for them fail as if there were no route, and title downloads from Nintendo's servers are
  refused.
- HOME Menu runs, headless too, with the system files from your own console. They come with
  `--home-menu [region]`, `--install-cia`, `--console new3ds|old3ds`, `--region` and `--country`
  (in memory only).
- `--offline` turns off the network entirely.
- Experimental Old 3DS mode (New 3DS mode off, or `--console old3ds`). The New 3DS-only parts
  (the 124 MB of `hw_memory_layout`, ZL, ZR and the C stick in scripts) are off in that mode.
  Old 3DS-only changes will come out as pre-releases (`X.Y.Z-preN`), which normal Zeste is
  never offered.
- Fixed an internal error after a failed name lookup (unknown host).
- Texts that called this program "Azahar" now say Zeste, and the save state version message no
  longer points to Azahar.
- The version follows a new release tag reliably.
- Merged Azahar's latest changes (input latency improvements, game list icons fix).

## 0.4.6 (2026-10-20)

- ZL, ZR and the C stick in scripts. `press`, `hold` and `release` accept `ZL` and `ZR`, and
  `cstick <x> <y> [frames]` holds the C stick. They go through ir:rst, as on a New 3DS.
- The title bar, `--version` and the About box show the release version instead of a commit hash
  (`-dev` for builds made after a release).

## 0.4.5 (2026-10-20)

- Minor fix concerning the playtesting report feature.

## 0.4.4 (2026-10-20)

- STRICT button in the status bar, and a checkable "Strict hardware mode" in the Zeste menu, to
  turn every Zeste check on or off at once.
- Nothing is left in the registry. Qt's file dialog key (`HKCU\Software\QtProject`) is restored
  as it was when Zeste exits.

## 0.4.3 (2026-10-19)

- Sampling CPU profiler with `--profile`, or `profile-start` and `profile-stop` in a script. It
  writes the hottest functions per core and thread to `profile.md` and collapsed stacks for flame
  graphs to `profile.folded`, with names from `--elf`.
- `--elf` reads only the symbol table, so debug ELFs of a gigabyte or more load quickly.

## 0.4.2 (2026-10-19)

- Update check. Zeste compares itself with the latest release (cached for 6 hours), offers the
  download page, and prints one line in headless runs. It is also in Help > Check for Updates.
- Removed Azahar's Report Compatibility and FAQ links from the Help menu.

## 0.4.1 (2026-10-19)

- Zeste's own compatibility list in the game list. Titles that haven't been run in Zeste show as
  Untested.

## 0.4.0 (2026-10-19)

- Portable by default. Everything Zeste writes goes into a `user` folder next to `zeste.exe`
  (`%APPDATA%\Zeste` only when that folder can't be written).
- Windows only. Linux builds are no longer supported or documented.
- Homebrew only. Commercial games aren't supported.

## 0.3.3 (2026-10-19)

- Fixed headless runs of apps that use the network crashing on exit (0xC0000005). Shutdown now
  closes the host sockets and waits for the game's blocking network calls first.
- Crash reports for `zeste.exe` itself, in `zeste_crash.txt` and `zeste_crash.dmp` in the run
  folder.

## 0.3.2 (2026-10-18)

- With `cpu_exceptions`, CPU writes to VRAM that the `.rsf` maps read-only are data aborts, as on
  the console.
- Scripted runs get `save-state`, `load-state` and `--load-state`, `speed` and `--speed`,
  `wait-log`, and seconds in `--quit-after` and in script timings.
- `heap-report <file>` reports the game's malloc heap (used, free, largest free block).
- A run summary (`summary.md`) for every run, with the crash and its call chain (`--elf`).
- `hw_memory_layout` no longer moves the linear heap, since that theory was disproven on
  hardware.
- `--help` and other command output go to stdout when it is redirected.

## 0.3.1 (2026-10-18)

- The headless software keyboard needs no dialog. It is answered with `keyboard <text>` or
  `keyboard-cancel` from a script or over RPC.
- Headless runs never take the foreground.

## 0.3.0 (2026-10-17)

- Only what has been measured on a New 3DS is modelled. A calibration ROM showed the PICA clips
  huge triangles like normal float clipping, so the fixed-point "wrap" model, `fixed_point_raster`
  and `draw_wrapped` are gone, and `strict_pica` only flags vertices beyond the measured range.
- `strict_pica` reports vertex shader outputs outside the PICA's 24-bit float range.
- `dump-cmdlist` can list every triangle's vertex shader output and clipped polygon, over several
  frames.
- `fs_checks` reports the `std::filesystem::remove_all` pattern that hangs on hardware.
- Removed the timing models (`pica_timing`, `cpu_speed_model`, `sd_latency`, `async_gx`), the
  draw-mode switch report and the bug and inaccuracy list.
- Merged Azahar's latest changes.

## 0.2.0 (2026-10-16)

- One folder per run (log, report, `run.txt`), so several instances never mix their files.
- Draw tags let homebrew name its GPU work, and findings show the name.
- A grouped summary at the top of the report.

## 0.1.0 (2026-10-16)

- First release, Azahar with opt-in checks that make it behave more like a real New 3DS (or say
  when something would break on one). It has CPU exceptions, GPU checks, cache flush detection,
  memory layout, seeded preemption, headless runs with scripts and RPC, and `--boot-cia`.
