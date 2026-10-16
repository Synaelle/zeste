// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste: glue between the common reporter (common/zeste_report.h) and the emulated system.

#pragma once

#include <string>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Zeste {

/// Installs the context provider (PC, LR, thread, core, frame) and the break handler, applies the
/// reporting settings and clears the counters. Called when a title has been loaded.
void InstallCoreHooks(Core::System& system);

/// Removes the hooks and writes the inaccuracy report (when enabled). Called on shutdown.
void OnSystemShutdown(Core::System& system);

/// Base path (without extension) of the inaccuracy report written on shutdown.
std::string DefaultReportBasePath();

/// Overrides the report base path (command line `--zeste-report`).
void SetReportBasePath(const std::string& path);

/// Current emulated frame (VBlank count of the renderer), 0 when no GPU exists.
u64 CurrentFrame(Core::System& system);

} // namespace Zeste
