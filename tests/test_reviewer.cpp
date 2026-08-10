// Tests for src/reviewer.* — the IReviewer factory, MockReviewer, and
// the codexmon-backed AgentReviewer. Invocation shape is asserted via
// agent_build_invocation_for_test; the run() path is exercised against
// a fake codexmon shell script so no real AI CLI (or network) is
// needed. The live codexmon spawn is covered in scripts/e2e_*.

#include "config.hpp"
#include "reviewer.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace solvcon_bot
{
// Test-only handle defined in reviewer_agent.cpp.
ReviewerInvocation agent_build_invocation_for_test(
    const Config & cfg, const std::string & diff);
} // namespace solvcon_bot

namespace
{

int g_failed = 0;
int g_passed = 0;

#define EXPECT(expr)                                                         \
    do                                                                       \
    {                                                                        \
        if (!(expr))                                                         \
        {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << ": " << #expr << "\n";                              \
            ++g_failed;                                                      \
        }                                                                    \
        else { ++g_passed; }                                                 \
    } while (0)

#define EXPECT_EQ(a, b)                                                      \
    do                                                                       \
    {                                                                        \
        auto _a = (a);                                                       \
        auto _b = (b);                                                       \
        if (!(_a == _b))                                                     \
        {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << ": " << #a << " == " << #b                          \
                      << "\n  got: <" << _a << ">"                           \
                      << "\n  want: <" << _b << ">\n";                       \
            ++g_failed;                                                      \
        }                                                                    \
        else { ++g_passed; }                                                 \
    } while (0)

using solvcon_bot::Config;
using solvcon_bot::ReviewerError;
using solvcon_bot::ReviewerKind;

namespace mb = solvcon_bot;

Config make_cfg(ReviewerKind kind)
{
    Config c;
    c.reviewer_kind = kind;
    c.max_output_bytes = 8192;
    c.subprocess_timeout_sec = 5;
    return c;
}

bool argv_contains_pair(const std::vector<std::string> & argv,
                        const std::string & flag,
                        const std::string & value)
{
    for (std::size_t i = 0; i + 1 < argv.size(); ++i)
    {
        if (argv[i] == flag && argv[i + 1] == value) return true;
    }
    return false;
}

std::size_t index_of(const std::vector<std::string> & argv,
                     const std::string & token)
{
    auto it = std::find(argv.begin(), argv.end(), token);
    return static_cast<std::size_t>(it - argv.begin());
}

// --- fake codexmon fixture ------------------------------------------------

// A scratch dir holding a fake `codexmon` shell script plus the files
// it reads/writes. The script captures its argv and stdin, then prints
// a canned status JSON and exits with a canned code — everything the
// AgentReviewer consumes from real codexmon.
struct FakeCodexmon
{
    std::string dir;

    FakeCodexmon()
    {
        char tmpl[] = "/tmp/solvcon-bot-fake-codexmon-XXXXXX";
        char * d = ::mkdtemp(tmpl);
        if (d == nullptr) throw std::runtime_error("mkdtemp failed");
        dir = d;
    }

    ~FakeCodexmon()
    {
        // Best-effort cleanup; the files are tiny and /tmp-rooted.
        const std::string cmd = "rm -rf '" + dir + "'";
        (void)std::system(cmd.c_str());
    }

    std::string path(const std::string & name) const { return dir + "/" + name; }

    void write_file(const std::string & name, const std::string & content) const
    {
        std::ofstream ofs(path(name), std::ios::binary);
        ofs << content;
    }

    std::string read_file(const std::string & name) const
    {
        std::ifstream ifs(path(name), std::ios::binary);
        std::ostringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }

    // Install the fake binary. `status_json` is printed on stdout;
    // stdin and argv are captured into files for later assertions.
    std::string install(const std::string & status_json, int exit_code) const
    {
        write_file("status.json", status_json);
        std::ostringstream sh;
        sh << "#!/bin/sh\n"
           << "printf '%s\\n' \"$@\" > '" << path("argv.txt") << "'\n"
           << "cat > '" << path("stdin.txt") << "'\n"
           << "cat '" << path("status.json") << "'\n"
           << "exit " << exit_code << "\n";
        write_file("codexmon", sh.str());
        ::chmod(path("codexmon").c_str(), 0755);
        return path("codexmon");
    }
};

// --- assemble_review_stdin + default_review_prompt -----------------------

void test_default_prompt_is_non_empty()
{
    EXPECT(!mb::default_review_prompt().empty());
    EXPECT(mb::default_review_prompt().find("diff") != std::string::npos);
}

void test_assemble_review_stdin_shape()
{
    const std::string out = mb::assemble_review_stdin("PROMPT", "DIFF\n");
    EXPECT(out.find("PROMPT") == 0);
    EXPECT(out.find("\nBEGIN_DIFF\n") != std::string::npos);
    EXPECT(out.find("\nDIFF\n") != std::string::npos);
    // END_DIFF is a line of its own.
    EXPECT(out.find("\nEND_DIFF\n") != std::string::npos);
}

void test_assemble_review_stdin_adds_trailing_newline()
{
    const std::string out = mb::assemble_review_stdin("P", "X");
    EXPECT(out.find("X\nEND_DIFF\n") != std::string::npos);
}

void test_assemble_review_stdin_empty_diff()
{
    const std::string out = mb::assemble_review_stdin("P", "");
    EXPECT(out == std::string("P\n\nBEGIN_DIFF\nEND_DIFF\n"));
}

void test_truncation_note_appended_only_when_truncated()
{
    EXPECT_EQ(mb::maybe_append_truncation_note("hi", false), std::string("hi"));
    const std::string out = mb::maybe_append_truncation_note("hi", true);
    EXPECT(out.find("solvcon-bot: reviewer output truncated") != std::string::npos);
    EXPECT(out.find("hi") == 0);
}

// --- MockReviewer ---------------------------------------------------------

void test_mock_echoes_diff_by_default()
{
    Config c = make_cfg(ReviewerKind::Mock);
    auto r = mb::make_reviewer(c);
    EXPECT_EQ(r->kind(), ReviewerKind::Mock);
    EXPECT_EQ(r->run("diff body\n"), std::string("diff body\n"));
}

void test_mock_fixed_output_overrides_diff()
{
    Config c = make_cfg(ReviewerKind::Mock);
    c.reviewer_mock_output = "mocked review\n";
    auto r = mb::make_reviewer(c);
    EXPECT_EQ(r->run("anything"), std::string("mocked review\n"));
}

void test_mock_forced_failure_throws()
{
    Config c = make_cfg(ReviewerKind::Mock);
    c.reviewer_mock_exit_code = 17;
    auto r = mb::make_reviewer(c);
    bool threw = false;
    std::string msg;
    try { r->run("anything"); }
    catch (const ReviewerError & e) { threw = true; msg = e.what(); }
    EXPECT(threw);
    EXPECT(msg.find("exited 17") != std::string::npos);
    EXPECT(msg.find("simulated failure") != std::string::npos);
}

// --- AgentReviewer invocation shape (no spawn) ----------------------------

void test_claude_invocation_defaults()
{
    // Empty REVIEWER_MODEL / REVIEWER_EFFORT inject the per-agent
    // defaults. codexmon carries the monitoring; the agent-native argv
    // after `--` stays minimal (codexmon injects the stream flags).
    Config c = make_cfg(ReviewerKind::Claude);
    auto inv = mb::agent_build_invocation_for_test(c, "diff bytes");
    EXPECT_EQ(inv.argv.size(), static_cast<std::size_t>(12));
    EXPECT_EQ(inv.argv[0], std::string("codexmon"));
    EXPECT_EQ(inv.argv[1], std::string("run"));
    EXPECT_EQ(inv.argv[2], std::string("--json"));
    EXPECT_EQ(inv.argv[3], std::string("--stdin"));
    EXPECT(argv_contains_pair(inv.argv, "--agent", "claude"));
    EXPECT(argv_contains_pair(inv.argv, "--wall-timeout", "5"));
    // Agent args come after the `--` separator.
    const std::size_t sep = index_of(inv.argv, "--");
    EXPECT(sep < inv.argv.size());
    EXPECT_EQ(inv.argv[sep + 1], std::string("-p"));
    EXPECT(argv_contains_pair(inv.argv, "--model", "claude-opus-4-8"));
    // Unset knobs must not surface as flags.
    EXPECT_EQ(index_of(inv.argv, "--idle-timeout"), inv.argv.size());
    EXPECT_EQ(index_of(inv.argv, "--heartbeat"), inv.argv.size());
    // stdin includes the default prompt + diff block.
    EXPECT(inv.stdin_input.find(mb::default_review_prompt()) == 0);
    EXPECT(inv.stdin_input.find("diff bytes") != std::string::npos);
    // CLAUDE_EFFORT defaults to high (env is inherited through codexmon).
    EXPECT_EQ(inv.env_values.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(inv.env_values[0].first, std::string("CLAUDE_EFFORT"));
    EXPECT_EQ(inv.env_values[0].second, std::string("high"));
}

void test_codex_invocation_defaults()
{
    Config c = make_cfg(ReviewerKind::Codex);
    auto inv = mb::agent_build_invocation_for_test(c, "diff bytes");
    EXPECT(argv_contains_pair(inv.argv, "--agent", "codex"));
    const std::size_t sep = index_of(inv.argv, "--");
    EXPECT(sep < inv.argv.size());
    EXPECT_EQ(inv.argv[sep + 1], std::string("exec"));
    EXPECT(argv_contains_pair(inv.argv, "-c", "reasoning.effort=high"));
    EXPECT(argv_contains_pair(inv.argv, "--model", "gpt-5.5"));
    // codex receives effort via argv, NOT as an env var.
    EXPECT(inv.env_values.empty());
}

void test_cursor_invocation_defaults()
{
    Config c = make_cfg(ReviewerKind::Cursor);
    auto inv = mb::agent_build_invocation_for_test(c, "x");
    EXPECT(argv_contains_pair(inv.argv, "--agent", "cursor"));
    const std::size_t sep = index_of(inv.argv, "--");
    EXPECT_EQ(inv.argv[sep + 1], std::string("-p"));
    // No default model for cursor — codexmon picks its own.
    EXPECT_EQ(index_of(inv.argv, "--model"), inv.argv.size());
    EXPECT(inv.env_values.empty());
}

void test_invocation_overrides_and_optional_flags()
{
    Config c = make_cfg(ReviewerKind::Codex);
    c.reviewer_model = "gpt-5";
    c.reviewer_effort = "xhigh";
    c.reviewer_heartbeat_sec = 15;
    c.reviewer_idle_timeout_sec = 0;
    c.codexmon_bin = "/opt/bin/codexmon";
    c.reviewer_env_passthrough = {"OPENAI_API_KEY"};
    auto inv = mb::agent_build_invocation_for_test(c, "x");
    EXPECT_EQ(inv.argv[0], std::string("/opt/bin/codexmon"));
    EXPECT(argv_contains_pair(inv.argv, "--model", "gpt-5"));
    EXPECT(argv_contains_pair(inv.argv, "-c", "reasoning.effort=xhigh"));
    EXPECT(argv_contains_pair(inv.argv, "--heartbeat", "15"));
    EXPECT(argv_contains_pair(inv.argv, "--idle-timeout", "0"));
    // Monitor flags stay before the `--` separator; agent args after.
    const std::size_t sep = index_of(inv.argv, "--");
    EXPECT(index_of(inv.argv, "--heartbeat") < sep);
    EXPECT(index_of(inv.argv, "--model") > sep);
    EXPECT_EQ(inv.env_passthrough.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(inv.env_passthrough[0], std::string("OPENAI_API_KEY"));
}

void test_custom_prompt_replaces_default()
{
    Config c = make_cfg(ReviewerKind::Claude);
    c.reviewer_prompt = "SPECIAL PROMPT";
    auto inv = mb::agent_build_invocation_for_test(c, "x");
    EXPECT(inv.stdin_input.find("SPECIAL PROMPT") == 0);
    EXPECT(inv.stdin_input.find(mb::default_review_prompt()) == std::string::npos);
}

// --- parse_codexmon_status -------------------------------------------------

void test_parse_codexmon_status_full()
{
    const auto st = mb::parse_codexmon_status(
        R"({"id":"cdx-1","state":"completed","health":"done",)"
        R"("result_preview":"LGTM","result_file":"/tmp/j/result.txt",)"
        R"("exit_code":0,"elapsed_sec":47.5})");
    EXPECT_EQ(st.state, std::string("completed"));
    EXPECT_EQ(st.error, std::string(""));
    EXPECT_EQ(st.result_file, std::string("/tmp/j/result.txt"));
    EXPECT_EQ(st.result_preview, std::string("LGTM"));
}

void test_parse_codexmon_status_error_fields()
{
    const auto st = mb::parse_codexmon_status(
        R"json({"state":"stalled","error":"idle for 180s (idle timeout 180s)"})json");
    EXPECT_EQ(st.state, std::string("stalled"));
    EXPECT(st.error.find("idle for 180s") != std::string::npos);
    EXPECT(st.result_file.empty());
}

void test_parse_codexmon_status_malformed_throws()
{
    bool threw = false;
    try { (void)mb::parse_codexmon_status("codexmon: boom\n"); }
    catch (const std::exception &) { threw = true; }
    EXPECT(threw);
}

// --- AgentReviewer::run against a fake codexmon ----------------------------

void test_agent_run_success_reads_result_file()
{
    FakeCodexmon fake;
    fake.write_file("result.txt", "## Review\nLooks solid.\n");
    Config c = make_cfg(ReviewerKind::Claude);
    c.codexmon_bin = fake.install(
        R"({"state":"completed","result_preview":"## Review",)"
        R"("result_file":")" + fake.path("result.txt") + R"("})",
        0);
    auto r = mb::make_reviewer(c);
    EXPECT_EQ(r->run("some diff\n"), std::string("## Review\nLooks solid.\n"));
    // The diff (wrapped in the review prompt) was fed on stdin.
    const std::string fed = fake.read_file("stdin.txt");
    EXPECT(fed.find("BEGIN_DIFF") != std::string::npos);
    EXPECT(fed.find("some diff") != std::string::npos);
    // And codexmon saw the monitored-run argv.
    const std::string argv = fake.read_file("argv.txt");
    EXPECT(argv.find("run") == 0);
    EXPECT(argv.find("--agent\nclaude") != std::string::npos);
}

void test_agent_run_falls_back_to_preview_without_result_file()
{
    FakeCodexmon fake;
    Config c = make_cfg(ReviewerKind::Codex);
    c.codexmon_bin = fake.install(
        R"({"state":"completed","result_preview":"short review"})", 0);
    auto r = mb::make_reviewer(c);
    EXPECT_EQ(r->run("d"), std::string("short review"));
}

void test_agent_run_agent_failure_throws_with_context()
{
    FakeCodexmon fake;
    Config c = make_cfg(ReviewerKind::Claude);
    c.codexmon_bin = fake.install(
        R"({"state":"failed","error":"agent exited 2"})", 2);
    auto r = mb::make_reviewer(c);
    bool threw = false;
    std::string msg;
    try { r->run("d"); }
    catch (const ReviewerError & e) { threw = true; msg = e.what(); }
    EXPECT(threw);
    EXPECT(msg.find("failed") != std::string::npos);
    EXPECT(msg.find("codexmon exit 2") != std::string::npos);
    EXPECT(msg.find("agent exited 2") != std::string::npos);
}

void test_agent_run_stall_maps_to_error()
{
    FakeCodexmon fake;
    Config c = make_cfg(ReviewerKind::Claude);
    c.codexmon_bin = fake.install(
        R"({"state":"stalled","error":"idle for 180s"})", 124);
    auto r = mb::make_reviewer(c);
    bool threw = false;
    std::string msg;
    try { r->run("d"); }
    catch (const ReviewerError & e) { threw = true; msg = e.what(); }
    EXPECT(threw);
    EXPECT(msg.find("stalled") != std::string::npos);
    EXPECT(msg.find("124") != std::string::npos);
}

void test_agent_run_garbage_stdout_throws_parse_error()
{
    FakeCodexmon fake;
    Config c = make_cfg(ReviewerKind::Claude);
    c.codexmon_bin = fake.install("not json at all", 0);
    auto r = mb::make_reviewer(c);
    bool threw = false;
    std::string msg;
    try { r->run("d"); }
    catch (const ReviewerError & e) { threw = true; msg = e.what(); }
    EXPECT(threw);
    EXPECT(msg.find("cannot parse codexmon status") != std::string::npos);
}

void test_agent_preflight_missing_binary_throws_install_hint()
{
    Config c = make_cfg(ReviewerKind::Claude);
    c.codexmon_bin = "/nonexistent/codexmon-definitely-missing";
    auto r = mb::make_reviewer(c);
    bool threw = false;
    std::string msg;
    try { r->preflight(); }
    catch (const ReviewerError & e) { threw = true; msg = e.what(); }
    EXPECT(threw);
    EXPECT(msg.find("install_codexmon") != std::string::npos);
}

void test_agent_result_file_capped_at_max_output_bytes()
{
    // MAX_OUTPUT_BYTES caps the review BODY only. The status JSON on
    // codexmon's stdout must survive a cap far smaller than itself —
    // real codexmon ships up to 600 chars of result_preview there
    // (regression: the status capture used to share the body cap,
    // so a small-but-valid cap failed successful reviews).
    FakeCodexmon fake;
    fake.write_file("result.txt", std::string(10000, 'x'));
    Config c = make_cfg(ReviewerKind::Claude);
    c.max_output_bytes = 100;
    c.codexmon_bin = fake.install(
        R"({"state":"completed","result_preview":")" + std::string(600, 'p')
            + R"(","result_file":")" + fake.path("result.txt") + R"("})",
        0);
    auto r = mb::make_reviewer(c);
    const std::string body = r->run("d");
    EXPECT(body.find(std::string(100, 'x')) == 0);
    EXPECT(body.find("truncated at MAX_OUTPUT_BYTES") != std::string::npos);
}

