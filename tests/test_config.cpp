// Tests for src/config.* — Config::from_env. We exercise the env-var
// matrix in plan.md §8 and the validation rules added in the M1 codex
// pass. Each test sets only the env vars it needs and restores the
// environment afterwards.

#include "config.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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
using solvcon_bot::ReviewerKind;

const char * const k_all_vars[] = {
    "GITHUB_TOKEN", "GITHUB_REPO", "BOT_HANDLE",
    "POLL_INTERVAL_SEC", "STATE_FILE",
    "MAX_DIFF_BYTES", "MAX_OUTPUT_BYTES", "SUBPROCESS_TIMEOUT_SEC",
    "HTTP_CONNECT_TIMEOUT_SEC", "HTTP_READ_TIMEOUT_SEC", "HTTP_WRITE_TIMEOUT_SEC",
    "REVIEWER_ENV_PASSTHROUGH",
    "REVIEWER_KIND", "REVIEWER_MODEL", "REVIEWER_EFFORT",
    "REVIEWER_PROMPT", "REVIEWER_PROMPT_FILE",
    "REVIEWER_MOCK_EXIT_CODE", "REVIEWER_MOCK_OUTPUT",
    "REVIEWER_STREAM_IO", "REVIEWER_HEARTBEAT_SEC",
    "GITHUB_API_BASE_URL",
};

void clear_env()
{
    for (const char * v : k_all_vars) ::unsetenv(v);
}

void set_required_defaults()
{
    ::setenv("GITHUB_TOKEN", "tok", 1);
    ::setenv("GITHUB_REPO", "owner/repo", 1);
    ::setenv("BOT_HANDLE", "the-bot", 1);
}

bool throws_with_substring(void (*fn)(), const std::string & needle)
{
    try { fn(); }
    catch (const std::exception & e)
    {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

// --- happy path ----------------------------------------------------------

void test_required_only_defaults_to_mock()
{
    clear_env();
    set_required_defaults();
    Config c = Config::from_env();
    EXPECT_EQ(c.github_token, std::string("tok"));
    EXPECT_EQ(c.github_owner, std::string("owner"));
    EXPECT_EQ(c.github_repo, std::string("repo"));
    EXPECT_EQ(c.bot_handle, std::string("the-bot"));

    // Reviewer defaults: Mock, no model/effort/prompt, mock knobs zero.
    EXPECT_EQ(c.reviewer_kind, ReviewerKind::Mock);
    EXPECT(c.reviewer_model.empty());
    EXPECT(c.reviewer_effort.empty());
    EXPECT(c.reviewer_prompt.empty());
    EXPECT_EQ(c.reviewer_mock_exit_code, 0);
    EXPECT(c.reviewer_mock_output.empty());

    // Numeric defaults from plan.md §8.
    EXPECT_EQ(c.poll_interval_sec, 30);
    EXPECT_EQ(c.state_file, std::string("./solvcon-bot.state"));
    EXPECT_EQ(c.max_diff_bytes, static_cast<std::size_t>(200000));
    EXPECT_EQ(c.max_output_bytes, static_cast<std::size_t>(60000));
    EXPECT_EQ(c.subprocess_timeout_sec, 300);
    EXPECT_EQ(c.http_connect_timeout_sec, 10);
    EXPECT_EQ(c.http_read_timeout_sec, 30);
    EXPECT_EQ(c.http_write_timeout_sec, 30);
}

void test_overrides_applied()
{
    clear_env();
    set_required_defaults();
    ::setenv("POLL_INTERVAL_SEC", "5", 1);
    ::setenv("STATE_FILE", "/tmp/foo.state", 1);
    ::setenv("MAX_DIFF_BYTES", "12345", 1);
    ::setenv("MAX_OUTPUT_BYTES", "9999", 1);
    ::setenv("SUBPROCESS_TIMEOUT_SEC", "60", 1);
    ::setenv("HTTP_CONNECT_TIMEOUT_SEC", "1", 1);
    ::setenv("HTTP_READ_TIMEOUT_SEC", "2", 1);
    ::setenv("HTTP_WRITE_TIMEOUT_SEC", "3", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.poll_interval_sec, 5);
    EXPECT_EQ(c.state_file, std::string("/tmp/foo.state"));
    EXPECT_EQ(c.max_diff_bytes, static_cast<std::size_t>(12345));
    EXPECT_EQ(c.max_output_bytes, static_cast<std::size_t>(9999));
    EXPECT_EQ(c.subprocess_timeout_sec, 60);
    EXPECT_EQ(c.http_connect_timeout_sec, 1);
    EXPECT_EQ(c.http_read_timeout_sec, 2);
    EXPECT_EQ(c.http_write_timeout_sec, 3);
}

// --- required-missing ---------------------------------------------------

void test_missing_token()
{
    clear_env();
    ::setenv("GITHUB_REPO", "o/r", 1);
    ::setenv("BOT_HANDLE", "b", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "GITHUB_TOKEN"));
}

void test_missing_repo()
{
    clear_env();
    ::setenv("GITHUB_TOKEN", "t", 1);
    ::setenv("BOT_HANDLE", "b", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "GITHUB_REPO"));
}

