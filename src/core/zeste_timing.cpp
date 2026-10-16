// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <map>
#include <mutex>
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/zeste_report.h"
#include "core/core_timing.h"
#include "core/zeste_timing.h"

namespace Zeste::Timing {

namespace {

constexpr u64 Ms = 1'000'000;
constexpr u64 SdRequestNs = 2 * Ms;
constexpr u64 SdReadNsPerByte = 100;   // ~10 MB/s
constexpr u64 SdWriteNsPerByte = 1600; // ~0.6 MB/s

thread_local u64 g_pending_sd_ns = 0;

std::mutex g_delete_mutex;
u64 g_delete_sequence = 0;
std::map<std::string, u64> g_last_delete_in_dir;
std::map<std::string, int> g_open_dirs;

void ReportRemoveAll(const std::string& host_dir) {
    ReportF(Category::System, "dir-delete-while-listing@" + host_dir,
            "entries of {} were deleted while a listing of that directory was still open: this "
            "is what std::filesystem::remove_all does, and it never returns on hardware (collect "
            "the names, close the directory, then delete)",
            host_dir);
}

} // Anonymous namespace

bool SdEnabled() {
    return Settings::values.zeste_sd_latency.GetValue();
}

u64 SdReadNs(u64 length) {
    return SdRequestNs + length * SdReadNsPerByte;
}

u64 SdWriteNs(u64 length) {
    return SdRequestNs + length * SdWriteNsPerByte;
}

u64 SdOpenNs() {
    return 6 * Ms;
}

u64 SdCreateNs() {
    return 1500 * Ms;
}

u64 SdDeleteNs() {
    return 30 * Ms;
}

void AddSdDelayNs(u64 ns) {
    if (SdEnabled()) {
        g_pending_sd_ns += ns;
    }
}

u64 TakeSdDelayNs() {
    const u64 ns = g_pending_sd_ns;
    g_pending_sd_ns = 0;
    return ns;
}

void OnSdDelete(const std::string& host_dir) {
    if (!SdEnabled()) {
        return;
    }
    bool listing_open = false;
    {
        std::scoped_lock lock{g_delete_mutex};
        g_last_delete_in_dir[host_dir] = ++g_delete_sequence;
        const auto it = g_open_dirs.find(host_dir);
        listing_open = it != g_open_dirs.end() && it->second > 0;
    }
    if (listing_open) {
        ReportRemoveAll(host_dir);
    }
}

void OnSdDirectoryOpen(const std::string& host_dir) {
    if (!SdEnabled()) {
        return;
    }
    std::scoped_lock lock{g_delete_mutex};
    ++g_open_dirs[host_dir];
}

void OnSdDirectoryClose(const std::string& host_dir) {
    std::scoped_lock lock{g_delete_mutex};
    if (auto it = g_open_dirs.find(host_dir); it != g_open_dirs.end() && --it->second <= 0) {
        g_open_dirs.erase(it);
    }
}

u64 SdDeleteSequence() {
    std::scoped_lock lock{g_delete_mutex};
    return g_delete_sequence;
}

void OnSdDirectoryRead(const std::string& host_dir, u64 open_sequence) {
    if (!SdEnabled()) {
        return;
    }
    {
        std::scoped_lock lock{g_delete_mutex};
        const auto it = g_last_delete_in_dir.find(host_dir);
        if (it == g_last_delete_in_dir.end() || it->second <= open_sequence) {
            return;
        }
    }
    ReportRemoveAll(host_dir);
}

void SetCpuSpeedup(Core::Timing& timing, bool speedup) {
    if (!Settings::values.zeste_cpu_speed_model.GetValue()) {
        return;
    }
    const s32 base = Settings::values.cpu_clock_percentage.GetValue();
    const u32 percentage = static_cast<u32>(base * (speedup ? 3 : 1));
    timing.UpdateClockSpeed(percentage);
    LOG_INFO(Zeste, "CPU speed model: {} MHz (clock scale {}%)", speedup ? 804 : 268, percentage);
}

} // namespace Zeste::Timing
