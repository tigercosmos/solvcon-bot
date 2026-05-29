#pragma once

#include <string>
#include <string_view>

namespace modmesh_bot
{

enum class LogLevel
{
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

// Each log line is one record:
//   YYYY-MM-DDTHH:MM:SS.mmmZ <level> <component> <message>
// The timestamp is UTC. Output goes to stderr. This format is single-line
// so log aggregators can split on newlines; key/value-rich messages are
// the caller's responsibility (use composed strings).
void log(LogLevel level, std::string_view component, std::string_view msg);

inline void log_debug(std::string_view c, std::string_view m) { log(LogLevel::Debug, c, m); }
inline void log_info (std::string_view c, std::string_view m) { log(LogLevel::Info,  c, m); }
inline void log_warn (std::string_view c, std::string_view m) { log(LogLevel::Warn,  c, m); }
inline void log_error(std::string_view c, std::string_view m) { log(LogLevel::Error, c, m); }

// Filter: messages with level < threshold are dropped. Default is Info.
// Reads MODMESH_BOT_LOG_LEVEL env var on first use; values are case-
// insensitive: "debug"|"info"|"warn"|"error".
LogLevel current_log_level();

} // namespace modmesh_bot
