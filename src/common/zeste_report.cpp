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
};

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
            }
        }
        if (it != map.end()) {
            key_count = ++it->second.count;
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
        LOG_WARNING(Zeste, "[{}] {} | {}{}", CategoryName(category), message, FormatContext(ctx),
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
    out += "| Category | Total |\n|---|---|\n";
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
    for (std::size_t i = 0; i < NumCategories; ++i) {
        if (s.keys[i].empty()) {
            continue;
        }
        out += fmt::format("\n## {}\n\n| Count | Issue | First seen |\n|---|---|---|\n",
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
            out += fmt::format("| {} | {} | {} |\n", entry->count, message,
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
                    "{}\n      {{\"key\": \"{}\", \"count\": {}, \"message\": \"{}\", \"pc\": {}, "
                    "\"lr\": {}, \"thread\": {}, \"core\": {}, \"frame\": {}}}",
                    first ? "" : ",", JsonEscape(key), entry.count,
                    JsonEscape(entry.first_message), c.pc, c.lr, c.thread_id, c.core_id, c.frame);
                first = false;
            }
            json += fmt::format("{}]}}{}\n", first ? "" : "\n    ",
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
