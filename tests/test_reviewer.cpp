// Tests for src/reviewer.* — the wrapper that runs the configured AI CLI
// over a diff and surfaces failures as ReviewerError.

#include "config.hpp"
#include "reviewer.hpp"

#include <iostream>
#include <string>

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
                      << ": " << #a << " == " << #b << "\n";                 \
            ++g_failed;                                                      \
        }                                                                    \
        else { ++g_passed; }                                                 \
    } while (0)

using modmesh_bot::Config;
using modmesh_bot::Reviewer;
using modmesh_bot::ReviewerError;

Config make_cfg(std::vector<std::string> argv)
{
    Config c;
    c.reviewer_argv = std::move(argv);
    c.max_output_bytes = 4096;
    c.subprocess_timeout_sec = 5;
    return c;
}

void test_success_passthrough()
{
    Reviewer r(make_cfg({"/bin/cat"}));
    EXPECT_EQ(r.run("diff content\n"), std::string("diff content\n"));
}

void test_nonzero_exit_throws()
{
    Reviewer r(make_cfg({"/bin/sh", "-c", "echo oh no 1>&2; exit 3"}));
    bool threw = false;
    std::string msg;
    try { r.run(""); }
    catch (const ReviewerError & e) { threw = true; msg = e.what(); }
    EXPECT(threw);
    EXPECT(msg.find("exited 3") != std::string::npos);
    EXPECT(msg.find("oh no") != std::string::npos);
}

void test_timeout_throws()
{
    Config c = make_cfg({"/bin/sleep", "10"});
    c.subprocess_timeout_sec = 1;
    Reviewer r(c);
    bool threw = false;
    std::string msg;
    try { r.run(""); }
    catch (const ReviewerError & e) { threw = true; msg = e.what(); }
    EXPECT(threw);
    EXPECT(msg.find("timed out") != std::string::npos);
}

void test_empty_argv_throws_reviewer_error()
{
    Reviewer r(make_cfg({}));
    bool threw_correct = false;
    try { r.run(""); }
    catch (const ReviewerError &) { threw_correct = true; }
    EXPECT(threw_correct);
}

void test_missing_binary_throws_reviewer_error()
{
    // execvp fails in the child → exit 127 → ReviewerError (not bare
    // std::runtime_error).
    Reviewer r(make_cfg({"/no/such/binary-modmesh-bot-test"}));
    bool threw_correct = false;
    try { r.run(""); }
    catch (const ReviewerError &) { threw_correct = true; }
    EXPECT(threw_correct);
}

} // namespace

int main()
{
    test_success_passthrough();
    test_nonzero_exit_throws();
    test_timeout_throws();
    test_empty_argv_throws_reviewer_error();
    test_missing_binary_throws_reviewer_error();

    std::cerr << "reviewer tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
