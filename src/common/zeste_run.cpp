// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <regex>
#include <vector>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/zeste_run.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Zeste::Run {

namespace {

std::string g_folder;

std::string Now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1,
                       tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void Prune(const std::string& runs_dir) {
    namespace fs = std::filesystem;
    static const std::regex pattern{R"(^\d{8}-\d{6}-\d+$)"};
    std::vector<fs::path> folders;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::u8path(runs_dir), ec)) {
        if (entry.is_directory(ec) && std::regex_match(entry.path().filename().string(), pattern)) {
            folders.push_back(entry.path());
        }
    }
    if (folders.size() <= static_cast<std::size_t>(KeptRunFolders)) {
        return;
    }
    // Names start with the timestamp, so the lexical order is the chronological order.
    std::sort(folders.begin(), folders.end());
    for (std::size_t i = 0; i + KeptRunFolders < folders.size(); ++i) {
        fs::remove_all(folders[i], ec);
    }
}

} // Anonymous namespace

u32 ProcessId() {
#ifdef _WIN32
    return static_cast<u32>(GetCurrentProcessId());
#else
    return static_cast<u32>(getpid());
#endif
}

std::string Begin(const std::string& command_line) {
    const std::string log_root = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
    const std::string runs_dir = log_root + "runs/";
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    const std::string name =
        fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}-{}", tm.tm_year + 1900, tm.tm_mon + 1,
                    tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ProcessId());
    const std::string folder = runs_dir + name + "/";
    if (!FileUtil::CreateFullPath(folder)) {
        return {}; // Keep the shared log folder rather than failing.
    }
    Prune(runs_dir);
    FileUtil::UpdateUserPath(FileUtil::UserPath::LogDir, folder);
    g_folder = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
    // Pointer for people looking for "the last run"; concurrent instances overwrite it.
    FileUtil::WriteStringToFile(true, log_root + "latest_run.txt", g_folder + "\n");
    Note("started", Now());
    Note("pid", std::to_string(ProcessId()));
    Note("command line", command_line);
    return g_folder;
}

const std::string& Folder() {
    return g_folder;
}

void Note(const std::string& key, const std::string& value) {
    if (g_folder.empty()) {
        return;
    }
    FileUtil::IOFile file{g_folder + "run.txt", "a"};
    file.WriteString(key + ": " + value + "\n");
}

} // namespace Zeste::Run
