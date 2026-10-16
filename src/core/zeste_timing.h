// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste timing models: SD card latency, New 3DS CPU speed switching and the remove_all hazard.
// (The PICA cost model lives in video_core, see Pica::Zeste::DrawCostCycles.)
//
// SD card model, from New 3DS hardware measurements (openmw3ds PORTING_NOTES, tests #1-#23):
//   read   ~2 ms per request + ~10 MB/s  (4K: 1.73 MB/s, 64K: 7.68 MB/s, 256K: 9.52 MB/s)
//   write  ~2 ms per request + ~0.6 MB/s (2 MB in 256K chunks: 0.40 MB/s incl. one creation)
//   open/stat ~6 ms (5-7 ms per stat), file or directory creation ~1.5 s, delete ~30 ms
//   (safeRemoveAll: 136 ms for 4 entries).

#pragma once

#include <string>
#include "common/common_types.h"

namespace Core {
class Timing;
}

namespace Zeste::Timing {

bool SdEnabled();

u64 SdReadNs(u64 length);
u64 SdWriteNs(u64 length);
u64 SdOpenNs();
u64 SdCreateNs();
u64 SdDeleteNs();

/// SD archive operations that run inside a service call add their cost here (thread-local);
/// the FS service then makes the client thread wait for it.
void AddSdDelayNs(u64 ns);
u64 TakeSdDelayNs();

/// Records a deletion inside `host_dir` (directory containing the deleted entry).
void OnSdDelete(const std::string& host_dir);
/// Current deletion counter; a directory listing remembers it when it is opened.
u64 SdDeleteSequence();
/// A directory listing opened at `open_sequence` reads more entries. If entries of that same
/// directory were deleted since, report the std::filesystem::remove_all pattern that never
/// returns on hardware.
void OnSdDirectoryRead(const std::string& host_dir, u64 open_sequence);

/// Directory listings currently open (deleting inside one of them is the remove_all pattern).
void OnSdDirectoryOpen(const std::string& host_dir);
void OnSdDirectoryClose(const std::string& host_dir);

/// New 3DS CPU speed: 268 MHz (normal) or 804 MHz (speedup), relative to the user's clock
/// percentage. No effect unless zeste_cpu_speed_model is on.
void SetCpuSpeedup(Core::Timing& timing, bool speedup);

} // namespace Zeste::Timing
