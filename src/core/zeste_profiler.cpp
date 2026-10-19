// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/zeste_run.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/zeste_core.h"
#include "core/zeste_profiler.h"
#include "core/zeste_summary.h"

namespace Zeste::Profiler {

namespace {

struct Key {
    u32 core;
    u32 thread; ///< thread id, 0 when the core is idle (no thread)
    u32 pc;
    u32 lr;
    bool operator==(const Key&) const = default;
};

struct KeyHash {
    std::size_t operator()(const Key& k) const {
        u64 h = (static_cast<u64>(k.core) << 56) ^ (static_cast<u64>(k.thread) << 32) ^ k.pc;
        h ^= static_cast<u64>(k.lr) * 0x9E3779B97F4A7C15ull;
        return std::hash<u64>{}(h);
    }
};

struct ThreadInfo {
    std::string process;
    u32 priority = 0;
};

std::mutex g_mutex;
bool g_active = false;
u64 g_samples_taken = 0;     ///< total, to notice when sampling stopped (save state load)
u64 g_samples_at_frame = 0;  ///< g_samples_taken at the previous VBlank
u32 g_quiet_frames = 0;
u32 g_generation = 0; ///< bumped on every start and stop: events of an older run end themselves
s64 g_interval_cycles = 0;
u32 g_interval_us = 0;
u64 g_start_frame = 0;
Core::TimingEventType* g_event = nullptr;
std::unordered_map<Key, u64, KeyHash> g_samples;
std::map<std::pair<u32, u32>, ThreadInfo> g_threads; ///< (core, thread id)
std::vector<std::string> g_reports;

void Sample(Core::System& system, u32 core) {
    if (!system.IsPoweredOn() || core >= system.GetNumCores()) {
        return;
    }
    auto& cpu = system.GetCore(core);
    const Kernel::Thread* thread = system.Kernel().GetThreadManager(core).GetCurrentThread();
    Key key{core, 0, 0, 0};
    if (thread) {
        key.thread = thread->thread_id;
        key.pc = cpu.GetPC();
        key.lr = cpu.GetReg(14);
    }
    std::scoped_lock lock{g_mutex};
    ++g_samples[key];
    ++g_samples_taken;
    if (thread && !g_threads.contains({core, key.thread})) {
        const auto process = thread->owner_process.lock();
        g_threads[{core, key.thread}] = {process ? process->GetName() : std::string{"?"},
                                         thread->current_priority};
    }
}

void Schedule(Core::System& system, u32 core, s64 cycles) {
    system.CoreTiming().ScheduleEvent(std::max<s64>(cycles, 1), g_event,
                                      (static_cast<std::uintptr_t>(g_generation) << 8) | core,
                                      core);
}

/// (Re)starts the sampling events on every core. The event is registered on the current Timing
/// each time: a boot or a save state load creates a new one (whose restored queue may even hold
/// our events, without a callback until this runs). Older events end themselves (generation).
void Arm(Core::System& system) {
    ++g_generation;
    RegisterEvent(system);
    for (u32 core = 0; core < system.GetNumCores(); ++core) {
        Schedule(system, core, g_interval_cycles);
    }
}

/// Function name for an address: the ELF symbol, else the address itself.
std::string Function(u32 address) {
    const std::string name = Summary::FunctionName(address);
    return name.empty() ? fmt::format("0x{:08X}", address & ~1u) : name;
}

std::string ThreadLabel(u32 core, u32 thread) {
    if (thread == 0) {
        return "idle";
    }
    const auto it = g_threads.find({core, thread});
    return it == g_threads.end()
               ? fmt::format("thread {}", thread)
               : fmt::format("thread {} ({}, priority 0x{:X})", thread, it->second.process,
                             it->second.priority);
}

std::string Percent(u64 part, u64 whole) {
    return whole == 0 ? "0.0" : fmt::format("{:.1f}", 100.0 * part / whole);
}

void WriteFiles(Core::System& system, const std::string& base) {
    // Per sample: function of the PC, and the caller from LR (approximate: LR holds the return
    // address into the caller only until the function itself makes a call).
    struct Row {
        std::string function;
        std::string caller;
    };
    std::map<u32, u64> core_total, core_idle;
    std::map<u32, std::map<std::string, u64>> functions;                // core -> function
    std::map<std::pair<u32, u32>, u64> thread_total;                    // (core, thread)
    std::map<std::pair<u32, u32>, std::map<std::string, u64>> by_thread; // (core, thread) -> fn
    std::map<std::string, u64> folded;
    std::map<u32, std::string> names; // memoized symbolization per address
    const auto name_of = [&](u32 address) -> const std::string& {
        auto it = names.find(address);
        if (it == names.end()) {
            it = names.emplace(address, Function(address)).first;
        }
        return it->second;
    };

    for (const auto& [key, count] : g_samples) {
        core_total[key.core] += count;
        if (key.thread == 0) {
            core_idle[key.core] += count;
            continue;
        }
        const std::string& fn = name_of(key.pc);
        const std::string& caller_fn = name_of(key.lr);
        // LR is also a scratch register: keep it only when it points at code (a function of the
        // ELF, or the application's code range without one).
        const bool has_caller = Summary::HasElf()
                                    ? !Summary::FunctionName(key.lr).empty()
                                    : key.lr >= 0x00100000 && key.lr < 0x08000000;
        functions[key.core][fn] += count;
        thread_total[{key.core, key.thread}] += count;
        by_thread[{key.core, key.thread}][fn] += count;
        std::string stack = fmt::format("core {};{}", key.core, ThreadLabel(key.core, key.thread));
        if (has_caller && caller_fn != fn) {
            stack += ";" + caller_fn;
        }
        stack += ";" + fn;
        folded[stack] += count;
    }

    const u64 frames = CurrentFrame(system) - g_start_frame;
    std::string md = "# Zeste CPU profile\n\n";
    md += fmt::format(
        "- One sample every {} us of emulated time on each core, over {} frames ({:.1f} s at "
        "60 Hz).\n",
        g_interval_us, frames, frames / 60.0);
    md += "- **Emulated time is not console time.** Zeste runs instructions at a flat rate, "
          "without the console's caches, memory waits or pipeline, so the percentages show "
          "where the instructions go, not where the console spends its time.\n";
    md += Summary::HasElf()
              ? "- Function names come from the ELF given with `--elf`. C++ names are mangled: "
                "`arm-none-eabi-c++filt` (devkitARM) demangles them.\n"
              : "- No `--elf` given: functions are shown as addresses (one row per address).\n";
    md += "- \"Caller\" comes from LR, so it is approximate: LR holds the return address into "
          "the caller only until the function itself calls something.\n";
    md += fmt::format("- Collapsed stacks for flame-graph tools: `{}.folded`.\n\n",
                      base.substr(base.find_last_of("/\\") + 1));

    for (const auto& [core, total] : core_total) {
        const u64 idle = core_idle[core];
        if (total == idle) {
            md += fmt::format("## Core {}\n\n{} samples, all idle.\n\n", core, total);
            continue;
        }
        md += fmt::format("## Core {}\n\n{} samples, {}% idle (no thread running).\n\n", core,
                          total, Percent(idle, total));
        md += "### Threads\n\n| Thread | Samples | % of the core |\n|---|---:|---:|\n";
        for (const auto& [ct, count] : thread_total) {
            if (ct.first == core) {
                md += fmt::format("| {} | {} | {} |\n", ThreadLabel(core, ct.second), count,
                                  Percent(count, total));
            }
        }
        std::vector<std::pair<std::string, u64>> hot(functions[core].begin(),
                                                     functions[core].end());
        std::sort(hot.begin(), hot.end(), [](auto& a, auto& b) { return a.second > b.second; });
        md += "\n### Hottest functions (self, all threads)\n\n| % of busy | % of the core | Samples "
              "| Function |\n|---:|---:|---:|---|\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(hot.size(), 40); ++i) {
            md += fmt::format("| {} | {} | {} | `{}` |\n", Percent(hot[i].second, total - idle),
                              Percent(hot[i].second, total), hot[i].second, hot[i].first);
        }
        for (const auto& [ct, fns] : by_thread) {
            if (ct.first != core || thread_total[ct] * 20 < total - idle) {
                continue; // threads under 5% of the busy samples
            }
            std::vector<std::pair<std::string, u64>> top(fns.begin(), fns.end());
            std::sort(top.begin(), top.end(),
                      [](auto& a, auto& b) { return a.second > b.second; });
            md += fmt::format("\n### {}: hottest functions\n\n| % of the thread | Samples | "
                              "Function |\n|---:|---:|---|\n",
                              ThreadLabel(core, ct.second));
            for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 20); ++i) {
                md += fmt::format("| {} | {} | `{}` |\n", Percent(top[i].second, thread_total[ct]),
                                  top[i].second, top[i].first);
            }
        }
        md += "\n";
    }

    std::string folded_text;
    for (const auto& [stack, count] : folded) {
        folded_text += fmt::format("{} {}\n", stack, count);
    }
    FileUtil::CreateFullPath(base);
    FileUtil::WriteStringToFile(true, base + ".md", md);
    FileUtil::WriteStringToFile(true, base + ".folded", folded_text);
}

} // namespace

