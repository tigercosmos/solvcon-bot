// Tests for src/config.* — Config::from_env. We exercise the env-var
// matrix in plan.md §8 and the validation rules added in the M1 codex
// pass. Each test sets only the env vars it needs and restores the
// environment afterwards.

#include "config.hpp"

#include <cstdlib>
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

using modmesh_bot::Config;

// All the env vars Config::from_env reads. Tests scrub all of them
// before each scenario so previous tests can't leak state.
const char * const k_all_vars[] = {
    "GITHUB_TOKEN", "GITHUB_REPO", "BOT_HANDLE", "REVIEWER_ARGV",
    "POLL_INTERVAL_SEC", "STATE_FILE",
    "MAX_DIFF_BYTES", "MAX_OUTPUT_BYTES", "SUBPROCESS_TIMEOUT_SEC",
    "HTTP_CONNECT_TIMEOUT_SEC", "HTTP_READ_TIMEOUT_SEC", "HTTP_WRITE_TIMEOUT_SEC",
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
    ::setenv("REVIEWER_ARGV", R"(["claude","-p"])", 1);
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

// --- happy path ---------------------------------------------------------

void test_required_only()
{
    clear_env();
    set_required_defaults();
    Config c = Config::from_env();
    EXPECT_EQ(c.github_token, std::string("tok"));
    EXPECT_EQ(c.github_owner, std::string("owner"));
    EXPECT_EQ(c.github_repo, std::string("repo"));
    EXPECT_EQ(c.bot_handle, std::string("the-bot"));
    EXPECT_EQ(c.reviewer_argv.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(c.reviewer_argv[0], std::string("claude"));
    EXPECT_EQ(c.reviewer_argv[1], std::string("-p"));

    // Defaults from plan §8.
    EXPECT_EQ(c.poll_interval_sec, 30);
    EXPECT_EQ(c.state_file, std::string("./modmesh-bot.state"));
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

void test_reviewer_argv_single_element()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_ARGV", R"(["only"])", 1);
    Config c = Config::from_env();
    EXPECT_EQ(c.reviewer_argv.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(c.reviewer_argv[0], std::string("only"));
}

// --- required-missing ---------------------------------------------------

void test_missing_token()
{
    clear_env();
    ::setenv("GITHUB_REPO", "o/r", 1);
    ::setenv("BOT_HANDLE", "b", 1);
    ::setenv("REVIEWER_ARGV", R"(["x"])", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "GITHUB_TOKEN"));
}

void test_missing_repo()
{
    clear_env();
    ::setenv("GITHUB_TOKEN", "t", 1);
    ::setenv("BOT_HANDLE", "b", 1);
    ::setenv("REVIEWER_ARGV", R"(["x"])", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "GITHUB_REPO"));
}

void test_missing_bot_handle()
{
    clear_env();
    ::setenv("GITHUB_TOKEN", "t", 1);
    ::setenv("GITHUB_REPO", "o/r", 1);
    ::setenv("REVIEWER_ARGV", R"(["x"])", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "BOT_HANDLE"));
}

void test_missing_reviewer_argv()
{
    clear_env();
    ::setenv("GITHUB_TOKEN", "t", 1);
    ::setenv("GITHUB_REPO", "o/r", 1);
    ::setenv("BOT_HANDLE", "b", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "REVIEWER_ARGV"));
}

// Empty string variants are treated as unset.
void test_empty_required_var_treated_as_missing()
{
    clear_env();
    ::setenv("GITHUB_TOKEN", "", 1);
    ::setenv("GITHUB_REPO", "o/r", 1);
    ::setenv("BOT_HANDLE", "b", 1);
    ::setenv("REVIEWER_ARGV", R"(["x"])", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "GITHUB_TOKEN"));
}

// --- GITHUB_REPO parsing -----------------------------------------------

void test_repo_no_slash_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("GITHUB_REPO", "missingslash", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "owner/repo"));
}

void test_repo_too_many_slashes_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("GITHUB_REPO", "a/b/c", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "extra slashes"));
}

void test_repo_empty_owner_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("GITHUB_REPO", "/repo", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "owner/repo"));
}

void test_repo_empty_name_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("GITHUB_REPO", "owner/", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "owner/repo"));
}

// --- REVIEWER_ARGV JSON parsing ----------------------------------------

void test_argv_not_an_array_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_ARGV", R"("claude")", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "missing opening bracket"));
}

void test_argv_empty_array_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("REVIEWER_ARGV", "[]", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "non-empty"));
}

// --- numeric env parsing -----------------------------------------------

void test_poll_interval_zero_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("POLL_INTERVAL_SEC", "0", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "POLL_INTERVAL_SEC"));
}

void test_poll_interval_negative_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("POLL_INTERVAL_SEC", "-30", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "non-negative"));
}

void test_poll_interval_trailing_garbage_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("POLL_INTERVAL_SEC", "30s", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "trailing garbage"));
}

void test_poll_interval_leading_garbage_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("POLL_INTERVAL_SEC", "x30", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "not an integer"));
}

void test_max_diff_bytes_negative_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("MAX_DIFF_BYTES", "-1", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "non-negative"));
}

void test_max_diff_bytes_zero_rejected()
{
    clear_env();
    set_required_defaults();
    ::setenv("MAX_DIFF_BYTES", "0", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "MAX_DIFF_BYTES"));
}

void test_poll_interval_too_large_rejected()
{
    clear_env();
    set_required_defaults();
    // Larger than 86400 (one day) cap.
    ::setenv("POLL_INTERVAL_SEC", "100000", 1);
    EXPECT(throws_with_substring([] { (void)Config::from_env(); },
                                  "POLL_INTERVAL_SEC"));
}

} // namespace

int main()
{
    test_required_only();
    test_overrides_applied();
    test_reviewer_argv_single_element();

    test_missing_token();
    test_missing_repo();
    test_missing_bot_handle();
    test_missing_reviewer_argv();
    test_empty_required_var_treated_as_missing();

    test_repo_no_slash_rejected();
    test_repo_too_many_slashes_rejected();
    test_repo_empty_owner_rejected();
    test_repo_empty_name_rejected();

    test_argv_not_an_array_rejected();
    test_argv_empty_array_rejected();

    test_poll_interval_zero_rejected();
    test_poll_interval_negative_rejected();
    test_poll_interval_trailing_garbage_rejected();
    test_poll_interval_leading_garbage_rejected();
    test_max_diff_bytes_negative_rejected();
    test_max_diff_bytes_zero_rejected();
    test_poll_interval_too_large_rejected();

    clear_env();
    std::cerr << "config tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
