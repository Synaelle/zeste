// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

namespace Common {

extern const char g_scm_rev[];
extern const char g_scm_branch[];
extern const char g_scm_desc[];
extern const char g_build_name[];
extern const char g_build_date[];
extern const char g_build_fullname[];
extern const char g_build_version[];
/// Zeste release this build comes from ("0.4.1", or "0.4.1-3-g3159690" after it).
extern const char g_zeste_version[];
/// For the title bar and --version: "0.4.5", or "0.4.5-dev" for a build after that release.
extern const char g_zeste_display_version[];
extern const char g_build_variant[];
extern const char g_shader_cache_version[];

} // namespace Common
