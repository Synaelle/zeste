// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste never reaches Nintendo's online services (or replacements of them such as Pretendo):
// with system files from a linked console, a test run must not log in, download titles or touch
// the console's account. Lookups and HTTP requests for these hosts fail as if the network had no
// route to them. Always on, not an option.

#pragma once

#include <string_view>

namespace Zeste::Net {

/// True for Nintendo's online service domains (and their subdomains) and Pretendo's.
bool IsBlockedHost(std::string_view host);

/// IsBlockedHost, and logs the block (once per host) with `what` (e.g. "DNS lookup").
bool CheckBlocked(std::string_view host, std::string_view what);

} // namespace Zeste::Net
