// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <iostream>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>
#include "common/color.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "common/zeste_report.h"
#include "common/zeste_run.h"
#include "core/core.h"
#include "core/frontend/image_interface.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "common/settings.h"
#include "core/zeste_automation.h"
#include "core/zeste_heap.h"
#include "core/zeste_keyboard.h"
#include "core/zeste_profiler.h"
#include "core/zeste_core.h"
#include "core/zeste_system_events.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

namespace Zeste::Automation {

namespace {

struct Command {
    u64 frame = 0;
    bool immediate = false; ///< Queued live: runs at the next VBlank whatever the frame.
    std::vector<std::string> args;
    int line = 0;
};

struct HeldInput {
    u64 until_frame;
    u32 buttons;
};

/// A "wait-log" in progress: the script is paused until the text shows up.
struct Wait {
    bool active = false;
    std::string text;
    std::string file;       ///< Host path of the watched file ("" = the game's debug output)
    u64 file_offset = 0;    ///< Bytes of the file already searched
    std::string carry;      ///< End of the last read (a match may straddle two reads)
    u64 started_frame = 0;
    u64 deadline_frame = 0; ///< 0 = no timeout
    bool seen = false;
};

struct State {
    std::mutex mutex;
    Wait wait;
    std::deque<std::string> debug_lines; ///< Last lines of the game's debug output
    std::vector<std::string> screenshots;
    std::string exit_reason;
    std::deque<Command> commands;
    std::vector<HeldInput> held;
    u32 sticky_buttons = 0;
    u64 stick_until = 0;
    u64 cstick_until = 0;
    u64 touch_until = 0;
    InputOverride input;
    bool active = false;
    std::function<void(int)> quit_handler;
};

State& GetState() {
    static State state;
    return state;
}

std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream stream{line};
    std::string token;
    while (stream >> token) {
        out.push_back(token);
    }
    return out;
}

bool ParseButtons(const std::string& text, u32& out) {
    out = 0;
    for (std::string name : Common::SplitString(text, '+')) {
        name = Common::ToUpper(name);
        if (name == "A") {
            out |= BUTTON_A;
        } else if (name == "B") {
            out |= BUTTON_B;
        } else if (name == "X") {
            out |= BUTTON_X;
        } else if (name == "Y") {
            out |= BUTTON_Y;
        } else if (name == "L") {
            out |= BUTTON_L;
        } else if (name == "R") {
            out |= BUTTON_R;
        } else if (name == "START") {
            out |= BUTTON_START;
        } else if (name == "SELECT") {
            out |= BUTTON_SELECT;
        } else if (name == "UP") {
            out |= BUTTON_UP;
        } else if (name == "DOWN") {
            out |= BUTTON_DOWN;
        } else if (name == "LEFT") {
            out |= BUTTON_LEFT;
        } else if (name == "RIGHT") {
            out |= BUTTON_RIGHT;
        } else if (name == "ZL") {
            out |= BUTTON_ZL;
        } else if (name == "ZR") {
            out |= BUTTON_ZR;
        } else if (name == "ALL") {
            out |= 0xFFF;
        } else {
            return false;
        }
    }
    return true;
}

u64 ParseNumber(const std::string& text, u64 fallback = 0) {
    try {
        std::size_t pos = 0;
        const u64 value = std::stoull(text, &pos, 0);
        return pos == text.size() ? value : fallback;
    } catch (...) {
        return fallback;
    }
}

s64 ParseSigned(const std::string& text) {
    try {
        return std::stoll(text, nullptr, 0);
    } catch (...) {
        return 0;
    }
}

/// "120" = 120 frames (60 Hz), "30s" = 30 seconds (1800 frames).
u64 ParseFrames(const std::string& text, u64 fallback = 0) {
    if (!text.empty() && (text.back() == 's' || text.back() == 'S')) {
        try {
            return static_cast<u64>(std::stod(text.substr(0, text.size() - 1)) * 60.0 + 0.5);
        } catch (...) {
            return fallback;
        }
    }
    return ParseNumber(text, fallback);
}

/// "sdmc:/x/y" -> host path in the emulated SD card; other paths are used as they are.
std::string HostPath(const std::string& path) {
    constexpr std::string_view prefix = "sdmc:/";
    if (path.rfind(prefix, 0) == 0) {
        return FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir) + path.substr(prefix.size());
    }
    return path;
}

