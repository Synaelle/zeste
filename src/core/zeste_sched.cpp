// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <random>
#include "common/settings.h"
#include "common/zeste_report.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/thread.h"
#include "core/zeste_sched.h"

namespace Zeste::Sched {

namespace {

constexpr s64 MinRandomSlice = 200;
/// A ready thread that has not run for this long (emulated time) is reported as starved.
constexpr u64 StarvationTicks = BASE_CLOCK_RATE_ARM11; // 1 second

std::mt19937 g_rng{1};

u32 Mode() {
    return Settings::values.zeste_preempt_mode.GetValue();
}

} // Anonymous namespace

void Reset() {
    g_rng.seed(Settings::values.zeste_preempt_seed.GetValue());
}

s64 AdjustSlice(s64 max_slice) {
    if (Mode() != 2 || max_slice <= MinRandomSlice) {
        return max_slice;
    }
    std::uniform_int_distribution<s64> dist{MinRandomSlice, max_slice};
    return dist(g_rng);
}

u32 ExtraSteps() {
    if (Mode() != 2) {
        return 0;
    }
    return std::uniform_int_distribution<u32>{0, 7}(g_rng);
}

void OnSvc(Core::System& system) {
    if (Mode() == 1) {
        system.PrepareReschedule();
    }
}

void OnVBlank(Core::System& system) {
    if (!Settings::values.zeste_strict_priority.GetValue() || !system.HasKernel()) {
        return;
    }
    auto& kernel = system.Kernel();
    for (u32 core = 0; core < system.GetNumCores(); ++core) {
        auto& manager = kernel.GetThreadManager(core);
        const auto* running = manager.GetCurrentThread();
        const u64 now = system.GetCore(core).GetTimer().GetTicks();
        for (const auto& thread : manager.GetThreadList()) {
            if (thread->status != Kernel::ThreadStatus::Ready || thread->last_running_ticks == 0 ||
                now < thread->last_running_ticks + StarvationTicks) {
                continue;
            }
            const u64 ms = (now - thread->last_running_ticks) * 1000 / BASE_CLOCK_RATE_ARM11;
            ReportF(Category::Scheduling, fmt::format("starved-{}", thread->GetThreadId()),
                    "thread {} '{}' (priority {}, core {}) has been ready for {} ms without "
                    "running{}: a strict-priority 3DS core never runs it while a higher-priority "
                    "thread keeps computing",
                    thread->GetThreadId(), thread->GetName(), thread->current_priority, core, ms,
                    running ? fmt::format(" (core busy with thread {} '{}', priority {})",
                                          running->GetThreadId(), running->GetName(),
                                          running->current_priority)
                            : "");
        }
    }
}

} // namespace Zeste::Sched
