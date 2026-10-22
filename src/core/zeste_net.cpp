// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <array>
#include <mutex>
#include <set>
#include <string>
#include "common/logging/log.h"
#include "core/zeste_net.h"

namespace Zeste::Net {

namespace {

constexpr std::array<std::string_view, 9> BlockedDomains{
    "nintendo.net",        // console services: NNID, friends, eShop, NUS, BOSS, conntest
    "nintendowifi.net",    // Nintendo WFC / NUS CDN / connection test
    "nintendo.com",        "nintendo.co.jp", "nintendo.jp", "nintendo-europe.com",
    "nintendo.co.uk",      "pretendo.cc", // Pretendo Network (replacement servers)
    "pretendo.network",
};

std::string Normalize(std::string_view host) {
    std::string out;
    out.reserve(host.size());
    for (const char c : host) {
        if (c == '\0') {
            break;
        }
        out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    while (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    return out;
}

} // namespace

bool IsBlockedHost(std::string_view host) {
    const std::string name = Normalize(host);
    for (const std::string_view domain : BlockedDomains) {
        if (name == domain ||
            (name.size() > domain.size() && name.ends_with(domain) &&
             name[name.size() - domain.size() - 1] == '.')) {
            return true;
        }
    }
    return false;
}

bool CheckBlocked(std::string_view host, std::string_view what) {
    if (!IsBlockedHost(host)) {
        return false;
    }
    static std::mutex mutex;
    static std::set<std::string> logged;
    std::scoped_lock lock{mutex};
    if (logged.insert(Normalize(host)).second) {
        LOG_WARNING(Zeste,
                    "Blocked {} of {}: Zeste never reaches Nintendo's online services (or "
                    "Pretendo), it fails as if there were no route",
                    what, Normalize(host));
    }
    return true;
}

} // namespace Zeste::Net