std::string JoinFrom(const std::vector<std::string>& args, std::size_t first) {
    std::string text;
    for (std::size_t i = first; i < args.size(); ++i) {
        text += (i > first ? " " : "") + args[i];
    }
    return text;
}

void Quit(Core::System& system, int code, const std::string& reason) {
    auto& s = GetState();
    {
        std::scoped_lock lock{s.mutex};
        if (s.exit_reason.empty()) {
            s.exit_reason = reason;
        }
    }
    if (s.quit_handler) {
        s.quit_handler(code);
    } else {
        system.RequestShutdown();
    }
}

std::string ValidateCommand(const std::vector<std::string>& args) {
    static const std::vector<std::pair<std::string, std::size_t>> min_args = {
        {"press", 2},     {"hold", 2},      {"release", 2},   {"touch", 3},   {"stick", 3},   {"cstick", 3},
        {"lcd", 3},       {"screenshot", 2}, {"dump-mem", 4}, {"dump-phys", 4}, {"dump-vram", 2},
        {"dump-cmdlist", 2}, {"home", 1},   {"sleep", 1},     {"close", 1},   {"rosalina", 1},
        {"report", 1},    {"log", 1},       {"quit", 1},      {"keyboard", 1},
        {"keyboard-cancel", 1}, {"wait-log", 2}, {"save-state", 2}, {"load-state", 2},
        {"speed", 2},     {"heap-report", 2}, {"profile-start", 1}, {"profile-stop", 1},
    };
    if (args.empty()) {
        return "empty command";
    }
    const auto it = std::find_if(min_args.begin(), min_args.end(),
                                 [&](const auto& entry) { return entry.first == args[0]; });
    if (it == min_args.end()) {
        return "unknown command '" + args[0] + "'";
    }
    if (args.size() < it->second) {
        return "missing arguments for '" + args[0] + "'";
    }
    if (args[0] == "press" || args[0] == "hold" || args[0] == "release") {
        u32 buttons;
        if (!ParseButtons(args[1], buttons)) {
            return "unknown button in '" + args[1] + "'";
        }
    }
    return {};
}

bool WriteFile(const std::string& path, const u8* data, std::size_t size) {
    FileUtil::IOFile file{path, "wb"};
    if (!file.IsOpen() || file.WriteBytes(data, size) != size) {
        LOG_ERROR(Zeste, "automation: cannot write {}", path);
        return false;
    }
    LOG_INFO(Zeste, "automation: wrote {} ({} bytes)", path, size);
    return true;
}

void FlushGpu(Core::System& system, PAddr addr, u32 size) {
    if (system.HasGPU() && system.GPU().HasRenderer()) {
        if (auto* rasterizer = system.GPU().Renderer().Rasterizer()) {
            rasterizer->FlushRegion(addr, size);
        }
    }
}

bool DumpPhysical(Core::System& system, PAddr addr, u32 size, const std::string& path) {
    FlushGpu(system, addr, size);
    std::vector<u8> buffer(size);
    auto& memory = system.Memory();
    for (u32 offset = 0; offset < size;) {
        // Copy page by page: physical regions are not guaranteed to be contiguous in the host.
        const u32 chunk = std::min<u32>(size - offset, 0x1000 - ((addr + offset) & 0xFFF));
        if (const u8* src = memory.GetPhysicalPointer(addr + offset)) {
            std::memcpy(buffer.data() + offset, src, chunk);
        }
        offset += chunk;
    }
    return WriteFile(path, buffer.data(), buffer.size());
}

