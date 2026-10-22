// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/zeste_report.h"
#include "core/zeste_automation.h"
#include "core/zeste_exceptions.h"
#include "core/zeste_memory.h"
#include "core/zeste_profiler.h"
#include "core/zeste_summary.h"

namespace Zeste::Summary {

namespace {

struct Symbol {
    u32 start;
    u32 size;
    std::string name;
};

std::mutex g_mutex;
std::string g_elf_path;
bool g_elf_loaded = false;
std::vector<Symbol> g_symbols; // sorted by start
u64 g_frames = 0;

template <typename T>
T ReadAt(const std::string& data, std::size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

/// Reads `size` bytes at `offset` of an open file ("" on failure).
std::string ReadRange(FileUtil::IOFile& file, u64 offset, std::size_t size) {
    std::string out(size, '\0');
    if (!file.Seek(static_cast<s64>(offset), SEEK_SET) ||
        file.ReadBytes(out.data(), size) != size) {
        return {};
    }
    return out;
}

/// Loads the function symbols of a 32-bit little-endian ARM ELF (.symtab + its string table).
/// Only those sections are read: a game's ELF with debug info can be over a gigabyte.
void LoadElf() {
    g_elf_loaded = true;
    if (g_elf_path.empty()) {
        return;
    }
    FileUtil::IOFile file(g_elf_path, "rb");
    const std::string header = file.IsOpen() ? ReadRange(file, 0, 52) : std::string{};
    if (header.size() < 52 || header.compare(0, 4, "\x7F" "ELF") != 0 || header[4] != 1) {
        return;
    }
    const u32 shoff = ReadAt<u32>(header, 0x20);
    const u16 shentsize = ReadAt<u16>(header, 0x2E);
    const u16 shnum = ReadAt<u16>(header, 0x30);
    const std::string sections =
        ReadRange(file, shoff, static_cast<std::size_t>(shnum) * shentsize);
    for (u16 i = 0; i < shnum && !sections.empty(); ++i) {
        const std::size_t sh = static_cast<std::size_t>(i) * shentsize;
        if (ReadAt<u32>(sections, sh + 4) != 2) { // SHT_SYMTAB
            continue;
        }
        const u32 link = ReadAt<u32>(sections, sh + 0x18);
        const std::size_t str_sh = static_cast<std::size_t>(link) * shentsize;
        const std::string symtab =
            ReadRange(file, ReadAt<u32>(sections, sh + 0x10), ReadAt<u32>(sections, sh + 0x14));
        const std::string strtab = ReadRange(file, ReadAt<u32>(sections, str_sh + 0x10),
                                             ReadAt<u32>(sections, str_sh + 0x14));
        for (std::size_t at = 0; at + 16 <= symtab.size(); at += 16) {
            const u32 name = ReadAt<u32>(symtab, at);
            const u32 value = ReadAt<u32>(symtab, at + 4);
            const u32 sym_size = ReadAt<u32>(symtab, at + 8);
            const u8 info = ReadAt<u8>(symtab, at + 12);
            if ((info & 0xF) != 2 || value == 0 || name >= strtab.size()) { // STT_FUNC
                continue;
            }
            g_symbols.push_back({value & ~1u, sym_size, std::string(strtab.c_str() + name)});
        }
    }
    std::sort(g_symbols.begin(), g_symbols.end(),
              [](const Symbol& a, const Symbol& b) { return a.start < b.start; });
}

std::string Hex(u32 v) {
    return fmt::format("0x{:08X}", v);
}

std::string WithName(u32 address) {
    const std::string name = Symbolize(address);
    return name.empty() ? Hex(address) : fmt::format("{} {}", Hex(address), name);
}

/// The last `count` lines of a text file.
std::vector<std::string> Tail(const std::string& path, std::size_t count) {
    std::string text;
    FileUtil::ReadFileToString(true, path, text);
    std::deque<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (lines.size() > count) {
            lines.pop_front();
        }
        start = end + 1;
    }
    return {lines.begin(), lines.end()};
}

} // Anonymous namespace

void SetElfPath(const std::string& path) {
    std::scoped_lock lock{g_mutex};
    g_elf_path = path;
    g_elf_loaded = false;
    g_symbols.clear();
}

std::string Symbolize(u32 address) {
    std::scoped_lock lock{g_mutex};
    if (!g_elf_loaded) {
        LoadElf();
    }
    const u32 a = address & ~1u;
    auto it = std::upper_bound(g_symbols.begin(), g_symbols.end(), a,
                               [](u32 v, const Symbol& s) { return v < s.start; });
    if (it == g_symbols.begin()) {
        return {};
    }
    --it;
    if (it->size != 0 && a >= it->start + it->size) {
        return {};
    }
    return a == it->start ? it->name : fmt::format("{}+0x{:X}", it->name, a - it->start);
}

std::string FunctionName(u32 address) {
    const std::string name = Symbolize(address);
    return name.substr(0, name.find("+0x"));
}

bool HasElf() {
    std::scoped_lock lock{g_mutex};
    if (!g_elf_loaded) {
        LoadElf();
    }
    return !g_symbols.empty();
}

void SetFramesRun(u64 frames) {
    g_frames = frames;
}

void Write(const std::string& folder, int exit_code, const std::string& reason) {
    if (folder.empty()) {
        return;
    }
    const auto& crash = Exceptions::LastCrash();
    std::string reason_text =
        crash.valid ? fmt::format("the game crashed: {} at 0x{:08X}, pc {}", crash.type,
                                  crash.fault_address, WithName(crash.regs[15]))
                    : Automation::ExitReason();
    if (reason_text.empty()) {
        reason_text = reason;
    }
    if (reason_text.size() > 300) {
        reason_text = reason_text.substr(0, 300) + "... (see the log)";
    }
    std::replace(reason_text.begin(), reason_text.end(), '\n', ' ');
    std::replace(reason_text.begin(), reason_text.end(), '\r', ' ');
    std::string out = fmt::format("# Zeste run summary\n\n"
                                  "- Exit code: {}\n- Why the run ended: {}\n- Frames run: {} "
                                  "({:.1f} s at 60 Hz)\n- Run folder: {}\n\n",
                                  exit_code, reason_text, g_frames, g_frames / 60.0, folder);

    out += "## Findings\n\n";
    u64 total = 0;
    for (u32 i = 0; i < static_cast<u32>(Category::Count); ++i) {
        const u64 n = Count(static_cast<Category>(i));
        if (n != 0) {
            out += fmt::format("- {}: {}\n", CategoryName(static_cast<Category>(i)), n);
            total += n;
        }
    }
    out += total ? "\nDetails: zeste_report.md.\n\n" : "None (only enabled checks run).\n\n";

    if (const std::string memory = MemoryBudget::SummaryText(); !memory.empty()) {
        out += "## Memory\n\n" + memory + "\n";
    }

    if (crash.valid) {
        out += fmt::format("## Crash\n\n{} at {}, faulting instruction {}\n\n", crash.type,
                           Hex(crash.fault_address), WithName(crash.regs[15]));
        out += "```\n";
        for (u32 i = 0; i < 16; ++i) {
            static constexpr const char* Names[16] = {"r0", "r1", "r2",  "r3",  "r4",  "r5",
                                                      "r6", "r7", "r8",  "r9",  "r10", "r11",
                                                      "r12", "sp", "lr", "pc"};
            out += fmt::format("{:>3} {:08X}{}", Names[i], crash.regs[i], i % 4 == 3 ? "\n" : "  ");
        }
        out += fmt::format("cpsr {:08X}\n```\n\n", crash.cpsr);
        out += "Call chain (pc, lr, then return addresses found on the stack: a scan, not a "
               "frame-pointer walk, so stale entries are possible):\n\n";
        out += fmt::format("1. {}\n2. {} (lr)\n", WithName(crash.regs[15]),
                           WithName(crash.regs[14]));
        int n = 3;
        for (const u32 ret : crash.stack_returns) {
            out += fmt::format("{}. {}\n", n++, WithName(ret));
        }
        if (g_elf_path.empty()) {
            out += "\n(Pass --elf <game.elf> for function names.)\n";
        }
        out += "\n";
    }

    const auto debug = Automation::RecentDebugOutput();
    if (!debug.empty()) {
        out += "## Last lines of the game's debug output\n\n```\n";
        for (const auto& line : debug) {
            out += line + (line.empty() || line.back() != '\n' ? "\n" : "");
        }
        out += "```\n\n";
    }

    out += "## Last lines of the log\n\n```\n";
    for (const auto& line : Tail(folder + "azahar_log.txt", 30)) {
        out += line + "\n";
    }
    out += "```\n\n";

    const auto profiles = Profiler::Reports();
    if (!profiles.empty()) {
        out += "## CPU profiles\n\n";
        for (const auto& profile : profiles) {
            out += "- " + profile + "\n";
        }
        out += "\n";
    }

    const auto shots = Automation::Screenshots();
    out += "## Screenshots\n\n";
    if (shots.empty()) {
        out += "None.\n";
    }
    for (const auto& shot : shots) {
        out += "- " + shot + "\n";
    }
    FileUtil::WriteStringToFile(true, folder + "summary.md", out);
}

} // namespace Zeste::Summary