// --- factory dispatch ----------------------------------------------------

void test_factory_dispatches_by_kind()
{
    auto m = mb::make_reviewer(make_cfg(ReviewerKind::Mock));
    EXPECT_EQ(m->kind(), ReviewerKind::Mock);
    auto cl = mb::make_reviewer(make_cfg(ReviewerKind::Claude));
    EXPECT_EQ(cl->kind(), ReviewerKind::Claude);
    auto cx = mb::make_reviewer(make_cfg(ReviewerKind::Codex));
    EXPECT_EQ(cx->kind(), ReviewerKind::Codex);
    auto cu = mb::make_reviewer(make_cfg(ReviewerKind::Cursor));
    EXPECT_EQ(cu->kind(), ReviewerKind::Cursor);
}

void test_parse_reviewer_kind()
{
    EXPECT_EQ(mb::parse_reviewer_kind("mock"), ReviewerKind::Mock);
    EXPECT_EQ(mb::parse_reviewer_kind("MOCK"), ReviewerKind::Mock);
    EXPECT_EQ(mb::parse_reviewer_kind("Claude"), ReviewerKind::Claude);
    EXPECT_EQ(mb::parse_reviewer_kind("codex"), ReviewerKind::Codex);
    EXPECT_EQ(mb::parse_reviewer_kind("cursor"), ReviewerKind::Cursor);
    bool threw = false;
    try { (void)mb::parse_reviewer_kind("gemini"); }
    catch (const std::exception &) { threw = true; }
    EXPECT(threw);
}

} // namespace

int main()
{
    test_default_prompt_is_non_empty();
    test_assemble_review_stdin_shape();
    test_assemble_review_stdin_adds_trailing_newline();
    test_assemble_review_stdin_empty_diff();
    test_truncation_note_appended_only_when_truncated();

    test_mock_echoes_diff_by_default();
    test_mock_fixed_output_overrides_diff();
    test_mock_forced_failure_throws();

    test_claude_invocation_defaults();
    test_codex_invocation_defaults();
    test_cursor_invocation_defaults();
    test_invocation_overrides_and_optional_flags();
    test_custom_prompt_replaces_default();

    test_parse_codexmon_status_full();
    test_parse_codexmon_status_error_fields();
    test_parse_codexmon_status_malformed_throws();

    test_agent_run_success_reads_result_file();
    test_agent_run_falls_back_to_preview_without_result_file();
    test_agent_run_agent_failure_throws_with_context();
    test_agent_run_stall_maps_to_error();
    test_agent_run_garbage_stdout_throws_parse_error();
    test_agent_preflight_missing_binary_throws_install_hint();
    test_agent_result_file_capped_at_max_output_bytes();

    test_factory_dispatches_by_kind();
    test_parse_reviewer_kind();

    std::cerr << "reviewer tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