void test_missing_bot_handle()
{
    clear_env();
    ::setenv("GITHUB_TOKEN", "t", 1);
    ::setenv("GITHUB_REPO", "o/r", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "BOT_HANDLE"));
}

void test_empty_required_var_treated_as_missing()
{
    clear_env();
    ::setenv("GITHUB_TOKEN", "", 1);
    ::setenv("GITHUB_REPO", "o/r", 1);
    ::setenv("BOT_HANDLE", "b", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "GITHUB_TOKEN"));
}

// --- GITHUB_REPO parsing ------------------------------------------------

void test_repo_no_slash_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("GITHUB_REPO", "missingslash", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "owner/repo"));
}

void test_repo_too_many_slashes_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("GITHUB_REPO", "a/b/c", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "extra slashes"));
}

void test_repo_empty_owner_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("GITHUB_REPO", "/repo", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "owner/repo"));
}

void test_repo_empty_name_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("GITHUB_REPO", "owner/", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "owner/repo"));
}

// --- REVIEWER_KIND / model / effort / prompt ----------------------------

void test_reviewer_kind_claude()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_KIND", "claude", 1);
    ::setenv("REVIEWER_MODEL", "claude-opus-4-7", 1);
    ::setenv("REVIEWER_EFFORT", "xhigh", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_kind, ReviewerKind::Claude);
    EXPECT_EQ(c.reviewer_model, std::string("claude-opus-4-7"));
    EXPECT_EQ(c.reviewer_effort, std::string("xhigh"));
}

void test_reviewer_kind_codex_uppercase()
{
    // Kind parsing is case-insensitive.
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_KIND", "CODEX", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_kind, ReviewerKind::Codex);
}

void test_reviewer_kind_invalid_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_KIND", "gemini", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "REVIEWER_KIND"));
}

void test_reviewer_prompt_literal()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_PROMPT", "review please", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_prompt, std::string("review please"));
}

void test_reviewer_prompt_from_file()
{
    char path_buf[] = "/tmp/solvcon-bot-prompt-XXXXXX";
    int fd = ::mkstemp(path_buf);
    EXPECT(fd >= 0);
    if (fd < 0) return;
    const char body[] = "file-based prompt body\n";
    (void)!::write(fd, body, sizeof(body) - 1);
    ::close(fd);

    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_PROMPT_FILE", path_buf, 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_prompt, std::string(body));

    ::unlink(path_buf);
}

void test_reviewer_prompt_and_prompt_file_mutually_exclusive()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_PROMPT", "literal", 1);
    ::setenv("REVIEWER_PROMPT_FILE", "/nonexistent", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "mutually exclusive"));
}

void test_reviewer_prompt_file_missing()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_PROMPT_FILE", "/no/such/file-solvcon-bot-test", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "REVIEWER_PROMPT_FILE"));
}