void Execute(Core::System& system, const Command& command) {
    auto& s = GetState();
    const auto& args = command.args;
    const std::string& verb = args[0];
    const u64 frame = Zeste::CurrentFrame(system);

    if (verb == "press" || verb == "hold") {
        u32 buttons = 0;
        ParseButtons(args[1], buttons);
        if ((buttons & IrRstButtons) && !Settings::values.is_new_3ds.GetValue()) {
            LOG_WARNING(Zeste, "automation: {} {}: an Old 3DS has no ZL / ZR (only with the "
                               "Circle Pad Pro, which scripts do not drive); ignored",
                        verb, args[1]);
        }
        if (verb == "hold") {
            s.sticky_buttons |= buttons;
        } else {
            const u64 frames = std::max<u64>(1, args.size() > 2 ? ParseNumber(args[2], 1) : 1);
            s.held.push_back({frame + frames, buttons});
        }
    } else if (verb == "release") {
        u32 buttons = 0;
        ParseButtons(args[1], buttons);
        s.sticky_buttons &= ~buttons;
        for (auto& held : s.held) {
            held.buttons &= ~buttons;
        }
    } else if (verb == "touch") {
        s.input.touch_active = true;
        s.input.touch_x = static_cast<u16>(std::clamp<s64>(ParseSigned(args[1]), 0, 319));
        s.input.touch_y = static_cast<u16>(std::clamp<s64>(ParseSigned(args[2]), 0, 239));
        s.touch_until = frame + std::max<u64>(1, args.size() > 3 ? ParseNumber(args[3], 1) : 1);
    } else if (verb == "stick") {
        s.input.stick_x = static_cast<s16>(std::clamp<s64>(ParseSigned(args[1]), -156, 156));
        s.input.stick_y = static_cast<s16>(std::clamp<s64>(ParseSigned(args[2]), -156, 156));
        s.input.stick_active = s.input.stick_x != 0 || s.input.stick_y != 0;
        s.stick_until =
            s.input.stick_active
                ? frame + std::max<u64>(1, args.size() > 3 ? ParseNumber(args[3], 1) : 1)
                : 0;
    } else if (verb == "cstick" && !Settings::values.is_new_3ds.GetValue()) {
        LOG_WARNING(Zeste, "automation: cstick: an Old 3DS has no C stick; ignored");
    } else if (verb == "cstick") {
        s.input.cstick_x = static_cast<s16>(std::clamp<s64>(ParseSigned(args[1]), -156, 156));
        s.input.cstick_y = static_cast<s16>(std::clamp<s64>(ParseSigned(args[2]), -156, 156));
        s.input.cstick_active = s.input.cstick_x != 0 || s.input.cstick_y != 0;
        s.cstick_until =
            s.input.cstick_active
                ? frame + std::max<u64>(1, args.size() > 3 ? ParseNumber(args[3], 1) : 1)
                : 0;
    } else if (verb == "lcd" || verb == "screenshot") {
        const std::string which = verb == "screenshot" ? "both" : args[1];
        const std::string& path = verb == "screenshot" ? args[1] : args[2];
        if (which == "both") {
            const auto dot = path.rfind('.');
            const std::string stem = dot == std::string::npos ? path : path.substr(0, dot);
            DumpLcd(system, 0, stem + "_top.png");
            DumpLcd(system, 1, stem + "_bottom.png");
        } else {
            DumpLcd(system, which == "bottom" ? 1 : 0, path);
        }
    } else if (verb == "dump-mem") {
        const VAddr addr = static_cast<VAddr>(ParseNumber(args[1]));
        const u32 size = static_cast<u32>(ParseNumber(args[2]));
        auto process = system.Kernel().GetCurrentProcess();
        if (!process) {
            LOG_ERROR(Zeste, "automation: dump-mem with no current process");
            return;
        }
        std::vector<u8> buffer(size);
        system.Memory().ReadBlock(*process, addr, buffer.data(), size);
        WriteFile(args[3], buffer.data(), buffer.size());
    } else if (verb == "dump-phys") {
        DumpPhysical(system, static_cast<PAddr>(ParseNumber(args[1])),
                     static_cast<u32>(ParseNumber(args[2])), args[3]);
    } else if (verb == "dump-vram") {
        DumpPhysical(system, Memory::VRAM_PADDR, Memory::VRAM_SIZE, args[1]);
    } else if (verb == "dump-cmdlist") {
        // dump-cmdlist <file> [vertex filter: tag substrings separated by '|', or *] [frames]
        system.GPU().PicaCore().ZesteArmCommandListDump(
            args[1], args.size() > 2 ? args[2] : "",
            args.size() > 3 ? static_cast<u32>(ParseNumber(args[3], 1)) : 1);
    } else if (verb == "home" || verb == "sleep" || verb == "close" || verb == "rosalina") {
        SystemEvents::Start(system, verb, args.size() > 1 ? ParseNumber(args[1], 0) : 0);
    } else if (verb == "report") {
        Zeste::WriteReport(args.size() > 1 ? args[1] : Zeste::DefaultReportBasePath(),
                           "on request");
    } else if (verb == "keyboard") {
        Keyboard::Type(JoinFrom(args, 1));
    } else if (verb == "keyboard-cancel") {
        Keyboard::Cancel();
    } else if (verb == "log") {
        std::string text;
        for (std::size_t i = 1; i < args.size(); ++i) {
            text += (i > 1 ? " " : "") + args[i];
        }
        LOG_INFO(Zeste, "script (frame {}): {}", frame, text);
    } else if (verb == "quit") {
        const int code = args.size() > 1 ? static_cast<int>(ParseSigned(args[1])) : 0;
        LOG_INFO(Zeste, "automation: quit (exit code {}) at frame {}", code, frame);
        Quit(system, code, fmt::format("quit command at frame {} (exit code {})", frame, code));
    } else if (verb == "wait-log") {
        // wait-log [timeout=<frames|Ns>] [file=<sdmc:/path or host path>] <text...>
        Wait wait;
        wait.active = true;
        wait.started_frame = frame;
        std::size_t first = 1;
        for (; first < args.size(); ++first) {
            if (args[first].rfind("timeout=", 0) == 0) {
                wait.deadline_frame = frame + ParseFrames(args[first].substr(8), 0);
            } else if (args[first].rfind("file=", 0) == 0) {
                wait.file = HostPath(args[first].substr(5));
                // Only text written from now on counts.
                wait.file_offset = FileUtil::Exists(wait.file) ? FileUtil::GetSize(wait.file) : 0;
            } else {
                break;
            }
        }
        wait.text = JoinFrom(args, first);
        LOG_INFO(Zeste, "automation: waiting for \"{}\" in {} (frame {})", wait.text,
                 wait.file.empty() ? std::string{"the debug output"} : wait.file, frame);
        std::scoped_lock lock{s.mutex};
        s.wait = std::move(wait);
    } else if (verb == "save-state" || verb == "load-state") {
        const std::string path = HostPath(args[1]);
        LOG_INFO(Zeste, "automation: {} {}", verb, path);
        system.SetSaveStatePathOverride(path);
        system.SendSignal(verb == "save-state" ? Core::System::Signal::Save
                                               : Core::System::Signal::Load,
                          99);
    } else if (verb == "speed") {
        // speed <percent | unlimited | normal>
        const std::string value = Common::ToLower(args[1]);
        if (value == "normal") {
            Settings::is_temporary_frame_limit = false;
        } else {
            Settings::is_temporary_frame_limit = true;
            Settings::temporary_frame_limit =
                value == "unlimited" ? 0.0 : static_cast<double>(ParseNumber(value, 100));
        }
        LOG_INFO(Zeste, "automation: speed {}", value);
    } else if (verb == "heap-report") {
        Heap::WriteReport(system, HostPath(args[1]));
    } else if (verb == "profile-start") {
        Profiler::Start(system, args.size() > 1
                                    ? static_cast<u32>(ParseNumber(args[1], Profiler::DefaultIntervalUs))
                                    : Profiler::DefaultIntervalUs);
    } else if (verb == "profile-stop") {
        // Default: profile-<frame> in the run folder. A given path may end in .md.
        std::string base = args.size() > 1 ? HostPath(args[1])
                                           : Run::Folder() + fmt::format("profile-{}", frame);
        if (base.ends_with(".md")) {
            base.resize(base.size() - 3);
        }
        const std::string written = Profiler::Stop(system, base);
        LOG_INFO(Zeste, "automation: profile-stop at frame {}: {}", frame,
                 written.empty() ? "no profile was running" : written);
    }
}

