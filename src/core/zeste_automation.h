// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste automation: a frame-timed command script (--script) and live commands (RPC), executed on
// the emulation thread at every emulated VBlank.
//
// Script syntax, one command per line ('#' starts a comment):
//   @<frame> <command>    run at absolute frame <frame> (VBlank count since boot)
//   +<frames> <command>   run <frames> after the previous command's frame
//   <command>             same frame as the previous command
//
// Commands:
//   press <buttons> [frames=1]     buttons joined with '+': A B X Y L R START SELECT UP DOWN LEFT
//                                  RIGHT (D-pad). Held for <frames> frames.
//   hold <buttons> / release <buttons|all>
//   touch <x> <y> [frames=1]       bottom screen coordinates (0-319, 0-239)
//   stick <x> <y> [frames=1]       circle pad, -156..156 (0 0 to centre)
//   lcd <top|bottom|both> <file.png>   save what the LCD shows, read from the LCD framebuffer
//                                  registers (like Rosalina), not from the host render targets
//   screenshot <file.png>          same as "lcd both"
//   dump-mem <vaddr> <size> <file> current process virtual memory
//   dump-phys <paddr> <size> <file>    physical memory (FCRAM/VRAM), GPU caches flushed first
//   dump-vram <file>               the 6 MB of VRAM
//   dump-cmdlist <file>            PICA command lists of the next frame, with the register
//                                  state at every draw
//   home [frames=120]              HOME button: suspend to (simulated) HOME Menu, resume after
//   sleep [frames=120]             close the lid: sleep query, sleep, wake up after <frames>
//   close                          HOME Menu "close": APT OrderToClose notification
//   rosalina [ms=3000]             Rosalina combo (L+Down+Select) then pause the process
//   report [path]                  write the inaccuracy report now
//   keyboard [text]                headless: answer the game's software keyboard with <text>
//                                  and OK (queued if the game has not asked yet)
//   keyboard-cancel                headless: press Cancel on the software keyboard
//   wait-log [timeout=<frames|Ns>] [file=<sdmc:/path>] <text>
//                                  pause the script until <text> appears in the game's debug
//                                  output (or in that file); later commands move by the time
//                                  waited; on timeout the run ends with exit code 3
//   save-state <file> / load-state <file>
//                                  save or load an emulator save state (same game build and
//                                  same Zeste build only)
//   speed <percent|unlimited|normal>  emulation speed limit (unlimited: as fast as possible)
//   heap-report <file>             the game's newlib malloc heap: free blocks, largest block
//   log <text>                     write a line to the log
//   quit [exit code=0]             stop emulation and exit (headless) / stop (windowed)

#pragma once

#include <functional>
#include <string>
#include <vector>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Zeste::Automation {

/// HID pad bits (same layout as Service::HID::PadState).
enum Buttons : u32 {
    BUTTON_A = 1 << 0,
    BUTTON_B = 1 << 1,
    BUTTON_SELECT = 1 << 2,
    BUTTON_START = 1 << 3,
    BUTTON_RIGHT = 1 << 4,
    BUTTON_LEFT = 1 << 5,
    BUTTON_UP = 1 << 6,
    BUTTON_DOWN = 1 << 7,
    BUTTON_R = 1 << 8,
    BUTTON_L = 1 << 9,
    BUTTON_X = 1 << 10,
    BUTTON_Y = 1 << 11,
    // New 3DS: these come from ir:rst (merged into hidKeysHeld by libctru), not the HID pad.
    BUTTON_ZL = 1 << 14,
    BUTTON_ZR = 1 << 15,
};

/// Buttons that belong to ir:rst rather than the HID pad.
constexpr u32 IrRstButtons = BUTTON_ZL | BUTTON_ZR;

struct InputOverride {
    u32 buttons = 0;
    bool stick_active = false;
    s16 stick_x = 0;
    s16 stick_y = 0;
    bool touch_active = false;
    u16 touch_x = 0;
    u16 touch_y = 0;
    bool cstick_active = false; ///< New 3DS C stick (ir:rst), same range as the circle pad
    s16 cstick_x = 0;
    s16 cstick_y = 0;
};

/// Loads a script file. Returns false (and logs) on a parse error.
bool LoadScript(const std::string& path);

/// Same as LoadScript, from text (used for --quit-after). `name` is used in error messages.
bool LoadScriptText(const std::string& text, const std::string& name = "command line");

/// Queues a single command for the next VBlank (RPC / debugging). Returns a parse error message,
/// or an empty string on success.
std::string QueueCommand(const std::string& line);

/// Called by the GPU at every emulated VBlank (emulation thread).
void OnVBlank(Core::System& system);

/// Read by the HID service when it builds the pad/touch state.
const InputOverride& GetInputOverride();

/// True when a script or queued command is active (HID then always polls the override).
bool IsActive();

/// Frontend hook for "quit" (headless: exit the process). Without one, emulation is shut down.
void SetQuitHandler(std::function<void(int)> handler);

/// Clears the script and the input state (new boot).
void Reset();

/// The game's debug output (svcOutputDebugString), for wait-log and the run summary.
void OnDebugOutput(const std::string& text);

/// For the run summary: the last lines of the debug output, the screenshots saved by script/RPC
/// commands, and why the run ended ("" if no command ended it).
std::vector<std::string> RecentDebugOutput();
std::vector<std::string> Screenshots();
std::string ExitReason();
/// Sets the exit reason unless one is already set.
void SetExitReason(const std::string& reason);

/// Saves the given LCD ("top" or "bottom") to a PNG through the frontend image interface.
bool DumpLcd(Core::System& system, int screen, const std::string& path);

} // namespace Zeste::Automation
