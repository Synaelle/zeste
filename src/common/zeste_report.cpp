// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/zeste_report.h"

namespace Zeste {

namespace {

constexpr std::size_t NumCategories = static_cast<std::size_t>(Category::Count);
/// Hard cap of log lines per category for a whole session, whatever the number of keys.
constexpr u64 CategoryLogBudget = 300;
/// Distinct keys remembered per category for the report (later keys are only counted).
constexpr std::size_t MaxKeysPerCategory = 512;

struct KeyEntry {
    u64 count = 0;
    std::string first_message;
    Context first_context;
    std::string first_tag;
    u64 last_frame = ~0ULL;
    u64 draws = 0; ///< Frames in which this site reported (one draw per frame and site)
};

/// Grouped totals per kind of issue (the key prefix before '@'), for the summary.
struct KindEntry {
    u64 count = 0;          ///< Reports (primitives, accesses...)
    u64 sites = 0;          ///< Distinct keys (draw sites, addresses...)
    u64 draws = 0;          ///< Distinct (site, frame) pairs
    u64 frames = 0;         ///< Distinct frames (counted as frame changes)
    u64 first_frame = 0;
    u64 last_frame = ~0ULL;
    std::string example;    ///< First message
};

std::string_view KindOf(std::string_view key) {
    const auto at = key.find('@');
    return at == std::string_view::npos ? key : key.substr(0, at);
}

thread_local std::string t_draw_tag;

struct State {
    std::mutex mutex;
    std::function<Context()> context_provider;
    std::function<void(Category, const std::string&)> break_handler;
    u32 per_key_limit = 3;
    bool break_on_first = false;
    bool broke = false;
    std::array<std::map<std::string, KeyEntry, std::less<>>, NumCategories> keys;
    std::array<u64, NumCategories> logged{};
    std::array<u64, NumCategories> dropped_keys{};
    std::array<std::atomic<u64>, NumCategories> totals{};
    std::string environment;
    std::array<std::map<std::string, KindEntry, std::less<>>, NumCategories> kinds;
    std::map<u32, std::string> tag_texts;
};

State& GetState() {
    static State state;
    return state;
}

std::string FormatContext(const Context& ctx) {
    if (!ctx.valid) {
        return "pc=? thread=? frame=?";
    }
    return fmt::format("pc=0x{:08X} lr=0x{:08X} thread={} core={} frame={}", ctx.pc, ctx.lr,
                       ctx.thread_id, ctx.core_id, ctx.frame);
}

std::string JsonEscape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    return out;
}

} // Anonymous namespace

const char* CategoryName(Category category) {
    switch (category) {
    case Category::PicaVertex:
        return "PICA vertex";
    case Category::PicaBounds:
        return "PICA bounds";
    case Category::PicaState:
        return "PICA state";
    case Category::CacheCpuToGpu:
        return "Cache (CPU->device)";
    case Category::CacheGpuToCpu:
        return "Cache (device->CPU)";
    case Category::DataAbort:
        return "Data abort";
    case Category::StackOverflow:
        return "Stack overflow";
    case Category::Kernel:
        return "Kernel";
    case Category::Scheduling:
        return "Scheduling";
    case Category::Timing:
        return "Timing";
    case Category::System:
        return "System";
    default:
        return "Unknown";
    }
}

void SetContextProvider(std::function<Context()> provider) {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.context_provider = std::move(provider);
}

void SetBreakHandler(std::function<void(Category, const std::string&)> handler) {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.break_handler = std::move(handler);
}

void SetReportEnvironment(std::string environment) {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.environment = std::move(environment);
}

void SetDrawTag(std::string_view tag) {
    t_draw_tag.assign(tag);
}

void RegisterTagText(u32 id, std::string text) {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.tag_texts[id] = std::move(text);
}

std::string TagText(u32 id) {
    if (id == 0) {
        return {};
    }
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    const auto it = s.tag_texts.find(id);
    return it != s.tag_texts.end() ? it->second : fmt::format("#{}", id);
}

std::string GetReportEnvironment() {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    return s.environment;
}

std::vector<IssueSummary> GetIssues() {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    std::vector<IssueSummary> out;
    for (std::size_t i = 0; i < NumCategories; ++i) {
        for (const auto& [key, entry] : s.keys[i]) {
            out.push_back({static_cast<Category>(i), entry.count, entry.first_message,
                           entry.first_context, entry.first_tag});
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const auto& a, const auto& b) { return a.count > b.count; });
    return out;
}

std::string FormatContextText(const Context& context) {
    return FormatContext(context);
}

void SetLogLimit(u32 per_key_limit) {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.per_key_limit = per_key_limit;
}

void SetBreakOnFirst(bool enabled) {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.break_on_first = enabled;
}

Context CurrentContext() {
    auto& s = GetState();
    std::function<Context()> provider;
    {
        std::scoped_lock lock{s.mutex};
        provider = s.context_provider;
    }
    return provider ? provider() : Context{};
}