/// Pauses the script until the awaited text shows up; then later script commands move by the
/// time waited, so "@frame" lines after a wait-log stay relative to it.
void CheckWait(Core::System& system, u64 frame) {
    auto& s = GetState();
    std::unique_lock lock{s.mutex};
    auto& wait = s.wait;
    if (!wait.seen && !wait.file.empty() && frame % 10 == 0 && FileUtil::Exists(wait.file)) {
        const u64 size = FileUtil::GetSize(wait.file);
        if (size < wait.file_offset) {
            wait.file_offset = 0; // the file was rewritten
        }
        if (size > wait.file_offset) {
            FileUtil::IOFile file{wait.file, "rb"};
            std::string chunk(static_cast<std::size_t>(size - wait.file_offset), '\0');
            file.Seek(static_cast<s64>(wait.file_offset), SEEK_SET);
            chunk.resize(file.ReadBytes(chunk.data(), chunk.size()));
            wait.file_offset += chunk.size();
            const std::string text = wait.carry + chunk;
            wait.seen = text.find(wait.text) != std::string::npos;
            wait.carry = text.substr(text.size() > wait.text.size() ? text.size() - wait.text.size()
                                                                     : 0);
        }
    }
    if (wait.seen) {
        const u64 waited = frame - wait.started_frame;
        LOG_INFO(Zeste, "automation: \"{}\" seen at frame {} (waited {} frames)", wait.text,
                 frame, waited);
        for (auto& command : s.commands) {
            if (!command.immediate) {
                command.frame += waited;
            }
        }
        wait = {};
        return;
    }
    if (wait.deadline_frame != 0 && frame >= wait.deadline_frame) {
        const std::string text = wait.text;
        wait = {};
        lock.unlock();
        LOG_ERROR(Zeste, "automation: wait-log timed out waiting for \"{}\"", text);
        std::cout << "Zeste wait-log: timed out waiting for \"" << text << "\"" << std::endl;
        Quit(system, 3,
             fmt::format("wait-log timed out at frame {} waiting for \"{}\"", frame, text));
    }
}

} // Anonymous namespace

