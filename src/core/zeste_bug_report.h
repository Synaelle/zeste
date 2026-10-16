// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste bug / inaccuracy list: one append-only Markdown file collecting every report filed by
// a person or an agent (script/RPC commands "bug" and "inaccuracy", or the Qt dialog) and the
// inaccuracies Zeste detected in each session. Default: <user dir>/zeste_reports.md.

#pragma once

#include <string>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Zeste::BugReports {

enum class Kind { Bug, Inaccuracy };

/// Path of the list file (default <user dir>/zeste_reports.md).
std::string ListPath();
void SetListPath(const std::string& path);

/// Files a report. With a running system, the entry records the frame, PC, thread and the
/// application, and (when `screenshot` is set) saves both LCDs next to the list file.
/// Must be called on the emulation thread when `system` is running. Returns the entry heading.
std::string File(Core::System* system, Kind kind, const std::string& title,
                 const std::string& description, bool screenshot);

/// Appends the inaccuracies detected during the session that is ending (no-op when none).
void AppendSessionDetections(const std::string& session_title);

} // namespace Zeste::BugReports
