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

/// True (once) when "break on first" just fired: the exception dispatcher then leaves the PC on
/// the faulting instruction so the debugger/pause shows it.
bool ConsumeBreakRequest();

/// Country of the emulated console for this run (CFG country code, e.g. 77 = France), applied in
/// memory when a title loads and never saved to the NAND's config. -1: the NAND's own.
void SetCountryOverride(int country_code);

/// Current emulated frame (VBlank count of the renderer), 0 when no GPU exists.
u64 CurrentFrame(Core::System& system);

} // namespace Zeste
