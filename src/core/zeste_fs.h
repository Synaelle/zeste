// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste file-system checks (zeste_fs_checks): the std::filesystem::remove_all pattern, which
// never returns on a real New 3DS (openmw3ds AZAHAR_INACCURACIES.md): entries of a directory are
// deleted while a listing of that same directory is still open.

#pragma once

#include <string>
#include "common/common_types.h"

namespace Zeste::Fs {

/// Records a deletion inside `host_dir` (directory containing the deleted entry).
void OnDelete(const std::string& host_dir);
/// Current deletion counter; a directory listing remembers it when it is opened.
u64 DeleteSequence();
/// A directory listing opened at `open_sequence` reads more entries. If entries of that same
/// directory were deleted since, report the remove_all pattern.
void OnDirectoryRead(const std::string& host_dir, u64 open_sequence);

/// Directory listings currently open (deleting inside one of them is the remove_all pattern).
void OnDirectoryOpen(const std::string& host_dir);
void OnDirectoryClose(const std::string& host_dir);

} // namespace Zeste::Fs
