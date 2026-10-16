// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <chrono>
#include <ctime>
#include <mutex>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/zeste_report.h"
#include "common/zeste_run.h"
#include "core/core.h"
#include "core/zeste_automation.h"
#include "core/zeste_bug_report.h"
#include "core/zeste_core.h"

namespace Zeste::BugReports {

namespace {

std::mutex g_mutex;
std::string g_list_path;

std::tm LocalNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    return tm;
}

std::string Timestamp(const std::tm& tm, bool for_file) {
    if (for_file) {
        return fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1,
                           tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1,
                       tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::string Directory(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash + 1);
}

/// Appends text to the list, creating it with a heading the first time.
void Append(const std::string& text) {
    // Several Zeste instances may append at the same time.
    Run::SharedFileLock lock;
    const std::string path = ListPath();
    const bool exists = FileUtil::Exists(path);
    FileUtil::CreateFullPath(path);
    FileUtil::IOFile file{path, "ab"};
    if (!file.IsOpen()) {
        LOG_ERROR(Zeste, "bug report: cannot open {}", path);
        return;
    }
    if (!exists) {
        const std::string heading =
            "# Zeste bug and inaccuracy reports\n\n"
            "Every report filed with the `bug` / `inaccuracy` commands (script, RPC) or the Zeste "
            "menu, and the inaccuracies Zeste detected in each session. Newest at the end.\n";
        file.WriteString(heading);
    }
    file.WriteString(text);
}

std::string OneLine(std::string text) {
    for (auto& c : text) {
        if (c == '\n' || c == '\r' || c == '|') {
            c = ' ';
        }
    }
    return text;
}

} // Anonymous namespace

std::string ListPath() {
    std::scoped_lock lock{g_mutex};
    if (!g_list_path.empty()) {
        return g_list_path;
    }
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "zeste_reports.md";
}

void SetListPath(const std::string& path) {
    std::scoped_lock lock{g_mutex};
    g_list_path = path;
}

std::string File(Core::System* system, Kind kind, const std::string& title,
                 const std::string& description, bool screenshot) {
    const std::tm tm = LocalNow();
    const char* kind_text = kind == Kind::Bug ? "BUG" : "INACCURACY";
    const std::string heading =
        fmt::format("{} · {} · {}", Timestamp(tm, false), kind_text, OneLine(title));

    std::string entry = "\n## " + heading + "\n\n";
    const bool running = system && system->IsPoweredOn();
    if (running) {
        entry += fmt::format("- Application: {} (title {:016X})\n", system->GetFilePath(),
                             system->GetTitleID());
        entry += "- Context: " + FormatContextText(CurrentContext()) + "\n";
    } else {
        entry += "- Context: no application running\n";
    }
    if (const std::string env = GetReportEnvironment(); !env.empty()) {
        // The environment block lists build, console, renderer and Zeste options.
        for (const auto& line : {std::string{"Build"}, std::string{"Zeste options"},
                                 std::string{"Renderer"}, std::string{"Console"}}) {
            const auto pos = env.find("- " + line + ":");
            if (pos != std::string::npos) {
                entry += env.substr(pos, env.find('\n', pos) - pos + 1);
            }
        }
    }
    if (running && screenshot) {
        const std::string dir = Directory(ListPath()) + "zeste_reports_files/";
        FileUtil::CreateFullPath(dir);
        const std::string stem = fmt::format("{}-{}", Timestamp(tm, true), Run::ProcessId());
        std::string shots;
        if (Automation::DumpLcd(*system, 0, dir + stem + "_top.png")) {
            shots += fmt::format("[top](zeste_reports_files/{}_top.png)", stem);
        }
        if (Automation::DumpLcd(*system, 1, dir + stem + "_bottom.png")) {
            shots += fmt::format("{}[bottom](zeste_reports_files/{}_bottom.png)",
                                 shots.empty() ? "" : ", ", stem);
        }
        if (!shots.empty()) {
            entry += "- Screens: " + shots + "\n";
        }
    }
    entry += "\n" + (description.empty() ? std::string{"(no description)"} : description) + "\n";

    Append(entry);
    LOG_INFO(Zeste, "bug report filed in {}: {}", ListPath(), heading);
    return heading;
}

void AppendSessionDetections(const std::string& session_title) {
    const auto issues = GetIssues();
    if (issues.empty()) {
        return;
    }
    const std::tm tm = LocalNow();
    std::string entry = fmt::format("\n## {} · DETECTED · {}\n\n", Timestamp(tm, false),
                                    OneLine(session_title));
    entry += "Inaccuracies detected automatically by the enabled Zeste checks during this "
             "session.\n\n| Count | Category | Issue | First seen |\n|---|---|---|---|\n";
    for (const auto& issue : issues) {
        entry += fmt::format("| {} | {} | {} | {} |\n", issue.count, CategoryName(issue.category),
                             OneLine(issue.message), FormatContextText(issue.first_context));
    }
    Append(entry);
    LOG_INFO(Zeste, "{} detected issue(s) appended to {}", issues.size(), ListPath());
}

} // namespace Zeste::BugReports