void test_reviewer_prompt_file_too_large()
{
    // Build a 300 KB file; cap is 256 KB.
    char path_buf[] = "/tmp/solvcon-bot-prompt-XXXXXX";
    int fd = ::mkstemp(path_buf);
    EXPECT(fd >= 0);
    if (fd < 0) return;
    const std::string blob(300 * 1024, 'A');
    (void)!::write(fd, blob.data(), blob.size());
    ::close(fd);

    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_PROMPT_FILE", path_buf, 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "exceeds the"));

    ::unlink(path_buf);
}

void test_reviewer_argv_rejected_with_migration_message()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_ARGV", R"(["/bin/cat"])", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "REVIEWER_ARGV has been removed"));
    ::unsetenv("REVIEWER_ARGV");
}

void test_reviewer_effort_invalid_chars_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_EFFORT", "high; rm -rf /", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "REVIEWER_EFFORT"));
}

void test_reviewer_effort_accepts_alphanumeric_dash()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_EFFORT", "xhigh-v2", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_effort, std::string("xhigh-v2"));
}

// --- REVIEWER_MOCK_* ----------------------------------------------------

void test_reviewer_mock_exit_code()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_KIND", "mock", 1);
    ::setenv("REVIEWER_MOCK_EXIT_CODE", "17", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_mock_exit_code, 17);
}

void test_reviewer_mock_exit_code_too_large_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_MOCK_EXIT_CODE", "256", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "REVIEWER_MOCK_EXIT_CODE"));
}

void test_reviewer_mock_output()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_MOCK_OUTPUT", "fixed review text", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_mock_output, std::string("fixed review text"));
}

// --- diagnostics knobs --------------------------------------------------

void test_reviewer_stream_io_on()
{
    clear_env();
    set_required_defaults();
    for (const char * val : {"1", "true", "yes", "ON"})
    {
        ::setenv("REVIEWER_STREAM_IO", val, 1);
        Config c = Config::from_env();
        EXPECT(c.reviewer_stream_io);
    }
}

void test_reviewer_stream_io_off()
{
    clear_env();
    set_required_defaults();
    for (const char * val : {"0", "false", "no", "off"})
    {
        ::setenv("REVIEWER_STREAM_IO", val, 1);
        Config c = Config::from_env();
        EXPECT(!c.reviewer_stream_io);
    }
}

void test_reviewer_stream_io_invalid_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_STREAM_IO", "maybe", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "REVIEWER_STREAM_IO"));
}

void test_reviewer_stream_io_falsy_clears_preset_true()
{
    // Regression: apply_reviewer_env's REVIEWER_STREAM_IO branch used to
    // be write-only-when-truthy, so a caller (e.g. run-reviewer setting
    // its own tool-default) couldn't disable it via env. Now the falsy
    // branch is a real setter.
    clear_env();
    set_required_defaults();
    for (const char * val : {"0", "false", "no", "off"})
    {
        ::setenv("REVIEWER_STREAM_IO", val, 1);
        Config c;
        c.reviewer_stream_io = true;
        apply_reviewer_env(c);
        EXPECT(!c.reviewer_stream_io);
    }
}

void test_reviewer_heartbeat_sec()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_HEARTBEAT_SEC", "15", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_heartbeat_sec, 15);
}

void test_reviewer_heartbeat_sec_zero_means_off()
{
    clear_env();
    set_required_defaults();
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_heartbeat_sec, 0);
}

void test_reviewer_heartbeat_sec_out_of_range()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_HEARTBEAT_SEC", "5000", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "REVIEWER_HEARTBEAT_SEC"));
}

// --- numeric env parsing ------------------------------------------------

void test_poll_interval_zero_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("POLL_INTERVAL_SEC", "0", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "POLL_INTERVAL_SEC"));
}

void test_poll_interval_negative_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("POLL_INTERVAL_SEC", "-30", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "non-negative"));
}