void OnDebugOutput(const std::string& text) {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.debug_lines.push_back(text);
    while (s.debug_lines.size() > 40) {
        s.debug_lines.pop_front();
    }
    if (s.wait.active && s.wait.file.empty() && text.find(s.wait.text) != std::string::npos) {
        s.wait.seen = true;
    }
}

std::vector<std::string> RecentDebugOutput() {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    return {s.debug_lines.begin(), s.debug_lines.end()};
}

std::vector<std::string> Screenshots() {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    return s.screenshots;
}

std::string ExitReason() {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    return s.exit_reason;
}

void SetExitReason(const std::string& reason) {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    if (s.exit_reason.empty()) {
        s.exit_reason = reason;
    }
}

bool LoadScript(const std::string& path) {
    std::string text;
    if (!FileUtil::Exists(path) || FileUtil::ReadFileToString(true, path, text) == 0) {
        LOG_CRITICAL(Zeste, "automation: cannot open script {}", path);
        return false;
    }
    return LoadScriptText(text, path);
}

bool LoadScriptText(const std::string& script, const std::string& path) {
    std::istringstream file{script};
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    u64 frame = 0;
    std::string line;
    int line_number = 0;
    bool ok = true;
    while (std::getline(file, line)) {
        ++line_number;
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        auto tokens = Tokenize(line);
        if (tokens.empty()) {
            continue;
        }
        if (tokens[0][0] == '@') {
            frame = ParseFrames(tokens[0].substr(1), frame);
            tokens.erase(tokens.begin());
        } else if (tokens[0][0] == '+') {
            frame += ParseFrames(tokens[0].substr(1), 0);
            tokens.erase(tokens.begin());
        }
        if (tokens.empty()) {
            continue;
        }
        if (const std::string error = ValidateCommand(tokens); !error.empty()) {
            LOG_CRITICAL(Zeste, "automation: {}:{}: {}", path, line_number, error);
            ok = false;
            continue;
        }
        s.commands.push_back({frame, false, std::move(tokens), line_number});
    }
    std::stable_sort(s.commands.begin(), s.commands.end(),
                     [](const Command& a, const Command& b) { return a.frame < b.frame; });
    s.active = true;
    LOG_INFO(Zeste, "automation: loaded {} commands from {}", s.commands.size(), path);
    return ok;
}

std::string QueueCommand(const std::string& line) {
    auto tokens = Tokenize(line);
    if (const std::string error = ValidateCommand(tokens); !error.empty()) {
        return error;
    }
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.commands.push_front({0, true, std::move(tokens), 0});
    s.active = true;
    return {};
}

