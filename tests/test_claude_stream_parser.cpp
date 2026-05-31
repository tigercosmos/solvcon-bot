#include "claude_stream_parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace mb = modmesh_bot;

namespace
{

int g_failures = 0;

#define EXPECT(cond)                                                   \
    do                                                                 \
    {                                                                  \
        if (!(cond))                                                   \
        {                                                              \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__        \
                      << ": " #cond "\n";                              \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

#define EXPECT_EQ(a, b)                                                \
    do                                                                 \
    {                                                                  \
        const auto & _a = (a);                                         \
        const auto & _b = (b);                                         \
        if (!(_a == _b))                                               \
        {                                                              \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__        \
                      << ": " #a " != " #b ": " << _a << " vs " << _b  \
                      << "\n";                                         \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

// Realistic stream-json for a trivial PROBE turn. Includes init, one
// assistant turn with a single text block, and the final result event.
// JSON-escaped so the C++ source stays readable.
const char * const k_probe_stream =
    "{\"type\":\"system\",\"subtype\":\"init\","
    "\"cwd\":\"/Users/tigercosmos/modmesh-bot\","
    "\"session_id\":\"6fb69400-e704-4aae-9891-e07a2d04bf90\","
    "\"model\":\"claude-opus-4-8\"}\n"
    "{\"type\":\"assistant\",\"message\":{\"id\":\"msg_xyz\","
    "\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-opus-4-8\","
    "\"content\":[{\"type\":\"text\",\"text\":\"PROBE\"}],"
    "\"stop_reason\":null,\"stop_sequence\":null,"
    "\"usage\":{\"input_tokens\":2760,\"output_tokens\":1}}}\n"
    "{\"type\":\"rate_limit_event\",\"rate_limit_info\":"
    "{\"status\":\"allowed\"}}\n"
    "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
    "\"duration_ms\":2485,\"num_turns\":1,\"result\":\"PROBE\","
    "\"total_cost_usd\":0.1378}\n";

// Test driver: count emitted progress lines and concatenate them.
struct CaptureSink
{
    std::string text;
    int count = 0;
    void operator()(std::string_view line)
    {
        text.append(line.data(), line.size());
        ++count;
    }
};

void test_result_event_is_authoritative_body()
{
    mb::ClaudeStreamParser p;
    p.feed(k_probe_stream);
    EXPECT(p.saw_result());
    EXPECT(!p.saw_error());
    EXPECT_EQ(p.final_text(), std::string("PROBE"));
    // line_count counts every line we attempted to parse (init,
    // assistant, rate_limit, result == 4). parse_error_count must be 0.
    EXPECT_EQ(p.line_count(), 4);
    EXPECT_EQ(p.parse_error_count(), 0);
    EXPECT_EQ(p.assistant_turn_count(), 1);
}

void test_progress_sink_emits_one_line_per_noteworthy_event()
{
    mb::ClaudeStreamParser p;
    CaptureSink sink;
    p.set_progress_sink(std::ref(sink));
    p.feed(k_probe_stream);
    // init + assistant text turn + result = 3 lines. rate_limit_event
    // (status=allowed) is intentionally suppressed.
    EXPECT_EQ(sink.count, 3);
    EXPECT(sink.text.find("claude: init model=claude-opus-4-8") != std::string::npos);
    EXPECT(sink.text.find("claude: turn 1 text(5b): PROBE") != std::string::npos);
    EXPECT(sink.text.find("claude: done in 2.5s, turns=1") != std::string::npos);
}

void test_no_sink_is_silent()
{
    mb::ClaudeStreamParser p;
    // Default sink is empty — body extraction still works, just nothing
    // emitted. This is the bot daemon's mode.
    p.feed(k_probe_stream);
    EXPECT(p.saw_result());
    EXPECT_EQ(p.final_text(), std::string("PROBE"));
}

void test_chunks_split_mid_line_reassemble_correctly()
{
    // Feed the same stream one byte at a time. The partial-line buffer
    // must accumulate across feeds without dropping bytes.
    mb::ClaudeStreamParser p;
    const std::string s(k_probe_stream);
    for (char c : s)
    {
        const std::string_view chunk(&c, 1);
        p.feed(chunk);
    }
    EXPECT_EQ(p.final_text(), std::string("PROBE"));
    EXPECT_EQ(p.line_count(), 4);
    EXPECT_EQ(p.parse_error_count(), 0);
}

void test_partial_trailing_line_processed_by_flush()
{
    // Child closed stdout without a final newline (e.g. mid-event
    // crash). flush() must process whatever's buffered. We replay the
    // probe stream's result line without trailing newline.
    mb::ClaudeStreamParser p;
    p.feed("{\"type\":\"result\",\"subtype\":\"success\","
           "\"is_error\":false,\"duration_ms\":100,\"num_turns\":1,"
           "\"result\":\"NO_NEWLINE\",\"total_cost_usd\":0.001}");
    // Before flush: nothing parsed yet (no '\n' seen).
    EXPECT(!p.saw_result());
    p.flush();
    EXPECT(p.saw_result());
    EXPECT_EQ(p.final_text(), std::string("NO_NEWLINE"));
}

void test_is_error_true_propagates()
{
    mb::ClaudeStreamParser p;
    p.feed("{\"type\":\"result\",\"subtype\":\"error\","
           "\"is_error\":true,\"duration_ms\":50,\"num_turns\":0,"
           "\"result\":\"context too long\",\"total_cost_usd\":0.0}\n");
    EXPECT(p.saw_result());
    EXPECT(p.saw_error());
    // Body still surfaced so the caller can include it in the error.
    EXPECT_EQ(p.final_text(), std::string("context too long"));
}

void test_unparseable_line_skipped_not_fatal()
{
    // A garbage line between two real events must not break parsing.
    mb::ClaudeStreamParser p;
    p.feed("{\"type\":\"system\",\"subtype\":\"init\","
           "\"model\":\"m\",\"session_id\":\"s\"}\n");
    p.feed("\xff\xfe not json at all\n");
    p.feed("{\"type\":\"result\",\"subtype\":\"success\","
           "\"is_error\":false,\"duration_ms\":1,\"num_turns\":1,"
           "\"result\":\"DONE\",\"total_cost_usd\":0.0}\n");
    EXPECT_EQ(p.final_text(), std::string("DONE"));
    EXPECT(p.parse_error_count() >= 1);
}

void test_fallback_assistant_text_when_no_result_event()
{
    // claude killed mid-stream: we saw assistant turns but never a
    // result event. Fallback returns the concatenated text so the
    // operator at least sees something coherent.
    mb::ClaudeStreamParser p;
    p.feed("{\"type\":\"assistant\",\"message\":{"
           "\"content\":[{\"type\":\"text\",\"text\":\"partial \"}]}}\n");
    p.feed("{\"type\":\"assistant\",\"message\":{"
           "\"content\":[{\"type\":\"text\",\"text\":\"output\"}]}}\n");
    EXPECT(!p.saw_result());
    EXPECT_EQ(p.final_text(), std::string("partial output"));
}

void test_tool_use_block_logged_but_does_not_break_text_capture()
{
    mb::ClaudeStreamParser p;
    CaptureSink sink;
    p.set_progress_sink(std::ref(sink));
    // A realistic tool-using turn followed by tool_result and final.
    p.feed("{\"type\":\"assistant\",\"message\":{"
           "\"content\":["
             "{\"type\":\"text\",\"text\":\"Let me check.\"},"
             "{\"type\":\"tool_use\",\"name\":\"Read\","
             "\"input\":{\"file_path\":\"/x\"}}"
           "]}}\n");
    p.feed("{\"type\":\"user\",\"message\":{\"content\":["
           "{\"type\":\"tool_result\",\"content\":\"contents\"}]}}\n");
    p.feed("{\"type\":\"result\",\"subtype\":\"success\","
           "\"is_error\":false,\"duration_ms\":1,\"num_turns\":2,"
           "\"result\":\"FINAL\",\"total_cost_usd\":0.0}\n");
    EXPECT_EQ(p.final_text(), std::string("FINAL"));
    EXPECT(sink.text.find("tool_use Read") != std::string::npos);
    EXPECT(sink.text.find("tool result received") != std::string::npos);
}

// Realistic stream payload when claude is invoked with
// --include-partial-messages. Captured shape mirrors the probe taken
// against a real `claude -p --output-format stream-json --verbose
// --include-partial-messages` invocation: status=requesting fires
// before the first stream_event; per-block start/delta/stop lifecycle
// events accompany the text content; a consolidated assistant event
// arrives after each completed block sharing the same message id.
const char * const k_partial_stream =
    "{\"type\":\"system\",\"subtype\":\"init\","
    "\"session_id\":\"abc\",\"model\":\"claude-opus-4-8\"}\n"
    "{\"type\":\"system\",\"subtype\":\"status\","
    "\"status\":\"requesting\"}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\","
    "\"message\":{\"id\":\"msg_partial\",\"role\":\"assistant\"}}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\","
    "\"index\":0,\"content_block\":{\"type\":\"thinking\","
    "\"thinking\":\"\",\"signature\":\"\"}}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
    "\"index\":0,\"delta\":{\"type\":\"signature_delta\","
    "\"signature\":\"deadbeef\"}}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_stop\","
    "\"index\":0}}\n"
    "{\"type\":\"assistant\",\"message\":{\"id\":\"msg_partial\","
    "\"role\":\"assistant\","
    "\"content\":[{\"type\":\"thinking\",\"thinking\":\"\"}]}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\","
    "\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
    "\"index\":1,\"delta\":{\"type\":\"text_delta\","
    "\"text\":\"Hello \"}}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
    "\"index\":1,\"delta\":{\"type\":\"text_delta\","
    "\"text\":\"world\"}}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_stop\","
    "\"index\":1}}\n"
    "{\"type\":\"assistant\",\"message\":{\"id\":\"msg_partial\","
    "\"role\":\"assistant\","
    "\"content\":[{\"type\":\"text\",\"text\":\"Hello world\"}]}}\n"
    "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
    "\"duration_ms\":1500,\"num_turns\":1,"
    "\"result\":\"Hello world\",\"total_cost_usd\":0.01}\n";

void test_stream_event_text_deltas_route_to_text_sink_verbatim()
{
    // The crux: live tokens should reach the operator BEFORE the
    // assistant event consolidates the message.
    mb::ClaudeStreamParser p;
    std::string streamed;
    p.set_text_delta_sink(
        [&streamed](std::string_view chunk)
        { streamed.append(chunk.data(), chunk.size()); });
    p.feed(k_partial_stream);
    EXPECT_EQ(streamed, std::string("Hello world"));
    EXPECT_EQ(p.final_text(), std::string("Hello world"));
    // Two assistant events but same message id -> one turn.
    EXPECT_EQ(p.assistant_turn_count(), 1);
}

void test_stream_event_progress_lines_cover_lifecycle()
{
    mb::ClaudeStreamParser p;
    CaptureSink sink;
    p.set_progress_sink(std::ref(sink));
    // No text sink: we just want to verify the progress lines.
    p.feed(k_partial_stream);
    // We expect lifecycle lines for: init, requesting, thinking,
    // writing text, text done, done. Plus the assistant-event
    // per-block "turn 1 text(11b):" emit (kept for callers that
    // don't install a text sink). The order should be:
    //   init -> requesting -> thinking -> writing text -> text done
    //   -> turn 1 text(...): preview -> done
    EXPECT(sink.text.find("claude: init") != std::string::npos);
    EXPECT(sink.text.find("claude: requesting") != std::string::npos);
    EXPECT(sink.text.find("claude: thinking...") != std::string::npos);
    EXPECT(sink.text.find("claude: writing text...") != std::string::npos);
    EXPECT(sink.text.find("claude: text done (11 chars)") != std::string::npos);
    EXPECT(sink.text.find("claude: done in 1.5s, turns=1") != std::string::npos);
}

void test_stream_event_text_sink_unset_leaves_capture_intact()
{
    // Without a text sink, deltas are silently parsed (chars counted
    // for the text_done line) but never streamed. final_text still
    // comes from the result event.
    mb::ClaudeStreamParser p;
    CaptureSink sink;
    p.set_progress_sink(std::ref(sink));
    p.feed(k_partial_stream);
    EXPECT_EQ(p.final_text(), std::string("Hello world"));
    EXPECT(sink.text.find("text done (11 chars)") != std::string::npos);
}

void test_unterminated_text_stream_gets_newline_before_progress()
{
    // Regression: when the last text_delta does NOT end with a newline
    // (the common case — claude rarely ends with one) the next
    // line-framed progress emit must not glue onto the streamed body.
    // emit() must inject a "\n" via the text-delta sink first.
    mb::ClaudeStreamParser p;
    std::string streamed;
    p.set_text_delta_sink(
        [&streamed](std::string_view chunk)
        { streamed.append(chunk.data(), chunk.size()); });
    CaptureSink progress;
    p.set_progress_sink(std::ref(progress));
    p.feed("{\"type\":\"stream_event\",\"event\":{\"type\":"
           "\"content_block_start\",\"index\":0,"
           "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}}\n");
    p.feed("{\"type\":\"stream_event\",\"event\":{\"type\":"
           "\"content_block_delta\",\"index\":0,\"delta\":{"
           "\"type\":\"text_delta\",\"text\":\"hello\"}}}\n");
    p.feed("{\"type\":\"stream_event\",\"event\":{\"type\":"
           "\"content_block_stop\",\"index\":0}}\n");
    // The streamed body should be "hello\n" — the trailing newline
    // was injected BY emit() right before "claude: text done"
    // landed on the progress sink.
    EXPECT_EQ(streamed, std::string("hello\n"));
    EXPECT(progress.text.find("text done (5 chars)") != std::string::npos);
}

void test_text_stream_ending_with_newline_does_not_double_newline()
{
    // Counterpart to above: if claude's last delta already ends with
    // '\n' (rare but possible), we MUST NOT inject another one.
    mb::ClaudeStreamParser p;
    std::string streamed;
    p.set_text_delta_sink(
        [&streamed](std::string_view chunk)
        { streamed.append(chunk.data(), chunk.size()); });
    p.set_progress_sink([](std::string_view){}); // installed but discards
    p.feed("{\"type\":\"stream_event\",\"event\":{\"type\":"
           "\"content_block_start\",\"index\":0,"
           "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}}\n");
    p.feed("{\"type\":\"stream_event\",\"event\":{\"type\":"
           "\"content_block_delta\",\"index\":0,\"delta\":{"
           "\"type\":\"text_delta\",\"text\":\"hello\\n\"}}}\n");
    p.feed("{\"type\":\"stream_event\",\"event\":{\"type\":"
           "\"content_block_stop\",\"index\":0}}\n");
    EXPECT_EQ(streamed, std::string("hello\n")); // exactly one newline
}

void test_partial_assistant_events_dedupe_turn_count()
{
    // Three assistant events sharing one message id: one logical
    // turn. (Would be three if the dedupe broke.)
    mb::ClaudeStreamParser p;
    p.feed("{\"type\":\"assistant\",\"message\":{\"id\":\"same\","
           "\"content\":[{\"type\":\"thinking\",\"thinking\":\"\"}]}}\n");
    p.feed("{\"type\":\"assistant\",\"message\":{\"id\":\"same\","
           "\"content\":[{\"type\":\"text\",\"text\":\"A\"}]}}\n");
    p.feed("{\"type\":\"assistant\",\"message\":{\"id\":\"same\","
           "\"content\":[{\"type\":\"text\",\"text\":\"B\"}]}}\n");
    EXPECT_EQ(p.assistant_turn_count(), 1);
    // Two text blocks across the three events: fallback accumulates
    // both.
    EXPECT_EQ(p.final_text(), std::string("AB"));
}

} // namespace

int main()
{
    test_result_event_is_authoritative_body();
    test_progress_sink_emits_one_line_per_noteworthy_event();
    test_no_sink_is_silent();
    test_chunks_split_mid_line_reassemble_correctly();
    test_partial_trailing_line_processed_by_flush();
    test_is_error_true_propagates();
    test_unparseable_line_skipped_not_fatal();
    test_fallback_assistant_text_when_no_result_event();
    test_tool_use_block_logged_but_does_not_break_text_capture();
    test_stream_event_text_deltas_route_to_text_sink_verbatim();
    test_stream_event_progress_lines_cover_lifecycle();
    test_stream_event_text_sink_unset_leaves_capture_intact();
    test_unterminated_text_stream_gets_newline_before_progress();
    test_text_stream_ending_with_newline_does_not_double_newline();
    test_partial_assistant_events_dedupe_turn_count();
    if (g_failures != 0)
    {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