void test_poll_interval_trailing_garbage_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("POLL_INTERVAL_SEC", "30s", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "trailing garbage"));
}

void test_poll_interval_leading_garbage_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("POLL_INTERVAL_SEC", "x30", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "not an integer"));
}

void test_max_diff_bytes_negative_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("MAX_DIFF_BYTES", "-1", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "non-negative"));
}

void test_max_diff_bytes_zero_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("MAX_DIFF_BYTES", "0", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "MAX_DIFF_BYTES"));
}

void test_poll_interval_too_large_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("POLL_INTERVAL_SEC", "100000", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); }, "POLL_INTERVAL_SEC"));
}

// --- timeout upper bounds -----------------------------------------------
//
// SUBPROCESS_TIMEOUT_SEC is capped at 86400 because reviewer_agent.cpp
// computes `subprocess_timeout_sec + grace` as int; an INT_MAX-ish
// value would overflow (UB). The HTTP timeouts are capped at 3600 for
// plain sanity. Both keep a minimum of 1.

void test_subprocess_timeout_accepts_max()
{
    clear_env();
    set_required_defaults();
    ::setenv("SUBPROCESS_TIMEOUT_SEC", "86400", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.subprocess_timeout_sec, 86400);
}

void test_subprocess_timeout_above_max_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("SUBPROCESS_TIMEOUT_SEC", "86401", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "SUBPROCESS_TIMEOUT_SEC"));
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "out of allowed range"));
}

void test_subprocess_timeout_int_max_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("SUBPROCESS_TIMEOUT_SEC", "2147483647", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "SUBPROCESS_TIMEOUT_SEC"));
}

void test_subprocess_timeout_zero_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("SUBPROCESS_TIMEOUT_SEC", "0", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "SUBPROCESS_TIMEOUT_SEC"));
}

void test_subprocess_timeout_capped_in_apply_reviewer_env_too()
{
    // run-reviewer never calls Config::from_env(); it goes straight to
    // apply_reviewer_env, which reads SUBPROCESS_TIMEOUT_SEC as well.
    // The cap has to hold on that path too.
    clear_env();
    ::setenv("SUBPROCESS_TIMEOUT_SEC", "86401", 1);
    EXPECT(throws_with_substring([]
                                 {
                                     Config c;
                                     apply_reviewer_env(c);
                                 },
                                 "SUBPROCESS_TIMEOUT_SEC"));

    ::setenv("SUBPROCESS_TIMEOUT_SEC", "86400", 1);
    Config c;
    apply_reviewer_env(c);
    EXPECT_EQ(c.subprocess_timeout_sec, 86400);
}

void test_http_timeouts_accept_max()
{
    clear_env();
    set_required_defaults();
    ::setenv("HTTP_CONNECT_TIMEOUT_SEC", "3600", 1);
    ::setenv("HTTP_READ_TIMEOUT_SEC", "3600", 1);
    ::setenv("HTTP_WRITE_TIMEOUT_SEC", "3600", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.http_connect_timeout_sec, 3600);
    EXPECT_EQ(c.http_read_timeout_sec, 3600);
    EXPECT_EQ(c.http_write_timeout_sec, 3600);
}

void test_http_connect_timeout_above_max_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("HTTP_CONNECT_TIMEOUT_SEC", "3601", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "HTTP_CONNECT_TIMEOUT_SEC"));
}

void test_http_read_timeout_int_max_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("HTTP_READ_TIMEOUT_SEC", "2147483647", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "HTTP_READ_TIMEOUT_SEC"));
}

void test_http_write_timeout_above_max_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("HTTP_WRITE_TIMEOUT_SEC", "3601", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "HTTP_WRITE_TIMEOUT_SEC"));
}

void test_http_timeout_zero_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("HTTP_READ_TIMEOUT_SEC", "0", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "HTTP_READ_TIMEOUT_SEC"));
}

// --- REVIEWER_ENV_PASSTHROUGH parsing -----------------------------------

