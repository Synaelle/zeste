// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste per-run folders. Every Zeste process gets its own folder,
//   <user dir>/log/runs/<YYYYmmdd-HHMMSS>-<pid>/
// which becomes the log directory: the log, the session inaccuracy report and run.txt (command
// line, title, start/end time, exit code) of that run live there. Two instances running at the
// same time therefore never rotate, lock or overwrite each other's files. The newest
// KeptRunFolders folders are kept; older ones are deleted at startup.

#pragma once

#include <string>
#include "common/common_types.h"

namespace Zeste::Run {

constexpr int KeptRunFolders = 50;

/// Creates this process's run folder, makes it the log directory and prunes old run folders.
/// Must be called before the logging backend is initialised. Returns the folder (with a
/// trailing separator).
std::string Begin(const std::string& command_line);

/// This process's run folder ("" before Begin).
const std::string& Folder();

/// Appends "key: value" to run.txt.
void Note(const std::string& key, const std::string& value);

/// Process ID, for file names.
u32 ProcessId();

/// Cross-process lock for files shared by every Zeste instance (the report list). Waits up to
/// a few seconds for another instance, then proceeds anyway (logged).
class SharedFileLock {
public:
    SharedFileLock();
    ~SharedFileLock();
    SharedFileLock(const SharedFileLock&) = delete;
    SharedFileLock& operator=(const SharedFileLock&) = delete;

private:
    void* handle = nullptr;
    bool owned = false;
};

} // namespace Zeste::Run
