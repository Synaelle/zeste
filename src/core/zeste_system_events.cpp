// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <optional>
#include "common/logging/log.h"
#include "common/zeste_report.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/service/apt/applet_manager.h"
#include "core/hle/service/apt/apt.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/sm/sm.h"
#include "core/zeste_automation.h"
#include "core/zeste_core.h"
#include "core/zeste_system_events.h"

namespace Zeste::SystemEvents {

namespace {

enum class Kind { None, Home, Sleep, Close, Rosalina };

struct State {
    Kind kind = Kind::None;
    int phase = 0;
    u64 next_frame = 0;
    u64 amount = 0;
    std::optional<u32> sleep_reply;
    std::weak_ptr<Kernel::Process> paused_process;
    u32 forced_buttons = 0;
};

State g_state;

constexpr u32 FramesPerSecond = 60;
/// SleepQueryReply values (APT): 0 = reject, 1 = accept, 2 = later.
constexpr u32 SleepReplyAccept = 1;

std::shared_ptr<Service::APT::AppletManager> GetApplets(Core::System& system) {
    const auto apt = Service::APT::GetModule(system);
    return apt ? apt->GetAppletManager() : nullptr;
}

void PauseApplication(Core::System& system, bool pause) {
    auto& kernel = system.Kernel();
    if (pause) {
        auto process = kernel.GetCurrentProcess();
        if (process) {
            process->SetUnscheduleMode(Kernel::UnscheduleMode::FRONTEND);
            g_state.paused_process = process;
        }
    } else if (auto process = g_state.paused_process.lock()) {
        process->ClearUnscheduleMode(Kernel::UnscheduleMode::FRONTEND);
        g_state.paused_process.reset();
    }
    system.PrepareReschedule();
}

void Finish() {
    g_state.kind = Kind::None;
    g_state.phase = 0;
    g_state.forced_buttons = 0;
}

} // Anonymous namespace

void Reset() {
    g_state = {};
}

u32 ForcedButtons() {
    return g_state.forced_buttons;
}

void OnSleepQueryReply(u32 reply) {
    g_state.sleep_reply = reply;
}

void Start(Core::System& system, const std::string& event, u64 amount) {
    if (g_state.kind != Kind::None) {
        LOG_WARNING(Zeste, "system event '{}' ignored: another event is still running", event);
        return;
    }
    const u64 frame = CurrentFrame(system);
    auto applets = GetApplets(system);
    if (!applets) {
        LOG_ERROR(Zeste, "system event '{}': APT is not available", event);
        return;
    }
    g_state.phase = 0;
    g_state.amount = amount;
    if (event == "home") {
        g_state.kind = Kind::Home;
        g_state.amount = amount ? amount : 120;
        g_state.next_frame = frame + 120; // time allowed to answer the HOME button
        LOG_INFO(Zeste, "event: HOME button (simulated HOME Menu for {} frames)", g_state.amount);
        applets->ZesteSetSimulatedHomeMenu(true);
        applets->SendNotification(Service::APT::Notification::HomeButtonSingle);
    } else if (event == "sleep") {
        g_state.kind = Kind::Sleep;
        g_state.amount = amount ? amount : 120;
        g_state.sleep_reply.reset();
        g_state.next_frame = frame + 30;
        LOG_INFO(Zeste, "event: lid closed (sleep query)");
        applets->SendNotification(Service::APT::Notification::SleepQuery);
    } else if (event == "close") {
        g_state.kind = Kind::Close;
        g_state.next_frame = frame + 5 * FramesPerSecond;
        LOG_INFO(Zeste, "event: HOME Menu close (OrderToClose)");
        applets->SendNotification(Service::APT::Notification::OrderToClose);
    } else if (event == "rosalina") {
        g_state.kind = Kind::Rosalina;
        g_state.amount = amount ? amount : 3000;
        g_state.forced_buttons = Automation::BUTTON_L | Automation::BUTTON_DOWN |
                                 Automation::BUTTON_SELECT;
        g_state.next_frame = frame + 5;
        LOG_INFO(Zeste, "event: Rosalina combination (L+Down+Select), then {} ms paused",
                 g_state.amount);
    }
}

void OnVBlank(Core::System& system) {
    if (g_state.kind == Kind::None) {
        return;
    }
    const u64 frame = CurrentFrame(system);
    auto applets = GetApplets(system);
    if (!applets) {
        Finish();
        return;
    }

    switch (g_state.kind) {
    case Kind::Home:
        if (g_state.phase == 0) {
            if (applets->ZesteIsInHomeMenu()) {
                auto gsp = system.ServiceManager().GetService<Service::GSP::GSP_GPU>("gsp::Gpu");
                if (gsp && gsp->ZesteIsGpuRightHeld()) {
                    ReportF(Category::System, "home-gpu-right",
                            "the application jumped to the HOME Menu while still holding the GPU "
                            "right (no GSPGPU_ReleaseRight): the HOME Menu could not use the GPU");
                }
                LOG_INFO(Zeste, "event: application suspended in the HOME Menu at frame {}",
                         frame);
                g_state.phase = 1;
                g_state.next_frame = frame + g_state.amount;
            } else if (frame >= g_state.next_frame) {
                LOG_WARNING(Zeste, "event: the application did not answer the HOME button "
                                   "within 2 s (HOME disallowed?)");
                applets->ZesteSetSimulatedHomeMenu(false);
                Finish();
            }
        } else if (frame >= g_state.next_frame) {
            LOG_INFO(Zeste, "event: leaving the simulated HOME Menu at frame {}", frame);
            applets->ZesteLeaveSimulatedHomeMenu();
            applets->ZesteSetSimulatedHomeMenu(false);
            Finish();
        }
        break;

    case Kind::Sleep:
        if (frame < g_state.next_frame) {
            break;
        }
        if (g_state.phase == 0) {
            if (g_state.sleep_reply && *g_state.sleep_reply != SleepReplyAccept) {
                LOG_INFO(Zeste, "event: the application refused to sleep (reply {})",
                         *g_state.sleep_reply);
                Finish();
                break;
            }
            applets->SendNotification(Service::APT::Notification::SleepAccepted);
            g_state.phase = 1;
            g_state.next_frame = frame + 10;
        } else if (g_state.phase == 1) {
            LOG_INFO(Zeste, "event: asleep for {} frames", g_state.amount);
            PauseApplication(system, true);
            g_state.phase = 2;
            g_state.next_frame = frame + g_state.amount;
        } else {
            PauseApplication(system, false);
            applets->SendNotification(Service::APT::Notification::SleepAwake);
            LOG_INFO(Zeste, "event: woke up at frame {}", frame);
            Finish();
        }
        break;

    case Kind::Close:
        if (frame >= g_state.next_frame) {
            ReportF(Category::System, "close-ignored",
                    "the application was still running 5 s after the HOME Menu asked it to close "
                    "(OrderToClose): on hardware the HOME Menu waits for it");
            Finish();
        }
        break;

    case Kind::Rosalina:
        if (frame < g_state.next_frame) {
            break;
        }
        if (g_state.phase == 0) {
            g_state.forced_buttons = 0;
            PauseApplication(system, true);
            g_state.phase = 1;
            g_state.next_frame = frame + g_state.amount * FramesPerSecond / 1000;
        } else {
            PauseApplication(system, false);
            LOG_INFO(Zeste, "event: Rosalina menu closed at frame {}", frame);
            Finish();
        }
        break;

    case Kind::None:
        break;
    }
}

} // namespace Zeste::SystemEvents
