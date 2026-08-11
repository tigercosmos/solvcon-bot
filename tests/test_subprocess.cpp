// Tests for src/subprocess.* using small POSIX utilities. We rely on
// /bin/cat, /bin/sh, /usr/bin/env, /bin/sleep, /bin/false, and a fake
// nonexistent path for spawn-failure coverage.

#include "subprocess.hpp"

#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <csignal>
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

using solvcon_bot::run_subprocess;
using solvcon_bot::RunResult;

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

// True when this process has no unreaped children left. run_subprocess
// must always leave the tree in this state: a stranded zombie (or a
// child it never waited for) shows up here as a pid > 0.
bool no_unreaped_children()
{
    int st = 0;
    pid_t p = ::waitpid(-1, &st, WNOHANG);
    return p < 0 && errno == ECHILD;
}

void test_timeout_reaps_child_that_ignores_sigterm()
{
    // The shell traps (ignores) SIGTERM, so the timeout path's SIGTERM
    // does nothing and only the follow-up SIGKILL can end it. Exercises
    // kill-then-reap end to end: we must come back promptly, report
    // timed_out, and leave no unreaped child behind.
    auto t0 = std::chrono::steady_clock::now();
    RunResult r = run_subprocess(
        {"/bin/sh", "-c", "trap '' TERM; while :; do sleep 0.2; done"},
        "", 4096, 1);
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT(r.timed_out);
    EXPECT_EQ(r.exit_status, -1); // killed by signal
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT(ms < 3000); // 1s timeout + 200ms grace, not a hang
    EXPECT(no_unreaped_children());
}

volatile std::sig_atomic_t g_alarm_count = 0;

// Plain assignment, not ++: compound ops on volatile are deprecated in
// C++20 and this build is -Werror.
extern "C" void count_alarm(int) { g_alarm_count = g_alarm_count + 1; }

void test_waitpid_survives_eintr()
{
    // Regression: run_subprocess used to call waitpid() exactly once. Our
    // real signal handlers (main.cpp) are installed WITHOUT SA_RESTART,
    // so a signal arriving while waitpid blocks returns EINTR and the old
    // code reported exit_status=-1 while leaving the child running and
    // unreaped. Reproduce that deterministically: install an SA_RESTART-
    // less SIGALRM handler, arm a repeating 100ms timer, and run a child
    // that closes both output pipes immediately and only THEN lives for a
    // second. The poll loop therefore finishes early and the parent spends
    // ~1s parked in waitpid, where every timer tick interrupts it.
    struct sigaction sa{};
    sa.sa_handler = count_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // deliberately NOT SA_RESTART
    struct sigaction old_sa{};
    EXPECT_EQ(::sigaction(SIGALRM, &sa, &old_sa), 0);

    struct itimerval timer{};
    timer.it_value.tv_usec = 100 * 1000;
    timer.it_interval.tv_usec = 100 * 1000;
    struct itimerval old_timer{};
    EXPECT_EQ(::setitimer(ITIMER_REAL, &timer, &old_timer), 0);

    g_alarm_count = 0;
    RunResult r = run_subprocess(
        {"/bin/sh", "-c", "exec 1>/dev/null 2>/dev/null; sleep 1"},
        "", 4096, 30);

    // Disarm before asserting so a failing EXPECT is not itself
    // interrupted by the timer.
    ::setitimer(ITIMER_REAL, &old_timer, nullptr);
    ::sigaction(SIGALRM, &old_sa, nullptr);

    EXPECT(g_alarm_count > 0); // the test actually delivered signals
    EXPECT_EQ(r.exit_status, 0);
    EXPECT(!r.timed_out);
    EXPECT(no_unreaped_children());
}

void test_stdin_write_survives_eintr()
{
    // Same signal storm, but with a large stdin payload so the parent is
    // repeatedly in the write()/poll() half of the loop. A mishandled
    // EINTR on the stdin write would silently truncate what cat echoes
    // back; the round trip must stay byte-exact.
    struct sigaction sa{};
    sa.sa_handler = count_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // deliberately NOT SA_RESTART
    struct sigaction old_sa{};
    EXPECT_EQ(::sigaction(SIGALRM, &sa, &old_sa), 0);

    // 1ms ticks: the payload below takes far longer than that to shuttle
    // through a 64KB pipe, so many ticks land inside the loop.
    struct itimerval timer{};
    timer.it_value.tv_usec = 1000;
    timer.it_interval.tv_usec = 1000;
    struct itimerval old_timer{};
    EXPECT_EQ(::setitimer(ITIMER_REAL, &timer, &old_timer), 0);

    g_alarm_count = 0;
    const std::string input(4 * 1024 * 1024, 'z');
    RunResult r = run_subprocess({"/bin/cat"}, input, 16 * 1024 * 1024, 30);

    ::setitimer(ITIMER_REAL, &old_timer, nullptr);
    ::sigaction(SIGALRM, &old_sa, nullptr);

    EXPECT(g_alarm_count > 0);
    EXPECT_EQ(r.exit_status, 0);
    EXPECT(!r.stdout_truncated);
    EXPECT_EQ(r.stdout_buf.size(), input.size());
    EXPECT(no_unreaped_children());
}

void test_spawn_failure_for_missing_binary()
{
    RunResult r = run_subprocess(
        {"/no/such/binary-solvcon-bot-test"}, "", 4096, 5);
    // execvp failure in the child → exit 127.
    EXPECT_EQ(r.exit_status, 127);
    EXPECT(!r.stderr_buf.empty()); // our "execvp failed" line
}

