// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/zeste_report.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/gdbstub/gdbstub.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/thread.h"
#include "core/zeste_automation.h"
#include "core/zeste_cache.h"
#include "core/zeste_core.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace Zeste {

namespace {
std::string g_report_base_path;
std::string g_title;
bool g_installed = false;
bool g_break_requested = false;
} // Anonymous namespace

u64 CurrentFrame(Core::System& system) {
    if (!system.HasGPU()) {
        return 0;
    }
    auto& gpu = system.GPU();
    if (!gpu.HasRenderer()) {
        return 0;
    }
    return gpu.Renderer().GetCurrentFrame();
}

std::string DefaultReportBasePath() {
    if (!g_report_base_path.empty()) {
        return g_report_base_path;
    }
    return FileUtil::GetUserPath(FileUtil::UserPath::LogDir) + "zeste_report";
}

void SetReportBasePath(const std::string& path) {
    g_report_base_path = path;
}

bool ConsumeBreakRequest() {
    const bool requested = g_break_requested;
    g_break_requested = false;
    return requested;
}

void InstallCoreHooks(Core::System& system) {
    Reset();
    Cache::Reset();
    g_installed = true;
    SetLogLimit(Settings::values.zeste_report_log_limit.GetValue());
    SetBreakOnFirst(Settings::values.zeste_break_on_first.GetValue());

    g_title = fmt::format("{} (title {:016X})", system.GetFilePath(), system.GetTitleID());

    SetContextProvider([&system]() {
        Context ctx;
        if (!system.IsPoweredOn() || !system.HasRunningCore()) {
            return ctx;
        }
        auto& core = system.GetRunningCore();
        ctx.pc = core.GetPC();
        ctx.lr = core.GetReg(14);
        ctx.core_id = core.GetID();
        if (system.HasKernel()) {
            if (const auto* thread =
                    system.Kernel().GetCurrentThreadManager().GetCurrentThread()) {
                ctx.thread_id = thread->GetThreadId();
            }
        }
        ctx.frame = CurrentFrame(system);
        ctx.valid = true;
        return ctx;
    });

    SetBreakHandler([&system](Category, const std::string& text) {
        g_break_requested = true;
#ifdef ENABLE_GDBSTUB
        if (GDBStub::IsConnected()) {
            GDBStub::Break(5); // SIGTRAP
            return;
        }
#endif
        system.SetStatus(Core::System::ResultStatus::ZesteBreak, text.c_str());
        system.PrepareReschedule();
    });

    if (Settings::IsAnyZesteOptionEnabled()) {
        LOG_INFO(Zeste, "Zeste hardware-accuracy options active for {}", g_title);
    }
}

void OnSystemShutdown(Core::System& system) {
    if (!g_installed) {
        return;
    }
    g_installed = false;
    Automation::Reset();
    SetContextProvider({});
    SetBreakHandler({});
    if (!Settings::values.zeste_write_report.GetValue()) {
        return;
    }
    u64 total = 0;
    for (u32 i = 0; i < static_cast<u32>(Category::Count); ++i) {
        total += Count(static_cast<Category>(i));
    }
    if (!Settings::IsAnyZesteOptionEnabled() && total == 0) {
        return;
    }
    WriteReport(DefaultReportBasePath(),
                fmt::format("{} | Zeste {} {}", g_title, Common::g_build_fullname,
                            Common::g_scm_desc));
}

} // namespace Zeste
