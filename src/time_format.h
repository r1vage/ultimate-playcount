#pragma once
// Text forms of the date fields. No foobar2000 dependency.
#include <cstdint>
#include <ctime>
#include <string>

namespace upc {

inline std::tm local_tm(int64_t ts) {
    std::tm tm{};
    const std::time_t t = static_cast<std::time_t>(ts);
    localtime_s(&tm, &t);
    return tm;
}

// "2026-10-16 13:05:09", local time.
inline std::string format_date(int64_t ts) {
    const std::tm tm = local_tm(ts);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// Elapsed time, two largest units: "3wk 2d", "5d 4h", "2h 7m", "4m 10s", "12s".
// shortcut: wording follows Playcount 2003's unit strings; compare side by side before relying on it in scripts.
inline std::string format_ago(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    const int64_t w = seconds / 604800, d = seconds / 86400 % 7, h = seconds / 3600 % 24, m = seconds / 60 % 60,
                  s = seconds % 60;
    auto two = [](int64_t a, const char* ua, int64_t b, const char* ub) {
        return std::to_string(a) + ua + " " + std::to_string(b) + ub;
    };
    if (w) return two(w, "wk", d, "d");
    if (seconds >= 86400) return two(seconds / 86400, "d", h, "h");
    if (h) return two(h, "h", m, "m");
    if (m) return two(m, "m", s, "s");
    return std::to_string(s) + "s";
}

// Calendar days back: "Today", "Yesterday", else "1y 2m 1w 3d" (zero parts left out).
inline std::string format_ago2(int64_t ts, int64_t now) {
    auto day_number = [](int64_t t) {
        std::tm tm = local_tm(t);
        tm.tm_hour = 12; // noon: immune to DST shifts
        tm.tm_min = tm.tm_sec = 0;
        tm.tm_isdst = -1;
        return static_cast<int64_t>(std::mktime(&tm)) / 86400;
    };
    int64_t days = day_number(now) - day_number(ts);
    if (days <= 0) return "Today";
    if (days == 1) return "Yesterday";
    std::string out;
    auto part = [&](int64_t n, const char* unit) {
        if (!n) return;
        if (!out.empty()) out += ' ';
        out += std::to_string(n) + unit;
    };
    part(days / 365, "y");
    days %= 365;
    part(days / 30, "m");
    days %= 30;
    part(days / 7, "w");
    part(days % 7, "d");
    return out;
}

} // namespace upc
