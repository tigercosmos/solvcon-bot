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
    if (g_failures != 0)
    {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
