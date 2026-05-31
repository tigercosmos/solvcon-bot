#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace modmesh_bot
{

// Incremental parser for `claude -p --output-format stream-json --verbose`
// output. Claude writes one JSON event per line on stdout; this class is
// fed chunks of those bytes as they arrive (boundaries do NOT need to
// align with line breaks) and:
//
//   * captures the `result` event's `result` field as the authoritative
//     final review body, returned via final_text(),
//   * fans noteworthy events out to an optional progress sink as short
//     human-readable lines (init, assistant turn, tool use, tool result,
//     final result) so an operator with stream_io on can watch claude
//     work without the raw JSON noise.
//
// The parser is silent when no progress sink is set; the bot daemon uses
// it that way to keep its logs clean while still extracting the body.
class ClaudeStreamParser
{
public:
    using ProgressSink = std::function<void(std::string_view)>;

    ClaudeStreamParser();
    ~ClaudeStreamParser();

    ClaudeStreamParser(const ClaudeStreamParser &) = delete;
    ClaudeStreamParser & operator=(const ClaudeStreamParser &) = delete;

    // Install the progress sink. Each emitted line is already
    // newline-terminated. Pass {} to disable progress output (default).
    void set_progress_sink(ProgressSink sink);

    // Feed a chunk of NDJSON bytes from claude's stdout. Complete lines
    // are parsed and dispatched immediately; partial trailing lines are
    // buffered until the next call (or flush()).
    void feed(std::string_view chunk);

    // Process any buffered partial line as if it were complete. Call
    // after the child closes its stdout so a tail without trailing
    // newline still gets parsed. Idempotent.
    void flush();

    // The `result` event's `result` field, captured at end-of-stream.
    // If no `result` event was seen but the parser observed assistant
    // text turns, returns the concatenation of all text content blocks
    // as a defensive fallback. Empty if the parser saw nothing usable.
    std::string final_text() const;

    // True iff a `result` event arrived with is_error=true.
    bool saw_error() const { return m_saw_error; }
    // True iff a `result` event was observed (regardless of is_error).
    bool saw_result() const { return m_saw_result; }

    // Diagnostics for tests and operator-facing summaries.
    int assistant_turn_count() const { return m_assistant_turn_count; }
    int line_count() const { return m_line_count; }
    int parse_error_count() const { return m_parse_error_count; }

private:
    void process_line(std::string_view line);
    void emit(std::string line);

    std::string m_partial_line;
    ProgressSink m_sink;
    std::string m_final_result;
    std::string m_assistant_text_fallback;
    bool m_saw_result = false;
    bool m_saw_error = false;
    int m_assistant_turn_count = 0;
    int m_line_count = 0;
    int m_parse_error_count = 0;
};

} // namespace modmesh_bot