void test_stderr_captured_separately()
{
    // /bin/sh is a shell, fine to use inside tests; the production-side
    // ban is on the bot using a shell to interpret the reviewer argv.
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

void test_default_env_passes_user_and_logname()
{
    // claude on macOS needs USER for keychain access; gh on Linux too.
    ::setenv("USER", "test-user", 1);
    ::setenv("LOGNAME", "test-user", 1);
    RunResult r = run_subprocess({"/usr/bin/env"}, "", 65536, 5);
    EXPECT_EQ(r.exit_status, 0);
    EXPECT(r.stdout_buf.find("USER=test-user") != std::string::npos);
    EXPECT(r.stdout_buf.find("LOGNAME=test-user") != std::string::npos);
}

void test_extra_env_allowlist_passes_through()
{
    // Set a credential-shaped var in parent; default allowlist drops it.
    ::setenv("SOLVCON_BOT_TEST_API_KEY", "sk-test-secret", 1);

    RunResult r_default = run_subprocess({"/usr/bin/env"}, "", 65536, 5);
    EXPECT_EQ(r_default.exit_status, 0);
    EXPECT(r_default.stdout_buf.find("SOLVCON_BOT_TEST_API_KEY") == std::string::npos);

    RunResult r_extra = run_subprocess(
        {"/usr/bin/env"}, "", 65536, 5,
        {"SOLVCON_BOT_TEST_API_KEY"});
    EXPECT_EQ(r_extra.exit_status, 0);
    EXPECT(r_extra.stdout_buf.find("SOLVCON_BOT_TEST_API_KEY=sk-test-secret") != std::string::npos);

    ::unsetenv("SOLVCON_BOT_TEST_API_KEY");
}

void test_extra_env_allowlist_drops_unset_vars()
{
    // Var is in the allowlist but not in the parent env — child should
    // not see a NAME= entry.
    ::unsetenv("SOLVCON_BOT_TEST_NOT_SET");
    RunResult r = run_subprocess(
        {"/usr/bin/env"}, "", 65536, 5,
        {"SOLVCON_BOT_TEST_NOT_SET"});
    EXPECT_EQ(r.exit_status, 0);
    EXPECT(r.stdout_buf.find("SOLVCON_BOT_TEST_NOT_SET") == std::string::npos);
}

void test_extra_env_values_set_explicit_var()
{
    // Var not in parent; explicit value is injected into child env.
    ::unsetenv("SOLVCON_BOT_TEST_EXPLICIT");
    RunResult r = run_subprocess(
        {"/usr/bin/env"}, "", 65536, 5,
        /*allowlist=*/{},
        /*values=*/{{"SOLVCON_BOT_TEST_EXPLICIT", "hello-world"}});
    EXPECT_EQ(r.exit_status, 0);
    EXPECT(r.stdout_buf.find("SOLVCON_BOT_TEST_EXPLICIT=hello-world")
           != std::string::npos);
}

void test_extra_env_values_override_passthrough()
{
    // Parent has X=parent-value; allowlist would pass it through;
    // explicit values override with "override-value".
    ::setenv("SOLVCON_BOT_TEST_OVERRIDE", "parent-value", 1);
    RunResult r = run_subprocess(
        {"/usr/bin/env"}, "", 65536, 5,
        /*allowlist=*/{"SOLVCON_BOT_TEST_OVERRIDE"},
        /*values=*/{{"SOLVCON_BOT_TEST_OVERRIDE", "override-value"}});
    EXPECT_EQ(r.exit_status, 0);
    EXPECT(r.stdout_buf.find("SOLVCON_BOT_TEST_OVERRIDE=override-value")
           != std::string::npos);
    // Make sure the parent value is NOT present.
    EXPECT(r.stdout_buf.find("SOLVCON_BOT_TEST_OVERRIDE=parent-value")
           == std::string::npos);
    ::unsetenv("SOLVCON_BOT_TEST_OVERRIDE");
}

void test_extra_env_values_overrides_empty_passthrough()
{
    // Regression for the upsert prefix-match bug: when an allowlisted
    // var has an EMPTY value in the parent ("KEY="), the override must
    // replace it, not append a duplicate. We test via a default-
    // allowlisted name (LANG) to also pin the default-list behavior.
    ::setenv("LANG", "", 1); // empty value
    RunResult r = run_subprocess(
        {"/usr/bin/env"}, "", 65536, 5,
        /*allowlist=*/{},
        /*values=*/{{"LANG", "overridden"}});
    EXPECT_EQ(r.exit_status, 0);
    // Exactly ONE LANG= line in the child env.
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = r.stdout_buf.find("LANG=", pos)) != std::string::npos)
    {
        ++count;
        pos += 5;
    }
    EXPECT_EQ(count, static_cast<std::size_t>(1));
    EXPECT(r.stdout_buf.find("LANG=overridden") != std::string::npos);
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
    test_timeout_reaps_child_that_ignores_sigterm();
    test_waitpid_survives_eintr();
    test_stdin_write_survives_eintr();
    test_spawn_failure_for_missing_binary();
    test_stderr_captured_separately();
    test_sanitized_env_no_github_token();
    test_sanitized_env_keeps_path();
    test_default_env_passes_user_and_logname();
    test_extra_env_allowlist_passes_through();
    test_extra_env_allowlist_drops_unset_vars();
    test_extra_env_values_set_explicit_var();
    test_extra_env_values_override_passthrough();
    test_extra_env_values_overrides_empty_passthrough();
    test_large_stdin_round_trip();

    std::cerr << "subprocess tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
