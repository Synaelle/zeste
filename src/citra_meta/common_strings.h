// Copyright 2024-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <string>

namespace Common {

constexpr char help_string[] =
    "Usage: {} [options] <file path>\n"
    "-c  [path]                  Z3DS compress a ROM located at the given path\n"
    "                              (optionally provide '-o [path]' for output directory)\n"
    "-d, --dump-video [path]     Dump video recording of emulator playback to the given file path\n"
    "-f, --fullscreen            Start in fullscreen mode\n"
    "-g, --gdbport [port]        Enable gdb stub on the given port\n"
    "-h, --help                  Display this help and exit\n"
    "-i, --install [path]        Install a CIA file at the given path\n"
    "-p, --movie-play [path]     Play a TAS movie located at the given path\n"
    "-r, --movie-record [path]   Record a TAS movie to the given file path\n"
    "-a, --movie-record-author [author]   Set the author for the recorded TAS movie (to be used "
    "alongside --movie-record)\n"
#ifdef ENABLE_ROOM
    "    --room                  Utilize dedicated multiplayer room functionality (equivalent to "
    "the old citra-room executable)\n"
#endif
    "-v, --version               Output version information and exit\n"
    "-w, --windowed              Start in windowed mode\n"
    "-x  [path]                  Decompress a Z3DS compressed ROM located at the given path\n"
    "                              (optionally provide '-o [path]' for output directory)\n"
    "\n"
    "Zeste (hardware accuracy, all off by default; command-line choices are not saved):\n"
    "    --strict                Strict hardware mode: enable every accuracy option\n"
    "    --zeste [list]          Enable options by name, comma separated: strict_pica,\n"
    "                              cpu_exceptions, kernel_fixes, cache_detection,\n"
    "                              hw_memory_layout, preempt_svc, preempt_random,\n"
    "                              strict_priority, fs_checks\n"
    "    --seed [n]              Seed for random preemption\n"
    "    --break-on-first        Pause on the first hardware-accuracy report\n"
    "    --no-dialogs            Never block on modal error dialogs (log and stop instead)\n"
    "    --headless              Run with no window on screen (needs an app path); audio muted,\n"
    "                              no dialogs, config not saved, exits when emulation stops\n"
    "    --script [file]         Frame-timed automation script (input, LCD dumps, memory dumps...)\n"
    "    --quit-after [frames]   Stop and exit after this many emulated frames\n"
    "    --renderer [api]        software, opengl or vulkan for this run\n"
    "    --boot-cia [path]       Install a CIA and boot the installed title\n"
    "    --rpc                   Enable the RPC server (UDP 45987, see dist/scripting/ZesteRPC.py)\n"
    "    --zeste-report [path]   Inaccuracy report path, without extension\n"
    "                              (default: <log dir>/zeste_report.md/.json)";

} // namespace Common