bool Report(Category category, std::string_view key, std::string_view message) {
    auto& s = GetState();
    const auto index = static_cast<std::size_t>(category);
    s.totals[index].fetch_add(1, std::memory_order_relaxed);

    // Context is gathered outside the lock: the provider may take other locks.
    const Context ctx = CurrentContext();

    bool do_log = false;
    bool do_break = false;
    u64 key_count = 0;
    std::function<void(Category, const std::string&)> break_handler;
    {
        std::scoped_lock lock{s.mutex};
        auto& map = s.keys[index];
        auto it = map.find(key);
        if (it == map.end()) {
            if (map.size() >= MaxKeysPerCategory) {
                ++s.dropped_keys[index];
            } else {
                it = map.emplace(std::string{key}, KeyEntry{}).first;
                it->second.first_message = std::string{message};
                it->second.first_context = ctx;
                it->second.first_tag = t_draw_tag;
            }
        }
        bool new_site = false;
        bool new_draw = false;
        if (it != map.end()) {
            key_count = ++it->second.count;
            new_site = key_count == 1;
            if (it->second.last_frame != ctx.frame) {
                it->second.last_frame = ctx.frame;
                ++it->second.draws;
                new_draw = true;
            }
        }
        // Grouped totals per kind, kept even for keys beyond MaxKeysPerCategory.
        auto& kinds = s.kinds[index];
        const auto kind_name = KindOf(key);
        auto kind_it = kinds.find(kind_name);
        if (kind_it == kinds.end()) {
            kind_it = kinds.emplace(std::string{kind_name}, KindEntry{}).first;
            kind_it->second.first_frame = ctx.frame;
            kind_it->second.example = std::string{message};
        }
        auto& kind = kind_it->second;
        ++kind.count;
        kind.sites += new_site ? 1 : 0;
        kind.draws += new_draw || it == map.end() ? 1 : 0;
        if (kind.last_frame != ctx.frame) {
            kind.last_frame = ctx.frame;
            ++kind.frames;
        }
        if (key_count != 0 && key_count <= s.per_key_limit &&
            s.logged[index] < CategoryLogBudget) {
            do_log = true;
            ++s.logged[index];
            if (s.logged[index] == CategoryLogBudget) {
                LOG_WARNING(Zeste,
                            "[{}] log budget reached; further reports in this category are only "
                            "counted (see the inaccuracy report)",
                            CategoryName(category));
            }
        }
        if (s.break_on_first && !s.broke && s.break_handler) {
            s.broke = true;
            do_break = true;
            break_handler = s.break_handler;
        }
    }

    if (do_log) {
        LOG_WARNING(Zeste, "[{}] {}{} | {}{}", CategoryName(category), message,
                    t_draw_tag.empty() ? std::string{} : fmt::format(" | tag \"{}\"", t_draw_tag),
                    FormatContext(ctx),
                    key_count == s.per_key_limit ? " (further reports of this kind are silenced)"
                                                 : "");
    }
    if (do_break) {
        const std::string text = fmt::format("[{}] {} | {}", CategoryName(category), message,
                                             FormatContext(ctx));
        LOG_CRITICAL(Zeste, "Break on first report: {}", text);
        break_handler(category, text);
    }
    return do_log;
}

void Reset() {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    for (std::size_t i = 0; i < NumCategories; ++i) {
        s.keys[i].clear();
        s.kinds[i].clear();
        s.logged[i] = 0;
        s.dropped_keys[i] = 0;
        s.totals[i].store(0, std::memory_order_relaxed);
    }
    s.broke = false;
}

u64 Count(Category category) {
    return GetState().totals[static_cast<std::size_t>(category)].load(std::memory_order_relaxed);
}

