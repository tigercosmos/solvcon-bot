// Tests for src/reviewer.* — the IReviewer factory plus MockReviewer,
// ClaudeReviewer, and CodexReviewer. The two AI-backed reviewers are
// tested via their build_invocation* test-only helpers so we don't
// spawn the real CLI here; the live spawn is covered in scripts/e2e_*.

#include "config.hpp"
#include "reviewer.hpp"

#include <iostream>
#include <string>

namespace modmesh_bot
{
// Test-only handles defined in reviewer_claude.cpp / reviewer_codex.cpp.
ReviewerInvocation claude_build_invocation_for_test(
    const Config & cfg, const std::string & diff);
ReviewerInvocation codex_build_invocation_for_test(
    const Config & cfg, const std::string & diff);
} // namespace modmesh_bot

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

using modmesh_bot::Config;
using modmesh_bot::IReviewer;
using modmesh_bot::ReviewerError;
using modmesh_bot::ReviewerKind;

namespace mb = modmesh_bot;

Config make_cfg(ReviewerKind kind)
{
    Config c;
    c.reviewer_kind = kind;
    c.max_output_bytes = 8192;
    c.subprocess_timeout_sec = 5;
    return c;
}

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
    // Diff without trailing newline -> we add one so END_DIFF lands on
    // its own line.
    const std::string out = mb::assemble_review_stdin("P", "X");
    EXPECT(out.find("X\nEND_DIFF\n") != std::string::npos);
}

void test_assemble_review_stdin_empty_diff()
{
    // Empty diff: no extra blank line between BEGIN_DIFF and END_DIFF.
    const std::string out = mb::assemble_review_stdin("P", "");
    EXPECT(out == std::string("P\n\nBEGIN_DIFF\nEND_DIFF\n"));
}

void test_assemble_review_stdin_diff_already_newline_terminated()
{
    // Diff that already ends with \n: don't add a second one.
    const std::string out = mb::assemble_review_stdin("P", "X\n");
    EXPECT(out.find("X\n\nEND_DIFF") == std::string::npos);
    EXPECT(out.find("X\nEND_DIFF") != std::string::npos);
}

void test_truncation_note_appended_only_when_truncated()
{
    EXPECT_EQ(mb::maybe_append_truncation_note("hi", false), std::string("hi"));
    const std::string out = mb::maybe_append_truncation_note("hi", true);
    EXPECT(out.find("modmesh-bot: reviewer output truncated") != std::string::npos);
    EXPECT(out.find("hi") == 0);
}

// --- MockReviewer (happy echo) -------------------------------------------

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

// --- ClaudeReviewer (introspect only; no spawn) --------------------------

