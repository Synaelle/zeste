// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste: central reporting for hardware-accuracy diagnostics.
//
// Every "this would misbehave on a real New 3DS" detection goes through Zeste::Report().
// Reports are:
//   - logged at warning level with PC, thread ID and frame number (supplied by the core through
//     a context provider), rate-limited per key and per category so logs stay small;
//   - counted and collected for the end-of-session inaccuracy report (Markdown + JSON);
//   - optionally used to pause emulation the first time they fire ("break on first").

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <fmt/format.h>
#include "common/common_types.h"

namespace Zeste {

enum class Category : u32 {
    PicaVertex,      ///< NaN/Inf or out-of-range post-clip vertex data
    PicaBounds,      ///< Primitive bounds outside the bound framebuffer
    PicaState,       ///< Invalid texture / buffer / address state at draw time
    CacheCpuToGpu,   ///< Device read of memory the CPU wrote without a cache flush
    CacheGpuToCpu,   ///< CPU read of memory a device wrote without a cache invalidate
    DataAbort,       ///< Unmapped / protected memory access
    StackOverflow,   ///< Access just below a thread's stack
    Kernel,          ///< SVC / kernel behaviour that differs from hardware
    Scheduling,      ///< Race-exposure / starvation diagnostics
    Timing,          ///< Timing-model notes
    System,          ///< SD card, APT, process lifetime
    Count,
};

const char* CategoryName(Category category);

/// Execution context attached to every report.
struct Context {
    u32 pc = 0;
    u32 lr = 0;
    u32 thread_id = 0;
    u32 core_id = 0;
    u64 frame = 0;
    bool valid = false;
};

/// Installed by the core once a system is running; must be cheap and thread-safe enough to be
/// called from the emulation and GPU threads.
void SetContextProvider(std::function<Context()> provider);

/// Called (once per category, or once in total, see SetBreakOnFirst) when a report fires and
/// "break on first" is enabled. The core installs a handler that pauses emulation.
void SetBreakHandler(std::function<void(Category, const std::string&)> handler);

/// Sets the per-key number of log lines before a key is silenced (summaries are still kept).
void SetLogLimit(u32 per_key_limit);
void SetBreakOnFirst(bool enabled);

/// Report a diagnostic. `key` deduplicates (e.g. "tex-size unit0 6x512"): every distinct key is
/// logged up to the per-key limit, then counted silently. Returns true if the message was logged.
bool Report(Category category, std::string_view key, std::string_view message);

template <typename... Args>
bool ReportF(Category category, std::string_view key, fmt::format_string<Args...> format,
             Args&&... args) {
    return Report(category, key, fmt::format(format, std::forward<Args>(args)...));
}

/// Returns the current context (or an invalid one when no provider is installed).
Context CurrentContext();

/// Clears all counters (called when a new title boots).
void Reset();

/// Total number of reports in a category since the last Reset().
u64 Count(Category category);

/// Writes the inaccuracy report (Markdown and JSON next to each other). `base_path` is the path
/// without extension. Returns false when nothing was written.
bool WriteReport(const std::string& base_path, std::string_view title);

/// Environment block (build, title, settings) included in every report.
void SetReportEnvironment(std::string environment);

/// Formats the report as Markdown (used by WriteReport and the Qt "Inaccuracy report" dialog).
std::string FormatMarkdown(std::string_view title);

} // namespace Zeste
