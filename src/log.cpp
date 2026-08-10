#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>

namespace solvcon_bot
{

namespace
{

const char * level_str(LogLevel l)
{
    switch (l)
    {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "?";
}

// Replace any \r, \n, or other control character that would split a log
// line. We escape \r/\n explicitly so messages remain readable; other
// control bytes get replaced with '?'.
std::string sanitize_one_line(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char ch : s)
    {
        const unsigned char b = static_cast<unsigned char>(ch);
        switch (b)
        {
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out.push_back('\t'); break;
        default:
            if (b < 0x20)
            {
                out.push_back('?');
            }
            else
            {
                out.push_back(ch);
            }
        }
    }
    return out;
}

LogLevel parse_level(const char * s)
{
    if (s == nullptr || *s == '\0') return LogLevel::Info;
    // Case-insensitive compare.
    auto eq = [](const char * a, const char * b) {
        for (; *a && *b; ++a, ++b)
        {
            char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
            if (ca != cb) return false;
        }
        return *a == '\0' && *b == '\0';
    };
    if (eq(s, "debug")) return LogLevel::Debug;
    if (eq(s, "info"))  return LogLevel::Info;
    if (eq(s, "warn"))  return LogLevel::Warn;
    if (eq(s, "error")) return LogLevel::Error;
    return LogLevel::Info;
}

std::mutex & log_mutex()
{
    static std::mutex m;
    return m;
}

} // namespace

LogLevel current_log_level()
{
    static LogLevel cached = parse_level(std::getenv("SOLVCON_BOT_LOG_LEVEL"));
    return cached;
}

void log(LogLevel level, std::string_view component, std::string_view msg)
{
    if (level < current_log_level()) return;

    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - secs).count();

    const std::time_t tt = clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif

    char ts[40];
    std::snprintf(ts, sizeof(ts),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        static_cast<long long>(ms));

    const std::string safe_component = sanitize_one_line(component);
    const std::string safe_msg = sanitize_one_line(msg);

    std::lock_guard<std::mutex> lk(log_mutex());
    std::cerr << ts << ' ' << level_str(level)
              << ' ' << safe_component << ' ' << safe_msg << '\n';
    std::cerr.flush();
}

} // namespace solvcon_bot
