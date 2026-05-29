// Tests for src/subprocess.* using small POSIX utilities. We rely on
// /bin/cat, /bin/sh, /usr/bin/env, /bin/sleep, /bin/false, and a fake
// nonexistent path for spawn-failure coverage.

#include "subprocess.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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
                      << ": " << #a << " == " << #b << "\n  got: <" << _a    \
                      << ">\n  want: <" << _b << ">\n";                      \
            ++g_failed;                                                      \
        }                                                                    \
        else { ++g_passed; }                                                 \
    } while (0)

using modmesh_bot::run_subprocess;
using modmesh_bot::RunResult;

void test_cat_echo()
{
    RunResult r = run_subprocess({"/bin/cat"}, "hello, world\n", 65536, 5);
    EXPECT_EQ(r.exit_status, 0);
    EXPECT(!r.timed_out);
    EXPECT(!r.stdout_truncated);
    EXPECT_EQ(r.stdout_buf, std::string("hello, world\n"));
    EXPECT_EQ(r.stderr_buf, std::string());
}

void test_empty_stdin()
{
    RunResult r = run_subprocess({"/bin/cat"}, "", 65536, 5);
    EXPECT_EQ(r.exit_status, 0);
    EXPECT_EQ(r.stdout_buf, std::string());
}

void test_stdout_truncation()
{
    // Generate 100 KB of input through cat with a 4 KB cap. cat may exit
    // non-zero (SIGPIPE) when we close the read end after the cap; the
    // contract being tested is that the cap is honored, not the exit code.
    const std::string input(100 * 1024, 'A');
    RunResult r = run_subprocess({"/bin/cat"}, input, 4096, 10);
    EXPECT(!r.timed_out);
    EXPECT(r.stdout_truncated);
    EXPECT_EQ(r.stdout_buf.substr(0, 4096), std::string(4096, 'A'));
    EXPECT(r.stdout_buf.size() > 4096); // footer present
}

void test_nonzero_exit()
{
    // /bin/false isn't present on macOS (it lives at /usr/bin/false); use
    // a portable shell invocation.
    RunResult r = run_subprocess(
        {"/bin/sh", "-c", "exit 1"}, "", 4096, 5);
    EXPECT_EQ(r.exit_status, 1);
    EXPECT(!r.timed_out);
}

void test_timeout_kills_long_sleep()
{
    auto t0 = std::chrono::steady_clock::now();
    RunResult r = run_subprocess({"/bin/sleep", "10"}, "", 4096, 1);
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT(r.timed_out);
    EXPECT(r.exit_status == -1);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT(ms < 3000); // killed promptly (timeout + grace ≪ 10s sleep)
}

void test_spawn_failure_for_missing_binary()
{
    RunResult r = run_subprocess(
        {"/no/such/binary-modmesh-bot-test"}, "", 4096, 5);
    // execvp failure in the child → exit 127.
    EXPECT_EQ(r.exit_status, 127);
    EXPECT(!r.stderr_buf.empty()); // our "execvp failed" line
}

void test_stderr_captured_separately()
{
    // /bin/sh is a shell, fine to use inside tests; the production-side
    // ban is on the bot using a shell to interpret REVIEWER_ARGV.
    RunResult r = run_subprocess(
        {"/bin/sh", "-c", "echo out; echo err 1>&2"},
        "", 4096, 5);
    EXPECT_EQ(r.exit_status, 0);
    EXPECT_EQ(r.stdout_buf, std::string("out\n"));
    EXPECT_EQ(r.stderr_buf, std::string("err\n"));
}

void test_sanitized_env_no_github_token()
{
    // Set a fake GITHUB_TOKEN in the parent. The child must NOT inherit it.
    ::setenv("GITHUB_TOKEN", "secret-do-not-leak", /*overwrite=*/1);

    RunResult r = run_subprocess({"/usr/bin/env"}, "", 65536, 5);
    EXPECT_EQ(r.exit_status, 0);
    EXPECT(r.stdout_buf.find("GITHUB_TOKEN") == std::string::npos);
    EXPECT(r.stdout_buf.find("secret-do-not-leak") == std::string::npos);

    ::unsetenv("GITHUB_TOKEN");
}

void test_sanitized_env_keeps_path()
{
    // PATH must survive — the child needs it to find /usr/bin/env when
    // resolution happens, and so AI CLIs find their own helpers.
    const char * parent_path = std::getenv("PATH");
    EXPECT(parent_path != nullptr);
    if (parent_path == nullptr) return;

    RunResult r = run_subprocess({"/usr/bin/env"}, "", 65536, 5);
    EXPECT_EQ(r.exit_status, 0);
    const std::string needle = std::string("PATH=") + parent_path;
    EXPECT(r.stdout_buf.find(needle) != std::string::npos);
}

void test_large_stdin_round_trip()
{
    // 200 KB through cat. Stresses POLLOUT/POLLIN interleave.
    const std::string input(200 * 1024, 'x');
    RunResult r = run_subprocess({"/bin/cat"}, input, 1024 * 1024, 30);
    if (r.exit_status != 0)
    {
        std::cerr << "  cat exit=" << r.exit_status
                  << " stderr=<" << r.stderr_buf << ">"
                  << " stdout_size=" << r.stdout_buf.size() << "\n";
    }
    EXPECT_EQ(r.exit_status, 0);
    EXPECT(!r.stdout_truncated);
    EXPECT_EQ(r.stdout_buf.size(), input.size());
}

} // namespace

int main()
{
    test_cat_echo();
    test_empty_stdin();
    test_stdout_truncation();
    test_nonzero_exit();
    test_timeout_kills_long_sleep();
    test_spawn_failure_for_missing_binary();
    test_stderr_captured_separately();
    test_sanitized_env_no_github_token();
    test_sanitized_env_keeps_path();
    test_large_stdin_round_trip();

    std::cerr << "subprocess tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
