// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <catch2/catch_test_macros.hpp>
#include "common/zeste_report.h"

TEST_CASE("Zeste report counts, deduplicates and rate-limits", "[common][zeste]") {
    Zeste::Reset();
    Zeste::SetLogLimit(2);
    Zeste::SetBreakOnFirst(false);
    Zeste::SetContextProvider([] {
        Zeste::Context ctx;
        ctx.pc = 0x00100040;
        ctx.thread_id = 7;
        ctx.frame = 42;
        ctx.valid = true;
        return ctx;
    });

    REQUIRE(Zeste::Report(Zeste::Category::PicaState, "tex unit0 6x512", "texture 6x512"));
    REQUIRE(Zeste::Report(Zeste::Category::PicaState, "tex unit0 6x512", "texture 6x512"));
    // Third report of the same key is counted but not logged.
    REQUIRE_FALSE(Zeste::Report(Zeste::Category::PicaState, "tex unit0 6x512", "texture 6x512"));
    REQUIRE(Zeste::Report(Zeste::Category::PicaState, "tex unit1 0x0", "texture address 0"));
    REQUIRE(Zeste::Count(Zeste::Category::PicaState) == 4);
    REQUIRE(Zeste::Count(Zeste::Category::DataAbort) == 0);

    const std::string md = Zeste::FormatMarkdown("unit test");
    REQUIRE(md.find("texture 6x512") != std::string::npos);
    REQUIRE(md.find("| 3 |") != std::string::npos);
    REQUIRE(md.find("pc=0x00100040") != std::string::npos);
    REQUIRE(md.find("frame=42") != std::string::npos);

    Zeste::SetContextProvider({});
    Zeste::Reset();
    REQUIRE(Zeste::Count(Zeste::Category::PicaState) == 0);
}

TEST_CASE("Zeste break on first fires once", "[common][zeste]") {
    Zeste::Reset();
    int breaks = 0;
    Zeste::SetBreakHandler([&breaks](Zeste::Category, const std::string&) { ++breaks; });
    Zeste::SetBreakOnFirst(true);
    Zeste::Report(Zeste::Category::DataAbort, "a", "first");
    Zeste::Report(Zeste::Category::DataAbort, "b", "second");
    REQUIRE(breaks == 1);
    Zeste::SetBreakOnFirst(false);
    Zeste::SetBreakHandler({});
    Zeste::Reset();
}

TEST_CASE("Zeste report: tags and grouped summary", "[common][zeste]") {
    Zeste::Reset();
    Zeste::SetLogLimit(1);
    Zeste::RegisterTagText(7, "water");
    REQUIRE(Zeste::TagText(7) == "water");
    REQUIRE(Zeste::TagText(9) == "#9");
    REQUIRE(Zeste::TagText(0).empty());
    Zeste::SetDrawTag(Zeste::TagText(7));
    Zeste::Report(Zeste::Category::PicaVertex, "range@20000000+10", "out of range A");
    Zeste::Report(Zeste::Category::PicaVertex, "range@20000000+20", "out of range B");
    Zeste::Report(Zeste::Category::PicaVertex, "range@20000000+20", "out of range B");
    Zeste::SetDrawTag({});
    const std::string md = Zeste::FormatMarkdown("tags");
    REQUIRE(md.find("## Grouped summary") != std::string::npos);
    REQUIRE(md.find("| PICA vertex | range | 3 | 2 |") != std::string::npos);
    REQUIRE(md.find("| water |") != std::string::npos);
    const auto issues = Zeste::GetIssues();
    REQUIRE(issues.size() == 2);
    REQUIRE(issues[0].first_tag == "water");
    Zeste::Reset();
}