std::string FormatMarkdown(std::string_view title) {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    std::string out = fmt::format("# Zeste inaccuracy report: {}\n\n", title);
    out += "Each line is something that the emulator handled but a real New 3DS would not (or "
           "would handle differently). Counts are totals; the context is the first occurrence.\n\n";
    if (!s.environment.empty()) {
        out += "## Environment\n\n" + s.environment + "\n";
    }
    out += "## Summary\n\n| Category | Total |\n|---|---|\n";
    u64 grand_total = 0;
    for (std::size_t i = 0; i < NumCategories; ++i) {
        const u64 total = s.totals[i].load(std::memory_order_relaxed);
        grand_total += total;
        out += fmt::format("| {} | {} |\n", CategoryName(static_cast<Category>(i)), total);
    }
    if (grand_total == 0) {
        out += "\nNo hardware-accuracy issue was detected (only enabled checks run).\n";
        return out;
    }

    // One line per kind of issue: how many reports, from how many draw sites / draws / frames.
    out += "\n## Grouped summary\n\nOne line per kind of issue. Reports = primitives or accesses; "
           "sites = distinct draw sites (command list + offset) or addresses; draws = (site, "
           "frame) pairs.\n\n| Category | Kind | Reports | Sites | Draws | Frames | First frame "
           "| Example |\n|---|---|---|---|---|---|---|---|\n";
    for (std::size_t i = 0; i < NumCategories; ++i) {
        std::vector<std::pair<const std::string*, const KindEntry*>> kinds;
        for (const auto& [name, entry] : s.kinds[i]) {
            kinds.emplace_back(&name, &entry);
        }
        std::sort(kinds.begin(), kinds.end(),
                  [](const auto& a, const auto& b) { return a.second->count > b.second->count; });
        for (const auto& [name, kind] : kinds) {
            std::string example = kind->example;
            if (const auto bar = example.find(" | "); bar != std::string::npos) {
                example.resize(bar);
            }
            for (auto& c : example) {
                if (c == '|' || c == '\n') {
                    c = ' ';
                }
            }
            out += fmt::format("| {} | {} | {} | {} | {} | {} | {} | {} |\n",
                               CategoryName(static_cast<Category>(i)), *name, kind->count,
                               kind->sites, kind->draws, kind->frames, kind->first_frame, example);
        }
    }

    for (std::size_t i = 0; i < NumCategories; ++i) {
        if (s.keys[i].empty()) {
            continue;
        }
        out += fmt::format("\n## {}\n\n| Count | Issue | Tag | First seen |\n|---|---|---|---|\n",
                           CategoryName(static_cast<Category>(i)));
        std::vector<std::pair<const std::string*, const KeyEntry*>> sorted;
        for (const auto& [key, entry] : s.keys[i]) {
            sorted.emplace_back(&key, &entry);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second->count > b.second->count; });
        for (const auto& [key, entry] : sorted) {
            std::string message = entry->first_message;
            for (auto& c : message) {
                if (c == '|' || c == '\n') {
                    c = ' ';
                }
            }
            out += fmt::format("| {} | {} | {} | {} |\n", entry->count, message, entry->first_tag,
                               FormatContext(entry->first_context));
        }
        if (s.dropped_keys[i] != 0) {
            out += fmt::format("\n{} further distinct issues were only counted.\n",
                               s.dropped_keys[i]);
        }
    }
    return out;
}

bool WriteReport(const std::string& base_path, std::string_view title) {
    const std::string markdown = FormatMarkdown(title);

    std::string json = fmt::format("{{\n  \"title\": \"{}\",\n  \"categories\": [\n",
                                   JsonEscape(title));
    {
        auto& s = GetState();
        std::scoped_lock lock{s.mutex};
        for (std::size_t i = 0; i < NumCategories; ++i) {
            json += fmt::format("    {{\"name\": \"{}\", \"total\": {}, \"issues\": [",
                                CategoryName(static_cast<Category>(i)),
                                s.totals[i].load(std::memory_order_relaxed));
            bool first = true;
            for (const auto& [key, entry] : s.keys[i]) {
                const auto& c = entry.first_context;
                json += fmt::format(
                    "{}\n      {{\"key\": \"{}\", \"count\": {}, \"draws\": {}, \"message\": "
                    "\"{}\", \"tag\": \"{}\", \"pc\": {}, \"lr\": {}, \"thread\": {}, "
                    "\"core\": {}, \"frame\": {}}}",
                    first ? "" : ",", JsonEscape(key), entry.count, entry.draws,
                    JsonEscape(entry.first_message), JsonEscape(entry.first_tag), c.pc, c.lr,
                    c.thread_id, c.core_id, c.frame);
                first = false;
            }
            json += fmt::format("{}], \"groups\": [", first ? "" : "\n    ");
            bool first_kind = true;
            for (const auto& [name, kind] : s.kinds[i]) {
                json += fmt::format(
                    "{}\n      {{\"kind\": \"{}\", \"reports\": {}, \"sites\": {}, "
                    "\"draws\": {}, \"frames\": {}, \"first_frame\": {}, \"example\": \"{}\"}}",
                    first_kind ? "" : ",", JsonEscape(name), kind.count, kind.sites, kind.draws,
                    kind.frames, kind.first_frame, JsonEscape(kind.example));
                first_kind = false;
            }
            json += fmt::format("{}]}}{}\n", first_kind ? "" : "\n    ",
                                i + 1 == NumCategories ? "" : ",");
        }
    }
    json += "  ]\n}\n";

    bool ok = FileUtil::WriteStringToFile(true, base_path + ".md", markdown) == markdown.size();
    ok &= FileUtil::WriteStringToFile(true, base_path + ".json", json) == json.size();
    if (ok) {
        LOG_INFO(Zeste, "Inaccuracy report written to {}.md / .json", base_path);
    } else {
        LOG_ERROR(Zeste, "Could not write the inaccuracy report to {}", base_path);
    }
    return ok;
}

} // namespace Zeste