void test_reviewer_env_passthrough_default_empty()
{
    clear_env();
    set_required_defaults();
    Config c = Config::from_env();
    EXPECT(c.reviewer_env_passthrough.empty());
}

void test_reviewer_env_passthrough_csv()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_ENV_PASSTHROUGH",
             "ANTHROPIC_API_KEY,OPENAI_API_KEY,SOME_OTHER", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_env_passthrough.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(c.reviewer_env_passthrough[0], std::string("ANTHROPIC_API_KEY"));
    EXPECT_EQ(c.reviewer_env_passthrough[1], std::string("OPENAI_API_KEY"));
    EXPECT_EQ(c.reviewer_env_passthrough[2], std::string("SOME_OTHER"));
}

void test_reviewer_env_passthrough_trims_whitespace()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_ENV_PASSTHROUGH",
             "  ANTHROPIC_API_KEY  ,  OPENAI_API_KEY  ", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_env_passthrough.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(c.reviewer_env_passthrough[0], std::string("ANTHROPIC_API_KEY"));
    EXPECT_EQ(c.reviewer_env_passthrough[1], std::string("OPENAI_API_KEY"));
}

void test_reviewer_env_passthrough_drops_empty_tokens()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_ENV_PASSTHROUGH", ",,FOO,,", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_env_passthrough.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(c.reviewer_env_passthrough[0], std::string("FOO"));
}

} // namespace

int main()
{
    test_required_only_defaults_to_mock();
    test_overrides_applied();

    test_missing_token();
    test_missing_repo();
    test_missing_bot_handle();
    test_empty_required_var_treated_as_missing();

    test_repo_no_slash_rejected();
    test_repo_too_many_slashes_rejected();
    test_repo_empty_owner_rejected();
    test_repo_empty_name_rejected();

    test_reviewer_kind_claude();
    test_reviewer_kind_codex_uppercase();
    test_reviewer_kind_invalid_rejected();
    test_reviewer_prompt_literal();
    test_reviewer_prompt_from_file();
    test_reviewer_prompt_and_prompt_file_mutually_exclusive();
    test_reviewer_prompt_file_missing();
    test_reviewer_prompt_file_too_large();
    test_reviewer_argv_rejected_with_migration_message();
    test_reviewer_effort_invalid_chars_rejected();
    test_reviewer_effort_accepts_alphanumeric_dash();

    test_reviewer_mock_exit_code();
    test_reviewer_mock_exit_code_too_large_rejected();
    test_reviewer_mock_output();

    test_reviewer_stream_io_on();
    test_reviewer_stream_io_off();
    test_reviewer_stream_io_invalid_rejected();
    test_reviewer_stream_io_falsy_clears_preset_true();
    test_reviewer_heartbeat_sec();
    test_reviewer_heartbeat_sec_zero_means_off();
    test_reviewer_heartbeat_sec_out_of_range();

    test_poll_interval_zero_rejected();
    test_poll_interval_negative_rejected();
    test_poll_interval_trailing_garbage_rejected();
    test_poll_interval_leading_garbage_rejected();
    test_max_diff_bytes_negative_rejected();
    test_max_diff_bytes_zero_rejected();
    test_poll_interval_too_large_rejected();

    test_subprocess_timeout_accepts_max();
    test_subprocess_timeout_above_max_rejected();
    test_subprocess_timeout_int_max_rejected();
    test_subprocess_timeout_zero_rejected();
    test_subprocess_timeout_capped_in_apply_reviewer_env_too();
    test_http_timeouts_accept_max();
    test_http_connect_timeout_above_max_rejected();
    test_http_read_timeout_int_max_rejected();
    test_http_write_timeout_above_max_rejected();
    test_http_timeout_zero_rejected();

    test_reviewer_env_passthrough_default_empty();
    test_reviewer_env_passthrough_csv();
    test_reviewer_env_passthrough_trims_whitespace();
    test_reviewer_env_passthrough_drops_empty_tokens();

    clear_env();
    std::cerr << "config tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
