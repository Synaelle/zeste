// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <catch2/catch_test_macros.hpp>
#include "core/zeste_net.h"

TEST_CASE("Zeste blocks Nintendo's online service hosts only", "[core][zeste]") {
    using Zeste::Net::IsBlockedHost;
    // Blocked: the domains and their subdomains, any case, with a trailing dot.
    REQUIRE(IsBlockedHost("nintendo.net"));
    REQUIRE(IsBlockedHost("account.nintendo.net"));
    REQUIRE(IsBlockedHost("nus.cdn.c.shop.nintendowifi.net"));
    REQUIRE(IsBlockedHost("conntest.nintendowifi.net"));
    REQUIRE(IsBlockedHost("NASC.NINTENDOWIFI.NET"));
    REQUIRE(IsBlockedHost("www.nintendo.com."));
    REQUIRE(IsBlockedHost("account.pretendo.cc"));
    REQUIRE(IsBlockedHost("nasc.pretendo.network"));
    // Not blocked: other hosts, look-alikes, and names that only contain a blocked one.
    REQUIRE_FALSE(IsBlockedHost("example.com"));
    REQUIRE_FALSE(IsBlockedHost("192.168.1.22"));
    REQUIRE_FALSE(IsBlockedHost("mynintendo.com"));
    REQUIRE_FALSE(IsBlockedHost("nintendo.net.example.com"));
    REQUIRE_FALSE(IsBlockedHost("notpretendo.cc"));
    REQUIRE_FALSE(IsBlockedHost("github.com"));
    REQUIRE_FALSE(IsBlockedHost(""));
}
