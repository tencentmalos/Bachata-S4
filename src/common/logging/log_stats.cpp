// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log_stats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "common/profiler.h"

namespace Common::Log::Stats {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto PublishInterval = std::chrono::milliseconds(250);
// Recent rate of a site: lines of the last complete window of this length.
constexpr auto RateWindow = std::chrono::seconds(10);

struct SiteKey {
    const char* file;
    int line;
    bool operator==(const SiteKey&) const = default;
};

struct SiteKeyHash {
    std::size_t operator()(const SiteKey& key) const noexcept {
        // __FILE__ literals are distinct objects per file within one binary.
        return std::hash<const void*>{}(key.file) ^ (std::size_t(key.line) * 0x9E3779B97F4A7C15ull);
    }
};

struct Site {
    Class log_class{};
    const char* func{};
    std::uint64_t lines{};
    std::uint64_t bytes{};
    Clock::time_point window_start{};
    std::uint64_t window_lines{};
    std::uint64_t last_window_lines{};
    bool rolled{}; // A complete window exists.
};

struct Shard {
    std::mutex mutex;
    std::unordered_map<SiteKey, Site, SiteKeyHash> sites;
};

std::array<Shard, 64> g_shards;
std::atomic<std::uint64_t> g_lines{0};
std::atomic<std::uint64_t> g_bytes{0};
std::array<std::atomic<std::uint64_t>, NUM_LOG_CLASSES> g_class_lines{};
std::array<std::atomic<std::uint64_t>, NUM_LOG_CLASSES> g_class_bytes{};

std::atomic<std::int64_t> g_next_publish{0};
std::mutex g_publish_mutex;

std::string_view Basename(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// Lines per second over the last complete window, or over the running one if it is the first.
double RecentRate(const Site& site, Clock::time_point now) {
    const auto age = now - site.window_start;
    if (age >= 2 * RateWindow) {
        return 0.0;
    }
    if (age >= RateWindow) {
        return double(site.window_lines) / std::chrono::duration<double>(RateWindow).count();
    }
    if (!site.rolled) {
        const double seconds = std::chrono::duration<double>(age).count();
        return double(site.window_lines) / std::max(seconds, 1.0);
    }
    return double(site.last_window_lines) / std::chrono::duration<double>(RateWindow).count();
}

} // namespace

void Record(Class log_class, const char* file, int line, const char* func,
            std::size_t message_bytes) noexcept {
    g_lines.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(message_bytes, std::memory_order_relaxed);
    const auto index = static_cast<std::size_t>(log_class);
    if (index < g_class_lines.size()) {
        g_class_lines[index].fetch_add(1, std::memory_order_relaxed);
        g_class_bytes[index].fetch_add(message_bytes, std::memory_order_relaxed);
    }
    const SiteKey key{file, line};
    auto& shard = g_shards[SiteKeyHash{}(key) % g_shards.size()];
    const auto now = Clock::now();
    try {
        std::scoped_lock lock{shard.mutex};
        auto [it, inserted] = shard.sites.try_emplace(key);
        auto& site = it->second;
        if (inserted) {
            site.log_class = log_class;
            site.func = func;
            site.window_start = now;
        } else if (now - site.window_start >= RateWindow) {
            site.last_window_lines =
                now - site.window_start < 2 * RateWindow ? site.window_lines : 0;
            site.window_lines = 0;
            site.window_start = now;
            site.rolled = true;
        }
        ++site.lines;
        site.bytes += message_bytes;
        ++site.window_lines;
    } catch (...) {
        // Accounting must never affect the logging caller.
    }
}

void Tick() noexcept {
    const std::int64_t now = Clock::now().time_since_epoch().count();
    if (now < g_next_publish.load(std::memory_order_relaxed) || !Common::Profiler::Enabled()) {
        return;
    }
    std::unique_lock publish{g_publish_mutex, std::try_to_lock};
    if (!publish.owns_lock() || now < g_next_publish.load(std::memory_order_relaxed)) {
        return;
    }
    g_next_publish.store(now + std::chrono::duration_cast<Clock::duration>(PublishInterval).count(),
                         std::memory_order_relaxed);
    try {
        static const std::array<std::string, NUM_LOG_CLASSES> class_counters = [] {
            std::array<std::string, NUM_LOG_CLASSES> names;
            for (std::size_t i = 0; i < names.size(); ++i) {
                names[i] = "Log.Class." + std::string(NameOf(static_cast<Class>(i)));
            }
            return names;
        }();
        // Every tick, so a capture started later still sees each active class.
        Common::Profiler::Counter("Log.Lines", g_lines.load(std::memory_order_relaxed));
        Common::Profiler::Counter("Log.MessageBytes", g_bytes.load(std::memory_order_relaxed));
        for (std::size_t i = 0; i < class_counters.size(); ++i) {
            if (const auto value = g_class_lines[i].load(std::memory_order_relaxed)) {
                Common::Profiler::Counter(class_counters[i].c_str(), value);
            }
        }
    } catch (...) {
    }
}

std::string Command(const std::vector<std::string>& args) {
    std::uint32_t top = 20;
    const bool status = args.empty() || args[0] == "status";
    if (!status || args.size() > 2) {
        return "error=usage: log_stats status [top]\n";
    }
    if (args.size() == 2) {
        const auto& text = args[1];
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), top);
        if (ec != std::errc{} || end != text.data() + text.size() || !top) {
            return "error=usage: log_stats status [top]\n";
        }
    }
    struct Row {
        SiteKey key;
        Site site;
        double rate;
    };
    std::vector<Row> rows;
    const auto now = Clock::now();
    for (auto& shard : g_shards) {
        std::scoped_lock lock{shard.mutex};
        for (const auto& [key, site] : shard.sites) {
            rows.push_back({key, site, RecentRate(site, now)});
        }
    }
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << "lines: " << g_lines.load() << "\nmessage_bytes: " << g_bytes.load()
        << "\nsites: " << rows.size() << "\nrecent_window_s: "
        << std::chrono::duration_cast<std::chrono::seconds>(RateWindow).count() << '\n';
    const auto print_sites = [&](const char* title, bool active_only, auto&& before) {
        std::ranges::sort(rows, before);
        out << title << " (lines/s lines MiB class site):\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(top, rows.size()); ++i) {
            const auto& [key, site, rate] = rows[i];
            if (active_only && rate == 0.0) {
                break;
            }
            out << "  " << rate << ' ' << site.lines << ' ' << double(site.bytes) / (1 << 20) << ' '
                << NameOf(site.log_class) << ' ' << Basename(key.file) << ':' << key.line << ' '
                << site.func << '\n';
        }
    };
    print_sites("sites_by_recent_rate", true,
                [](const Row& a, const Row& b) { return a.rate > b.rate; });
    print_sites("sites_by_total_lines", false,
                [](const Row& a, const Row& b) { return a.site.lines > b.site.lines; });
    std::vector<std::pair<std::uint64_t, std::size_t>> classes;
    for (std::size_t i = 0; i < g_class_lines.size(); ++i) {
        if (const auto value = g_class_lines[i].load()) {
            classes.emplace_back(value, i);
        }
    }
    std::ranges::sort(classes, std::greater{});
    out << "classes_by_total_lines (lines MiB class):\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(top, classes.size()); ++i) {
        const auto index = classes[i].second;
        out << "  " << classes[i].first << ' ' << double(g_class_bytes[index].load()) / (1 << 20)
            << ' ' << NameOf(static_cast<Class>(index)) << '\n';
    }
    return out.str();
}

} // namespace Common::Log::Stats
