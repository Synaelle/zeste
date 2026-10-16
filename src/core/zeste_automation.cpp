// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
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
#include "core/core.h"
#include "core/frontend/image_interface.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/zeste_automation.h"
#include "core/zeste_core.h"
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

struct State {
    std::mutex mutex;
    std::deque<Command> commands;
    std::vector<HeldInput> held;
    u32 sticky_buttons = 0;
    u64 stick_until = 0;
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

std::string ValidateCommand(const std::vector<std::string>& args) {
    static const std::vector<std::pair<std::string, std::size_t>> min_args = {
        {"press", 2},     {"hold", 2},      {"release", 2},   {"touch", 3},   {"stick", 3},
        {"lcd", 3},       {"screenshot", 2}, {"dump-mem", 4}, {"dump-phys", 4}, {"dump-vram", 2},
        {"dump-cmdlist", 2}, {"home", 1},   {"sleep", 1},     {"close", 1},   {"rosalina", 1},
        {"report", 1},    {"log", 1},       {"quit", 1},
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
        system.GPU().PicaCore().ZesteArmCommandListDump(args[1]);
    } else if (verb == "home" || verb == "sleep" || verb == "close" || verb == "rosalina") {
        LOG_WARNING(Zeste, "automation: '{}' is not available in this build yet", verb);
    } else if (verb == "report") {
        Zeste::WriteReport(args.size() > 1 ? args[1] : Zeste::DefaultReportBasePath(),
                           "on request");
    } else if (verb == "log") {
        std::string text;
        for (std::size_t i = 1; i < args.size(); ++i) {
            text += (i > 1 ? " " : "") + args[i];
        }
        LOG_INFO(Zeste, "script (frame {}): {}", frame, text);
    } else if (verb == "quit") {
        const int code = args.size() > 1 ? static_cast<int>(ParseSigned(args[1])) : 0;
        LOG_INFO(Zeste, "automation: quit (exit code {}) at frame {}", code, frame);
        if (s.quit_handler) {
            s.quit_handler(code);
        } else {
            system.RequestShutdown();
        }
    }
}

} // Anonymous namespace

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
            frame = ParseNumber(tokens[0].substr(1), frame);
            tokens.erase(tokens.begin());
        } else if (tokens[0][0] == '+') {
            frame += ParseNumber(tokens[0].substr(1), 0);
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
    auto& s = GetState();
    if (!s.active) {
        return;
    }
    const u64 frame = Zeste::CurrentFrame(system);
    std::vector<Command> due;
    {
        std::scoped_lock lock{s.mutex};
        while (!s.commands.empty() &&
               (s.commands.front().immediate || s.commands.front().frame <= frame)) {
            due.push_back(std::move(s.commands.front()));
            s.commands.pop_front();
        }
    }
    for (const auto& command : due) {
        Execute(system, command);
    }

    // Expire timed inputs and rebuild the override.
    std::erase_if(s.held, [frame](const HeldInput& held) { return held.until_frame <= frame; });
    u32 buttons = s.sticky_buttons;
    for (const auto& held : s.held) {
        buttons |= held.buttons;
    }
    s.input.buttons = buttons;
    if (s.input.touch_active && s.touch_until <= frame) {
        s.input.touch_active = false;
    }
    if (s.input.stick_active && s.stick_until <= frame) {
        s.input.stick_active = false;
        s.input.stick_x = s.input.stick_y = 0;
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
        LOG_INFO(Zeste, "automation: LCD {} (0x{:08X}, format {}) saved to {}",
                 screen == 0 ? "top" : "bottom", addr, static_cast<u32>(format), path);
    }
    return ok;
}

} // namespace Zeste::Automation