void OnVBlank(Core::System& system) {
    Profiler::OnVBlank(system);
    auto& s = GetState();
    if (!s.active) {
        return;
    }
    const u64 frame = Zeste::CurrentFrame(system);
    if (s.wait.active) {
        CheckWait(system, frame);
    }
    std::vector<Command> due;
    {
        std::scoped_lock lock{s.mutex};
        while (!s.commands.empty() &&
               (s.commands.front().immediate ||
                (!s.wait.active && s.commands.front().frame <= frame))) {
            due.push_back(std::move(s.commands.front()));
            s.commands.pop_front();
        }
    }
    for (const auto& command : due) {
        Execute(system, command);
    }
    SystemEvents::OnVBlank(system);

    // Expire timed inputs and rebuild the override.
    std::erase_if(s.held, [frame](const HeldInput& held) { return held.until_frame <= frame; });
    u32 buttons = s.sticky_buttons;
    for (const auto& held : s.held) {
        buttons |= held.buttons;
    }
    buttons |= SystemEvents::ForcedButtons();
    s.input.buttons = buttons;
    if (s.input.touch_active && s.touch_until <= frame) {
        s.input.touch_active = false;
    }
    if (s.input.stick_active && s.stick_until <= frame) {
        s.input.stick_active = false;
        s.input.stick_x = s.input.stick_y = 0;
    }
    if (s.input.cstick_active && s.cstick_until <= frame) {
        s.input.cstick_active = false;
        s.input.cstick_x = s.input.cstick_y = 0;
    }
}

const InputOverride& GetInputOverride() {
    return GetState().input;
}

bool IsActive() {
    return GetState().active;
}

void SetQuitHandler(std::function<void(int)> handler) {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.quit_handler = std::move(handler);
}

void Reset() {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.held.clear();
    s.sticky_buttons = 0;
    s.input = {};
}

bool DumpLcd(Core::System& system, int screen, const std::string& path) {
    if (!system.HasGPU()) {
        return false;
    }
    const auto& fb = system.GPU().PicaCore().regs.framebuffer_config[screen];
    const PAddr addr = fb.second_fb_active ? fb.address_left2 : fb.address_left1;
    const auto format = fb.color_format.Value();
    const u32 bpp = Pica::BytesPerPixel(format);
    // The LCD framebuffers are stored rotated: a "row" is one 240-pixel column of the screen.
    const u32 column_pixels = 240;
    const u32 columns = screen == 0 ? 400 : 320;
    const u32 stride = fb.stride != 0 ? fb.stride : column_pixels * bpp;
    const u32 size = stride * columns;
    if (addr == 0 || bpp == 0) {
        LOG_ERROR(Zeste, "automation: LCD {} has no framebuffer (address 0x{:08X})", screen, addr);
        return false;
    }
    FlushGpu(system, addr, size);

    std::vector<u8> pixels(static_cast<std::size_t>(columns) * column_pixels * 4);
    auto& memory = system.Memory();
    for (u32 column = 0; column < columns; ++column) {
        const u8* src = memory.GetPhysicalPointer(addr + column * stride);
        if (!src) {
            continue;
        }
        for (u32 i = 0; i < column_pixels; ++i) {
            const u8* p = src + i * bpp;
            Common::Vec4<u8> c;
            switch (format) {
            case Pica::PixelFormat::RGBA8:
                c = Common::Color::DecodeRGBA8(p);
                break;
            case Pica::PixelFormat::RGB8:
                c = Common::Color::DecodeRGB8(p);
                break;
            case Pica::PixelFormat::RGB565:
                c = Common::Color::DecodeRGB565(p);
                break;
            case Pica::PixelFormat::RGB5A1:
                c = Common::Color::DecodeRGB5A1(p);
                break;
            default:
                c = Common::Color::DecodeRGBA4(p);
                break;
            }
            // Pixel i of a column is counted from the bottom of the screen.
            const u32 x = column;
            const u32 y = column_pixels - 1 - i;
            u8* dst = &pixels[(static_cast<std::size_t>(y) * columns + x) * 4];
            dst[0] = c.r();
            dst[1] = c.g();
            dst[2] = c.b();
            dst[3] = 0xFF;
        }
    }
    auto image_interface = system.GetImageInterface();
    if (!image_interface) {
        LOG_ERROR(Zeste, "automation: no image encoder available for {}", path);
        return false;
    }
    const bool ok = image_interface->EncodePNG(path, columns, column_pixels, pixels);
    if (ok) {
        {
            auto& s = GetState();
            std::scoped_lock lock{s.mutex};
            s.screenshots.push_back(path);
        }
        LOG_INFO(Zeste, "automation: LCD {} (0x{:08X}, format {}) saved to {}",
                 screen == 0 ? "top" : "bottom", addr, static_cast<u32>(format), path);
    }
    return ok;
}

} // namespace Zeste::Automation