void test_claude_invocation_minimal_uses_defaults()
{
    // Empty REVIEWER_MODEL + REVIEWER_EFFORT must inject the class
    // defaults: claude-opus-4-8 + high. The bot ships an explicit
    // model/effort to claude on EVERY invocation; the operator opts
    // OUT by setting these env vars to a different value.
    Config c = make_cfg(ReviewerKind::Claude);
    auto inv = mb::claude_build_invocation_for_test(c, "diff bytes");
    // argv: claude -p --model claude-opus-4-8
    EXPECT_EQ(inv.argv.size(), static_cast<std::size_t>(4));
    EXPECT_EQ(inv.argv[0], std::string("claude"));
    EXPECT_EQ(inv.argv[1], std::string("-p"));
    EXPECT_EQ(inv.argv[2], std::string("--model"));
    EXPECT_EQ(inv.argv[3], std::string("claude-opus-4-8"));
    // stdin includes the default prompt + diff block
    EXPECT(inv.stdin_input.find(mb::default_review_prompt()) == 0);
    EXPECT(inv.stdin_input.find("diff bytes") != std::string::npos);
    // CLAUDE_EFFORT defaults to high.
    EXPECT_EQ(inv.env_values.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(inv.env_values[0].first, std::string("CLAUDE_EFFORT"));
    EXPECT_EQ(inv.env_values[0].second, std::string("high"));
}

void test_claude_invocation_model_override()
{
    Config c = make_cfg(ReviewerKind::Claude);
    c.reviewer_model = "claude-sonnet-4-6";
    auto inv = mb::claude_build_invocation_for_test(c, "x");
    EXPECT_EQ(inv.argv.size(), static_cast<std::size_t>(4));
    EXPECT_EQ(inv.argv[2], std::string("--model"));
    EXPECT_EQ(inv.argv[3], std::string("claude-sonnet-4-6"));
    // Effort still defaults.
    EXPECT_EQ(inv.env_values.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(inv.env_values[0].second, std::string("high"));
}

void test_claude_effort_override()
{
    Config c = make_cfg(ReviewerKind::Claude);
    c.reviewer_effort = "xhigh";
    auto inv = mb::claude_build_invocation_for_test(c, "x");
    EXPECT_EQ(inv.env_values.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(inv.env_values[0].first, std::string("CLAUDE_EFFORT"));
    EXPECT_EQ(inv.env_values[0].second, std::string("xhigh"));
    // Model still defaults.
    EXPECT_EQ(inv.argv[3], std::string("claude-opus-4-8"));
}

void test_claude_custom_prompt_replaces_default()
{
    Config c = make_cfg(ReviewerKind::Claude);
    c.reviewer_prompt = "SPECIAL PROMPT";
    auto inv = mb::claude_build_invocation_for_test(c, "x");
    EXPECT(inv.stdin_input.find("SPECIAL PROMPT") == 0);
    EXPECT(inv.stdin_input.find(mb::default_review_prompt()) == std::string::npos);
}

void test_claude_env_passthrough_threaded()
{
    Config c = make_cfg(ReviewerKind::Claude);
    c.reviewer_env_passthrough = {"ANTHROPIC_API_KEY"};
    auto inv = mb::claude_build_invocation_for_test(c, "x");
    EXPECT_EQ(inv.env_passthrough.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(inv.env_passthrough[0], std::string("ANTHROPIC_API_KEY"));
}

// --- CodexReviewer (introspect only) -------------------------------------

void test_codex_invocation_minimal_uses_defaults()
{
    // Empty REVIEWER_MODEL + REVIEWER_EFFORT must inject the class
    // defaults: gpt-5.5 + high.
    Config c = make_cfg(ReviewerKind::Codex);
    auto inv = mb::codex_build_invocation_for_test(c, "diff bytes");
    EXPECT_EQ(inv.argv.size(), static_cast<std::size_t>(6));
    EXPECT_EQ(inv.argv[0], std::string("codex"));
    EXPECT_EQ(inv.argv[1], std::string("exec"));
    EXPECT_EQ(inv.argv[2], std::string("--model"));
    EXPECT_EQ(inv.argv[3], std::string("gpt-5.5"));
    EXPECT_EQ(inv.argv[4], std::string("-c"));
    EXPECT_EQ(inv.argv[5], std::string("reasoning.effort=high"));
    EXPECT(inv.stdin_input.find("BEGIN_DIFF") != std::string::npos);
    // codex receives effort via argv, NOT as an env var.
    EXPECT(inv.env_values.empty());
}

void test_codex_invocation_overrides()
{
    Config c = make_cfg(ReviewerKind::Codex);
    c.reviewer_model = "gpt-5";
    c.reviewer_effort = "xhigh";
    auto inv = mb::codex_build_invocation_for_test(c, "x");
    EXPECT_EQ(inv.argv.size(), static_cast<std::size_t>(6));
    EXPECT_EQ(inv.argv[3], std::string("gpt-5"));
    EXPECT_EQ(inv.argv[5], std::string("reasoning.effort=xhigh"));
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
}

void test_parse_reviewer_kind()
{
    EXPECT_EQ(mb::parse_reviewer_kind("mock"), ReviewerKind::Mock);
    EXPECT_EQ(mb::parse_reviewer_kind("MOCK"), ReviewerKind::Mock);
    EXPECT_EQ(mb::parse_reviewer_kind("Claude"), ReviewerKind::Claude);
    EXPECT_EQ(mb::parse_reviewer_kind("codex"), ReviewerKind::Codex);
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
    test_assemble_review_stdin_diff_already_newline_terminated();
    test_truncation_note_appended_only_when_truncated();

    test_mock_echoes_diff_by_default();
    test_mock_fixed_output_overrides_diff();
    test_mock_forced_failure_throws();

    test_claude_invocation_minimal_uses_defaults();
    test_claude_invocation_model_override();
    test_claude_effort_override();
    test_claude_custom_prompt_replaces_default();
    test_claude_env_passthrough_threaded();

    test_codex_invocation_minimal_uses_defaults();
    test_codex_invocation_overrides();

    test_factory_dispatches_by_kind();
    test_parse_reviewer_kind();

    std::cerr << "reviewer tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