void RegisterEvent(Core::System& system) {
    g_event = system.CoreTiming().RegisterEvent(
        "ZesteProfilerSample", [&system](std::uintptr_t data, int cycles_late) {
            const u32 core = static_cast<u32>(data & 0xFF);
            if (!g_active || (data >> 8) != g_generation) {
                return;
            }
            Sample(system, core);
            Schedule(system, core, g_interval_cycles - cycles_late);
        });
}

void Start(Core::System& system, u32 interval_us) {
    {
        std::scoped_lock lock{g_mutex};
        g_samples.clear();
        g_threads.clear();
        g_interval_us = std::max<u32>(interval_us, 10);
        g_interval_cycles = usToCycles(static_cast<s64>(g_interval_us));
        g_start_frame = CurrentFrame(system);
        g_active = true;
    }
    Arm(system);
    LOG_INFO(Zeste, "profiler: sampling every {} us of emulated time on {} cores", g_interval_us,
             system.GetNumCores());
}

std::string Stop(Core::System& system, const std::string& base_path) {
    {
        std::scoped_lock lock{g_mutex};
        if (!g_active) {
            return {};
        }
        g_active = false;
        ++g_generation;
    }
    std::scoped_lock lock{g_mutex};
    if (g_samples.empty()) {
        return {};
    }
    WriteFiles(system, base_path);
    const std::string md = base_path + ".md";
    g_reports.push_back(md);
    Run::Note("profile", md);
    LOG_INFO(Zeste, "profiler: report written to {}", md);
    return md;
}

void OnVBlank(Core::System& system) {
    if (!g_active) {
        return;
    }
    // Loading a save state replaces the timing event queue, which drops the sampling events.
    if (g_samples_taken != g_samples_at_frame) {
        g_samples_at_frame = g_samples_taken;
        g_quiet_frames = 0;
        return;
    }
    if (++g_quiet_frames < 3) {
        return;
    }
    g_quiet_frames = 0;
    Arm(system);
    LOG_INFO(Zeste, "profiler: sampling resumed (the event queue was replaced, save state load?)");
}

bool Active() {
    return g_active;
}

void OnShutdown(Core::System& system) {
    if (g_active && !Run::Folder().empty()) {
        Stop(system, Run::Folder() + "profile");
    }
    g_active = false;
    g_event = nullptr;
}

std::vector<std::string> Reports() {
    std::scoped_lock lock{g_mutex};
    return g_reports;
}

} // namespace Zeste::Profiler
