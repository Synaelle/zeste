// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <map>
#include <mutex>
#include "common/settings.h"
#include "common/zeste_report.h"
#include "core/zeste_fs.h"

namespace Zeste::Fs {

namespace {

std::mutex g_mutex;
u64 g_delete_sequence = 0;
std::map<std::string, u64> g_last_delete_in_dir;
std::map<std::string, int> g_open_dirs;

bool Enabled() {
    return Settings::values.zeste_fs_checks.GetValue();
}

void ReportRemoveAll(const std::string& host_dir) {
    ReportF(Category::System, "dir-delete-while-listing@" + host_dir,
            "entries of {} were deleted while a listing of that directory was still open: this "
            "is what std::filesystem::remove_all does, and it never returns on hardware (collect "
            "the names, close the directory, then delete)",
            host_dir);
}

} // Anonymous namespace

void OnDelete(const std::string& host_dir) {
    if (!Enabled()) {
        return;
    }
    bool listing_open = false;
    {
        std::scoped_lock lock{g_mutex};
        g_last_delete_in_dir[host_dir] = ++g_delete_sequence;
        const auto it = g_open_dirs.find(host_dir);
        listing_open = it != g_open_dirs.end() && it->second > 0;
    }
    if (listing_open) {
        ReportRemoveAll(host_dir);
    }
}

void OnDirectoryOpen(const std::string& host_dir) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    ++g_open_dirs[host_dir];
}

void OnDirectoryClose(const std::string& host_dir) {
    std::scoped_lock lock{g_mutex};
    if (auto it = g_open_dirs.find(host_dir); it != g_open_dirs.end() && --it->second <= 0) {
        g_open_dirs.erase(it);
    }
}

u64 DeleteSequence() {
    std::scoped_lock lock{g_mutex};
    return g_delete_sequence;
}

void OnDirectoryRead(const std::string& host_dir, u64 open_sequence) {
    if (!Enabled()) {
        return;
    }
    {
        std::scoped_lock lock{g_mutex};
        const auto it = g_last_delete_in_dir.find(host_dir);
        if (it == g_last_delete_in_dir.end() || it->second <= open_sequence) {
            return;
        }
    }
    ReportRemoveAll(host_dir);
}

} // namespace Zeste::Fs
